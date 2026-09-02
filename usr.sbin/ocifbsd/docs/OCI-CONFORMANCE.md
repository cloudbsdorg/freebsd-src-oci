<!--
SPDX-License-Identifier: BSD-2-Clause
Copyright (c) 2026 REVYTECH, Inc.
-->
# OCI Runtime Specification conformance

This document records how `ocifbsd` maps to the
[OCI Runtime Specification](https://github.com/opencontainers/runtime-spec)
v1.0.2, and where FreeBSD semantics differ from the Linux-centric parts of
the spec. The guiding rule (see `.plan/021.0`) is **map honestly to FreeBSD
primitives or return a clear error** — never emulate Linux badly.

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
