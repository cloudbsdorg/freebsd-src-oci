<!--
SPDX-License-Identifier: BSD-2-Clause
Copyright (c) 2026 REVYTECH, Inc.
-->
# ocifbsd Architecture

This document is the consolidated architecture reference for `ocifbsd`, the
native FreeBSD OCI runtime and tooling. It synthesizes the per-topic design
notes under `.plan/` into a single map of the system: what the components are,
how a container flows through them, and which FreeBSD primitives back each OCI
concept. For the authoritative task-level status see
[`.plan/005.0-Risks-TODO.md`](../../../.plan/005.0-Risks-TODO.md); for the
runtime-spec mapping see [`OCI-CONFORMANCE.md`](OCI-CONFORMANCE.md).

## Design goals

- **Native, not emulated.** An OCI container is a FreeBSD jail. The OCI spec
  is translated to `jail(8)`/`jail(2)` parameters; where a Linux-centric part
  of the spec has no honest FreeBSD analogue, the runtime returns a clear
  error rather than pretending (the guiding rule from `.plan/021.0`).
- **Self-contained / zero-ports.** The tree builds against the FreeBSD base
  system only. json-c and libcurl are vendored under `contrib/`; nothing under
  `/usr/local` or the ports tree is referenced. This keeps `ocifbsd` shippable
  as part of a base-system build or a cleanroom release.
- **VNET-first networking.** Container networking uses VNET with `epair(4)` and
  `if_bridge(4)` rather than shared-IP jails, so each container gets its own
  network stack.
- **Open defaults, restrict by example.** Defaults are permissive and usable;
  hardening (MAC, RCTL caps, restricted networking) is opt-in and demonstrated
  by worked configuration examples rather than imposed up front.
- **Human-facing output reads well.** JSON that a person will read
  (`inspect`, `state`, `network inspect`, …) is pretty-printed by default, with
  a `--compact` escape hatch for scripts; streaming/event output stays
  line-delimited.

## Component map

The runtime is one PIE binary, `ocifbsd`, linked from a small core plus a set
of in-tree static archives (one per subsystem). Each archive builds
independently under its own directory and is linked into the final binary.

| Area | Directory | Role |
|------|-----------|------|
| CLI + core lifecycle | `src/`, `ocifbsd.c` | Argument dispatch, container create/start/kill/delete, state persistence, OCI-spec→jail translation (`oci2jail.c`), hooks |
| OCI spec conversion | `convert/` | Compose/other manifest parsing into OCI/jail config |
| Images | `image/` | Registry client (`pull.c`), layer unpack (`unpack.c`), image store (`zfs_store.c`), push (`push.c`), load |
| Networking | `network/` | Per-container net config (`netcfg.c`) and named-network resources (`network.c`): bridge/epair lifecycle |
| Orchestration | `orchestration/` | Pods, services, stacks; compose-up semantics, scale, rolling update |
| Clustering | `clustering/` | Raft consensus, control plane, node agent, scheduler, cluster PKI/mTLS |
| Namespaces & limits | `namespace/`, `security/` | Namespace grouping; RCTL/RACCT resource limits; MAC integration |
| Credentials & auth | `pam/`, `security-daemon/`, `cert/` | PAM/JWT auth, secret encryption (OpenSSL EVP), ACME/X.509 certificate management |
| Observability | `logd/`, `metrics/` | Log forwarding + alerting; CPU/memory/load metrics |
| Lifecycle housekeeping | `gc/`, `export/` | Garbage collection; cloud export/migration |
| API surface | `api/` | HTTP API (event-loop layer) |
| Vendored deps | `contrib/` | json-c, libcurl (self-contained build) |
| Tooling | `tools/` | `ocifbsd-perf.sh`, `ocifbsd-stress.sh`, cross-build |

## Container lifecycle

The OCI runtime verbs map to jail operations:

```
ocifbsd create --name N BUNDLE   parse config.json -> jail params, persist state (status=created)
ocifbsd start  CID               apply mounts, jail_set(2) with persist, run process, hooks
ocifbsd state  CID               emit OCI state JSON (ociVersion,id,status,pid,bundle)
ocifbsd kill   CID [signal]      signal the jail's process (idempotent across created/stopped)
ocifbsd delete CID [--force]     jail_remove(2), unmount, drop persisted state
```

`oci2jail.c` performs the translation: the OCI `root.path` becomes the jail
path, `process.args`/`env`/`cwd` drive the launched process, `hostname` and
VNET/mount directives become jail parameters, and `freebsd.*` extension keys
pass FreeBSD-specific settings through. State is persisted as a directory per
container so that `state`/`list`/`delete` work across separate CLI
invocations; the OCI config is reloaded on state load so a restart sees the
original bundle.

Measured baseline (oci-e2e, FreeBSD 16-CURRENT, `tools/ocifbsd-perf.sh`,
n=20): create ~4 ms, start (jail bring-up) ~106 ms, state/kill ~3 ms, delete
~4 ms, end-to-end p50 ~120 ms. The concurrent-churn harness
(`tools/ocifbsd-stress.sh`) brings up 25 jails at once across rounds with zero
launch failures and zero leaked jails on teardown.

## Image subsystem

`ocifbsd pull <ref>` resolves a reference (Docker Hub names remap to
`registry-1.docker.io` / `library/…`), fetches the manifest and layers over
libcurl, verifies digests, and unpacks the layers — with tar path-traversal and
OCI whiteout handling — into an on-disk store at
`/var/lib/ocifbsd/<registry>/<repo>/<tag>/` (a `config.json` runtime config
plus a populated `rootfs/`). `images` walks that store for usable image roots
(a directory carrying both `config.json` and `rootfs/`); `rmi` removes an image
tree, refusing one still referenced by a container unless `--force`. `load`
imports a local OCI archive without a registry.

## Networking

Two distinct surfaces share the `network` command:

- **Per-container config** (`network list|set`, backed by `netcfg.c`) records a
  container's VNET/IP/gateway/DNS settings.
- **Named networks** (`network create|ls|rm|inspect`, backed by `network.c`)
  are Docker-style network objects. `create` builds an `if_bridge(4)` named
  `ocifbsd<name>`, assigns the gateway address to the bridge, and persists a
  JSON descriptor under `/var/run/ocifbsd/networks/`. Per-container `epair(4)`
  interfaces are created at attach time by `network_connect` and added to the
  bridge; `network_delete` tears the bridge (and its state) down. Interface and
  jail names flow into `ifconfig` only via `fork`/`exec` with an explicit argv —
  never a shell — so names containing shell metacharacters cannot inject
  commands.

## Orchestration

`pod`, `service`, and `stack` dispatch to the orchestration library. Objects
persist across CLI processes (state-as-directory). `service create` uses
compose-up semantics: it launches the replicas' backing containers and
`service delete` reaps them; `service scale` and `service update` (rolling
replacement to a new image) launch and reap replicas while persisting the new
mapping, so nothing leaks. All of these drive real containers through the
`ocifbsd_*_container` shims defined in the binary.

## Clustering

The clustering subsystem provides a control plane, a node agent, a scheduler,
and Raft-based consensus. The Raft implementation honors the §5.4.2 current-term
commit restriction (a leader only advances the commit index over entries from
its own term) with a majority of `(count + 1) / 2 + 1`. Nodes authenticate to
the cluster with mutual TLS backed by a cluster PKI.

## Security & credentials

- **Resource limits:** RCTL/RACCT caps per container/namespace (`security/`),
  applied through the real `rctl` limit fields.
- **Mandatory access control:** MAC integration (e.g. `mac_biba`,
  `mac_set_proc`) for containers that opt into confinement.
- **Authentication:** PAM service modules and JWT verification (`pam/`). JWT
  checks always enforce the `exp` claim and compare signatures in constant time.
- **Secrets:** the security daemon encrypts stored secrets with OpenSSL EVP
  (AES-256-CBC, random IV prefixed to the ciphertext).
- **Certificates:** ACME/RFC 8555 issuance and X.509 handling, including
  revocation, in `cert/`.

## Observability

`logd/` forwards container logs and evaluates alert rules (including silence
windows via a pure `alert_rule_active()` policy). `metrics/` samples CPU
(`kern.cp_time`), memory, and load average (`getloadavg`).

## Build & test model

- **Build system:** `bmake` (`bsd.prog.mk` / `bsd.lib.mk` / `bsd.test.mk`).
  Each subsystem is a static archive linked into the PIE binary; because the
  binary is a PIE, archive objects are compiled `-fPIC`. Lab builds use FreeBSD
  META_MODE (`.MAKE.MODE="meta verbose"` + `filemon(4)`) so incremental
  rebuilds are dependency-accurate.
- **Tests:** ATF/kyua. C unit tests `#include` the module `.c` directly (the
  module files have no `main`); root-requiring integration tests are shell-ATF
  (`lifecycle_test`, `network_cli_test`, `perfstress_test`). The suite runs
  green on FreeBSD 16-CURRENT (269 cases, 0 failed at time of writing).
- **Verification host:** a `vm-bhyve` FreeBSD 16-CURRENT guest (`oci-e2e`),
  reached over SSH, exercises the root-only jail/network paths.

## Where behavior diverges from the OCI/Linux model

`ocifbsd` maps to FreeBSD primitives honestly and returns a clear error where a
Linux-only concept has no analogue (see `OCI-CONFORMANCE.md` for the itemized
mapping). Cgroups become RCTL; Linux namespaces become the jail boundary plus
VNET; seccomp/AppArmor/SELinux become Capsicum/MAC where applicable. Linux
container images still run only if their binaries are executable under the
host's ABI support; the image *store and unpack* path is OS-agnostic.
