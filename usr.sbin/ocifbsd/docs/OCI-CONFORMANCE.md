<!--
SPDX-License-Identifier: BSD-2-Clause
Copyright (c) 2026 REVYTECH, Inc.
-->
# OCI conformance

This document records how `ocifbsd` maps to the OCI specifications and where
FreeBSD semantics differ from the Linux-centric parts. It has two parts: the
[OCI Runtime Specification](https://github.com/opencontainers/runtime-spec)
v1.0.2 (below) and the
[OCI Image Specification](https://github.com/opencontainers/image-spec) v1.0.0
(second half). The guiding rule (see `.plan/021.0`) is **map honestly to
FreeBSD primitives or return a clear error** — never emulate Linux badly.

## Runtime Specification

The official `oci-runtime-tools` conformance harness is written in Go and is
not part of the FreeBSD base system; running it would violate this project's
self-contained, zero-ports constraint. Conformance is therefore asserted by
the in-tree ATF/kyua suite (unit + root integration on a FreeBSD lab host),
which exercises each operation and the state schema directly. The table below
is the authoritative statement of coverage.

## Operations

| Operation | Status | Notes |
|-----------|--------|-------|
| `create`  | Conformant | Creates the jail from the bundle; persists state. `create_start_kill_delete` (root). |
| `start`   | Conformant | Runs `process.args` as the init; applies mounts, RCTL, VNET, MAC. |
| `kill`    | Conformant | Signals the init; idempotent for CREATED/STOPPED. |
| `delete`  | Conformant | Removes the jail and reclaims mounts/epairs/RCTL rules. |
| `state`   | Conformant | Emits `ociVersion`, `id`, `status`, `bundle`, and `pid` (when a process exists), per the spec's state schema. |

`list`, `inspect`, and `run` are convenience commands beyond the runtime CLI
contract.

## Lifecycle states

The spec defines `creating → created → running → stopped`. `ocifbsd` reports
`created`, `running`, and `stopped`; `creating` is transient and not observable
between two CLI invocations. `paused`/`paused (high priority)` are FreeBSD
extensions (jail SIGSTOP of the init) — the spec permits runtime-defined
status values.

## config.json coverage

| Field | Status | FreeBSD mapping |
|-------|--------|-----------------|
| `ociVersion` | Read | Recorded; not version-gated. |
| `process.args` / `cwd` / `env` | Conformant | exec of the init inside the jail. |
| `process.user.uid` / `gid` | Conformant | Applied to the init. |
| `process.rlimits` | Conformant | `setrlimit(2)` (`hooks`/`parse_rlimits` tests). |
| `process.noNewPrivileges` | Conformant | Enforced via a nosuid nullfs root. |
| `root.path` / `root.readonly` | Conformant | Read-only root via a `ro` nullfs overlay. |
| `mounts` | Conformant | Applied host-side before the init attaches; unmounted on delete. Traversal in destinations is rejected. |
| `hooks` (prestart/poststart/poststop) | Conformant | Executed at the spec's points (`hooks_test`). |
| `hostname` | Conformant | `host.hostname` jail parameter. |
| `linux.resources` (memory/cpu/pids) | Mapped | `rctl(8)` rules; needs `kern.racct.enable=1`. |
| `linux.readonlyPaths` / `linux.maskedPaths` | Parsed only | Recorded and **warned as unenforced** — no per-path overlay yet. |
| `linux.namespaces` | Native | A jail is a single combined namespace; per-type Linux namespaces are not modeled. VNET covers the network namespace. |
| `linux.seccomp` | Gap | No seccomp; Capsicum/MAC are the FreeBSD analogues (documented, not auto-derived). |
| `linux.devices` / `cgroupsPath` | Not applicable | Devfs and RCTL replace cgroup device/limit control. |
| `annotations` | Read | Preserved from the bundle. |
| `freebsd.*` | Extension | VNET, ip4/ip6, defaultGateway, bridge, macLabel — see `ocifbsd(8)`. |

## Honest gaps

- **seccomp**: not implemented; there is no faithful FreeBSD equivalent, so the
  field is ignored rather than silently "translated".
- **readonlyPaths / maskedPaths**: parsed and warned, not enforced.
- **Linux namespaces**: mapped onto the jail model rather than reproduced
  one-for-one.
- **Port publishing**: not a runtime-spec field; handled at the
  orchestration/Ensemble layer (`convert/`).

These are deliberate: the runtime maps what FreeBSD can honestly enforce and
warns (or no-ops) rather than pretending to enforce Linux-only controls.

## Image Specification

This section records how the image subsystem (`image/`) maps to the
[OCI Image Specification](https://github.com/opencontainers/image-spec) v1.0.0
and the compatible Docker Registry HTTP API v2. As with the runtime side, the
same self-contained constraint applies: parsing and verification use the
vendored json-c and libcurl plus base `libmd`/`libarchive`, with no ports.

## Pull / resolve

| Concern | Status | Notes |
|---------|--------|-------|
| Reference parsing | Conformant | `registry[:port]/repo[:tag|@digest]`; bare names default to `docker.io` and the `library/` namespace (`image_parse_test:registry_init_docker_hub`). |
| Registry API v2 | Conformant | Manifest and blob fetch over libcurl against the Docker Registry v2 endpoints; Docker Hub is remapped to `registry-1.docker.io`. |
| Manifest (schema 2) | Conformant | `application/vnd.docker.distribution.manifest.v2+json` and the OCI `application/vnd.oci.image.manifest.v1+json` layout: config descriptor + ordered layer list (`image_parse_test:parse_manifest_v2`, `parse_manifest_empty_layers`). |
| Image config | Conformant | `Entrypoint`+`Cmd`, `Env`, `WorkingDir` merged into the runtime config (`image_parse_test:parse_config_entrypoint_cmd`). |
| Content digests | Conformant | `sha256:` digests computed with `libmd` and verified against fetched content; a mismatch fails the pull (`image_parse_test:compute_and_verify_digest`, `verify_layer_missing_file`). |
| Malformed input | Conformant | Invalid JSON / empty references are rejected rather than crashing (`parse_manifest_bad_json`, `registry_init_invalid`). |

## Layers & filesystem

| Concern | Status | Notes |
|---------|--------|-------|
| Layer media types | Conformant | gzip'd tar layers (`...layer.v1.tar+gzip` / OCI `...layer.v1.tar+gzip`) unpacked with `libarchive`. |
| Ordered application | Conformant | Layers applied in manifest order over a single `rootfs/`. |
| Whiteouts | Conformant | `.wh.<name>` and `.wh..wh..opq` opaque markers handled per the layer spec (`whiteout_test`). |
| Path-traversal safety | Conformant | Entries escaping the destination (`..`, absolute paths) are rejected during unpack (`unpack_layer_test`). |
| Store layout | Conformant | `/var/lib/ocifbsd/<registry>/<repo>/<tag>/` holding a runtime `config.json` and a populated `rootfs/`; `images` lists usable roots, `rmi` removes them. |

## Honest gaps (image)

- **Signatures / provenance**: cothority/Notary/sigstore verification is not
  implemented; digests are verified but image *signatures* are not.
- **Multi-arch indexes**: `application/vnd.oci.image.index.v1+json` (fat
  manifests) resolve to the host architecture; cross-arch selection beyond the
  build host is not exercised.
- **Push**: `push.c` implements the upload path but is unverified against a
  live registry (see tasks 2.7 / 2.12) — it is not yet asserted conformant.
- **Live Docker Hub pull**: the pull→unpack→store pipeline is proven by a real
  unpacked image in the store and by the parser/unpack unit tests; a fresh
  end-to-end network fetch is gated on a routable test host (task 2.11).
