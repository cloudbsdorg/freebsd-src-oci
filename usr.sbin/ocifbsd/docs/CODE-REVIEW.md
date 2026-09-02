<!--
SPDX-License-Identifier: BSD-2-Clause
Copyright (c) 2026 REVYTECH, Inc.
-->
# ocifbsd code review

Task 5.10. This records the code-review pass over `usr.sbin/ocifbsd`: how it was
done, what it found, and the state it leaves the tree in. It is the review
artifact; the independent maintainer sign-off that gates an upstream submission
(task 5.11) is separate.

## Methodology

The review was **test-driven against real infrastructure** rather than reading
alone: for each subsystem a test was written that exercises the actual code
path — a live Docker Registry v2 for image pull/push, a mock CNI plugin for the
CNI interface, a file-backed ZFS pool for the storage backend, and real jails
on a FreeBSD 16 lab host for the runtime. Bugs were fixed as found and locked
with an ATF test. This surfaced defects that static reading had missed, because
several were argument-count or protocol-shape errors that only fail at runtime.

## Findings and resolutions

All of the following were found and fixed during this pass; each is covered by
a test now in the suite (275 cases, 0 failures on FreeBSD 16-CURRENT).

### Image push (`image/push.c`)
- **Wrong scheme and doubled port.** `upload_start`/`push_manifest` hardcoded
  `https://%s:%d` with `reg->host`, which already includes `:port`; the URL had
  a duplicated port and ignored the registry's insecure/TLS setting, so any
  plain-HTTP registry was unreachable. Fixed with a scheme-aware `push_url()`.
- **Config blob never uploaded.** `push_image` pushed layers then a manifest
  referencing a config blob it never sent — a conformant registry rejects that.
  The config blob is now pushed like a layer.

### Image pull / cache (`image/pull.c`)
- **No layer caching.** `registry_pull_layer` re-downloaded every layer. Added
  a content-addressed cache at `<data-dir>/layers/<digest>` with verify-on-hit.

### CNI (`network/cni.c`)
- **Plugin environment discarded.** `cni_add`/`cni_del`/`cni_check` built the
  `CNI_*` environment but `cni_call_plugin` took no env argument, so plugins ran
  with no `CNI_COMMAND` and could not function.
- **Netconf never sent to stdin**, and **child stdout never captured** — so even
  a correct plugin produced nothing the caller could read. `cni_call_plugin`
  now wires the socketpair to the child's stdin and stdout, applies the env, and
  writes the netconf.

### Networking (`network/network.c`)
- **`network_get` was a stub** returning empty configs, so every listed or
  inspected network came back blank and unresolvable. Implemented real parsing.
- **`bridge_delete` argument order.** `ifconfig bridge destroy <name>` made
  ifconfig treat the literal "bridge" as the interface; every bridge leaked.
  Now `ifconfig <name> destroy`.
- **stdio leaks.** `run_cmd` let child stdout corrupt the CLI's own output;
  `run_cmd_output` didn't capture stderr, so `bridge_exists`'s existence probe
  never matched and leaked ifconfig diagnostics. Both fixed.
- **epair leak.** `network_create` made a per-network epair that `network_delete`
  could not identify or reap; the address belongs on the bridge, and per-
  container epairs are `network_connect`'s job. Removed the leak.

### ZFS store (`image/zfs_store.c`)
- **`zfs destroy` flag order** (`<ds> -r -f` → `-r -f <ds>`) — recursive image
  destroy always failed.
- **`run_zfs` argc mismatches**: `snapshot` (4→3) and `clone` (5→4) passed an
  undefined trailing `va_arg` to zfs(8); the zvol create (5→6) dropped the
  dataset operand entirely.

### CLI (`ocifbsd.c`)
- **Doubled usage line.** Every subcommand printed `Usage: <cmd> <cmd>` because
  `usage()` was passed `argv[0]` (the subcommand name) as the program name.
  Collapsed to a fixed `Usage: ocifbsd <cmd>`.

## Configurability added

Following the project's open-default-plus-override convention, three directories
became env-overridable so the code is testable and deployable without touching
system paths: `OCIFBSD_CNI_CONF_DIR` / `OCIFBSD_CNI_BIN_DIR` (CNI) and
`OCIFBSD_ZFS_POOL` (storage), alongside the existing `OCIFBSD_DATA_DIR`.

## State of the tree

- **Suite:** 275 ATF cases, 0 failures on FreeBSD 16-CURRENT; root- and
  resource-gated tests (jails, ZFS pool, registry) skip cleanly when their
  dependency is absent.
- **Verified end-to-end:** runtime lifecycle, orchestration, named networks,
  CNI invocation, image pull/push/round-trip/caching, ZFS dataset ops, and
  release-image consumption.
- **Conformance:** documented in `OCI-CONFORMANCE.md` (runtime + image specs).

## Known limitations (not defects)

- ACME/PAM live round-trips need external servers; per-pod RCTL usage collection
  is a stub; `api/` needs a kqueue rewrite to build; multi-plugin CNI chains and
  multi-arch image indexes are not yet handled. These are tracked in
  `.plan/005.0-Risks-TODO.md` with honest status.
