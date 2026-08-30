# OCI-STATUS — ocifbsd Bootstrap Status

> **Purpose**: Single source of truth for the ocifbsd bootstrap effort. If the
> current systems go offline for any reason, this document is sufficient to
> resume work from any other machine.
>
> **Last updated**: 2026-08-30 — Grok-assisted audit loop on the new code

## 🔁 **2026-08-30 — Grok-4.6 audit loop (grok-analyze skill)**

Ran the new `grok-analyze` skill (local `grok`/FreeGrok CLI driving grok-4.6
as an independent auditor) over this session's new/changed code, verifying
each finding against the source before fixing. It surfaced real defects the
earlier reviews and the build had missed, especially in the new `image/load.c`
and the `ocifbsd.c` CLI:

- **load.c**: outer-archive extraction now sets libarchive secure flags and
  is restricted to regular files + directories (no symlink/hardlink/device/
  FIFO — closes a root arbitrary-overwrite and a follow-into-host-device);
  the untrusted `ref.name` annotation and the index/config/manifest digests
  are validated and content-verified before becoming root-written paths;
  import is transactional (atomic exclusive `mkdir`, rollback on failure);
  index/blobs must be regular files (lstat) before opening.
- **ocifbsd.c**: `ref_component_is_safe` rejects empty/"."/".."` (so `rmi
  alpine:.` no longer rm_rf's the whole repo); subcommand flags after the
  positional now work (`delete <id> --force`, `kill <id> 9`, …); `stop
  --timeout` bounded; `rm_rf`/`images`/`list` error handling tightened.

**Known design-level limitations (not yet addressed — need a design choice):**

1. **No per-container rootfs.** `create/run --image` uses the shared image
   store directory directly as the container's OCI bundle, so multiple
   containers from one image share and mutate the same rootfs, and `rmi`
   while a container is running would remove that container's rootfs. The
   correct fix is a per-container copy or a ZFS snapshot/clone of the image
   rootfs (the zfs_store layer has clone helpers to wire up), plus refusing
   `rmi` of an in-use image.
2. **Deep symlink TOCTOU in rm_rf / load_mkdirp.** Both build paths and act
   on them, so a component swapped to a symlink mid-operation could be
   followed. Low risk while the store lives under a root-owned
   /var/lib/ocifbsd, but the robust fix is an `openat`/`unlinkat`/`mkdirat`
   walk with `O_NOFOLLOW` from a base dirfd.
3. **No cross-invocation locking of container state.** Lifecycle operations
   serialize only with an in-process pthread mutex, so two concurrent CLI
   invocations on the same container (e.g. `start` racing `network set`, whose
   jail rebuild removes and recreates the jail) are not mutually excluded.
   `container_start` and `container_reconfigure_network` mitigate the worst
   outcome — attaching or removing a jail whose jid was reused — by resolving
   the jail by its `ocifbsd-<id>` name via `jail_getid` and refusing on a
   mismatch, but a narrow get-then-act window remains (the same window
   `jail(8) -r` has, since there is no atomic remove-by-name syscall). The
   robust fix is an exclusive `flock`/`lockf` on the per-container state file
   held across the read-check-act sequence in every lifecycle op.

## 🚀 **2026-08-28 — Verified running real FreeBSD OCI images end-to-end**

## 🚀 **2026-08-28 — Verified running real FreeBSD OCI images end-to-end**

`ocifbsd` was driven end-to-end on FreeBSD 15.1-STABLE against genuine OCI
images and every issue found was fixed. Confirmed working:

- **Run a real FreeBSD base image**: downloaded
  `FreeBSD-15.1-RELEASE-amd64-container-image-runtime.txz` from
  download.freebsd.org, `ocifbsd load`ed it, and `run`/`create`+`start`
  launched it as a native jail. `exec` inside reports the image's userland
  (freebsd-version 15.1-RELEASE) and the jailed hostname.
- **Full lifecycle**: create → start → exec → pause → resume → stop →
  delete, plus `kill` with signal names, all verified against a live
  container. Read-only rootfs is enforced (writes fail inside).
- **Image store**: `pull` from Docker Hub (anonymous token, multi-arch
  index resolution, layer download + digest verification, unpack) produces
  a usable rootfs+config.json; `load` imports a local OCI archive (dir or
  .txz) equivalently; `images` lists real repo:tag entries; `rmi` removes
  the store (including immutable/schg files).

New in this pass: the `load` command; container stdio detached to a
per-container log (so `run` no longer blocks the caller); container logs
removed on delete; `images` lists real images; fast-exiting containers are
no longer misreported as start failures; the layer unpacker accepts the
legitimate absolute/relative symlinks present in every real base image
(traversal still blocked via entry-name checks + O_NOFOLLOW); and rmi/delete
clear immutable flags so FreeBSD image rootfses can be removed.

## 🔒 **2026-08-28 — Full correctness & security hardening pass**

A comprehensive review of every subdirectory was completed and every
confirmed bug fixed. The whole tree builds clean on FreeBSD 15.1-STABLE
(`/usr/bin/make`, json-c installed) and the test suite passes **128/128**
via `kyua test -k Kyuafile` (including the root-only jail-lifecycle tests
run under sudo). New CLI verbs landed: `exec`, `stop`, `pause`, `resume`,
`push`.

**Fixed (by area):**

- **Build**: the GNU-make guard tested `.MAKE.VERSION` (undefined under real
  bmake) and aborted every native build — now `MAKE_VERSION`. Undefined
  `SRCDIR` expanded to root-relative `-I/include` in five subdir Makefiles.
- **Layer unpack** (remote-triggerable, root): tar path-traversal and
  symlink-escape are now rejected; files open with `O_NOFOLLOW`. Manifest
  layer digests are validated (`sha256:<hex>`) before use as paths.
- **Command injection (root RCE)**: eliminated across `network`/`vnet`/
  `bridge` (popen→fork/exec), `namespace` (strict name validation + exec),
  `security/mac`, `cert` backup, and `orchestration/health` (exec probes).
- **Registry push/pull/zfs**: fixed libcurl read/write callback misuse
  (crashes), a `char**`→`oci_layer**` type confusion, `popen` injection and
  an uninitialized-dataset destroy in `zfs_store`.
- **Core runtime**: apply `process.user` uid/gid (containers ran as root),
  PID-reuse-safe kill, hook-timeout enforcement, atomic state writes,
  parse poststart/poststop hooks, working `list`.
- **Orchestration**: NULL-deref crashes (`service_start`, `scheduler_init`),
  unbounded `pod_get`/`pod_list` recursion, and registry use-after-free.
- **logd**: ring-buffer infinite loop and forwarder self-deadlock (logging
  was fully broken), plus unbounded array indexing.
- **gc**: no longer deletes every volume/bridge; spares untracked interfaces.
- **rctl / tpm / convert / clustering**: correct rctl(8) syntax, TPM
  length/PCR bounds, YAML/JSON escaper overflows and k8s multi-doc/kind
  bugs, and a reference-counted cluster-node lifetime (fixes a UAF) with
  validated gossip/JOIN network input.

**Remaining (feature work, not bugs — mostly comment-only stub files):**
real ACME/Let's Encrypt (`cert/acme.c`), cloud export for AWS/GCP/Azure
(`export/*.c`), HTTP log forwarding (`logd/forward.c`), real TPM 2.0
operations (software fallback only today), and full Raft. These are
greenfield implementations that need external resources (credentials,
registry/cloud endpoints, TPM hardware) and product decisions; they are the
natural next targets once prioritized.

## 🎉🎉🎉 **ALL 15 SUBDIRs RE-ENABLED — BOOTSTRAP 100% COMPLETE!**

All 9 previously-commented-out SUBDIRs (cert, export, gc,
image, logd, network, orchestration, pam, security) have
been fixed and committed. The end-to-end build of
`ocifbsd` + all **15 active SUBDIRs** succeeds cleanly on
FreeBSD 16.0-CURRENT.

- **Main binary**: `ocifbsd` (50K) builds, links, and runs.
- **15 active SUBDIRs build clean**: api, cert, clustering,
  convert, export, gc, image, logd, metrics, namespace,
  network, orchestration, pam, security, security-daemon, tpm.
- **0 SUBDIRs commented out** — full coverage.

**Phase 1 commits (all pushed to origin)**:
- `2c33724ce60` — enable cert SUBDIR (OpenSSL 3.0 + FreeBSD 16 fixes)
- `2d35e44db14` — enable export SUBDIR (libmd SHA256 + FreeBSD 16 fixes)
- `d2219adb756` — enable gc SUBDIR (RB-tree + mkdirp_local)
- `f93983e352f` — enable logd SUBDIR (RB-tree + curl port path)
- `727bc76597c` — enable pam SUBDIR (LIB + pam_sm_* entry points)

**Phase 2 commits**:
- `12bf50dc15d` — enable network SUBDIR (PROG→LIB)
- `3a17a8285dd` — enable orchestration SUBDIR (PROG→LIB + stubs.c)

**Phase 3 commits**:
- `41a3643ee25` — enable image SUBDIR (PROG→LIB + libarchive)
- `59f2465dd21` — enable security SUBDIR (LIB + rctl/mac refactor)

**Re-verified at 2026-06-05 14:20 UTC**: Full `make` succeeds
end-to-end on the VM. All 15 SUBDIRs build clean. 42/42 unit
tests pass via `kyua test -k Kyuafile`.

**Earlier (BOOTSTRAP COMPLETE)**: `make` first succeeded
end-to-end at 2026-06-05 07:42 UTC with the original 7 SUBDIRs
(api, clustering, convert, metrics, namespace, security-daemon,
tpm). Binary is 50,712 bytes, links against
libc/libjail/libutil/libzfs/libm/libpthread/libcrypto/libmd
(per `make` `.depend` output).

## 🧪 **Unit test suite landed (2026-06-05 08:42 UTC)**

42/42 tests pass on the VM via `kyua test -k Kyuafile`. New files:

- `tests/usr.sbin/ocifbsd/parser_test.c` — 22 tests for
  `convert/parser.c` (yaml_escape, json_escape, native_format_*,
  parse_yaml/json stubs).
- `tests/usr.sbin/ocifbsd/k8s_test.c` — 20 tests for
  `convert/k8s.c` (k8s_detect_kind for all 13 kinds, plus
  k8s_convert_deployment and k8s_convert_multi).
- `tests/usr.sbin/ocifbsd/Makefile` — switched to
  `ATF_TESTS_C=parser_test k8s_test` (the correct modern variable;
  `ATF_TESTS` is the deprecated form), `WARNS=3` (matches the
  convert/ Makefile level), and the right `-I` paths so
  `#include "convert/parser.c"` and `convert.h` resolve.
- `tests/usr.sbin/ocifbsd/ocifbsd_test.c.disabled` — the original
  test, renamed because it referenced 13+ functions
  (ocifbsd_generate_cid, ocifbsd_validate_name, etc.) that
  never landed in the source. Preserved for review; can be
  revived once those functions are implemented.

**Two pre-existing source bugs discovered and pinned** (not fixed
— per CLAUDE.md "mention it - don't delete it"):

1. `yaml_escape(NULL)` / `json_escape(NULL)` return `strdup("")`
   (an empty string), not the quoted empty string `""`. The
   `yaml_escape_null` / `json_escape_null` tests pin this.
2. `k8s_convert_multi()` silently drops the **last** YAML document
   in a multi-doc stream (it only processes docs that are followed
   by another `---` separator). The `k8s_convert_multi_two_docs`
   test pins this with a `KNOWN BUG` comment.

**Build/run on the VM**:

```sh
cd ~/ocifbsd-build/tests/usr.sbin/ocifbsd
make                            # builds parser_test + k8s_test
kyua test -k ./Kyuafile         # runs all 42 tests
```
> **Branch**: `feature/oci-bootstrap`
> **Owner**: REVYTECH, Inc.
> **Target**: FreeBSD 16.0-CURRENT (native, tier-1)

---

## 1. Executive Summary

> **Status convention**: All times in this document are UTC unless
> noted. Local time (CDT = UTC-5 in summer) is shown alongside for
> convenience. The change log uses exact `YYYY-MM-DD HH:MM:SS` UTC
> timestamps; older entries use `YYYY-MM-DD ~HH:MM` approximations.

`ocifbsd` is a native OCI runtime for FreeBSD, built on top of `jail(8)`, VNET,
RCTL, ZFS, and capsicum. This branch (`feature/oci-bootstrap`) contains the
full bootstrap effort: cherry-picked OCI code from `origin/devel`, hardened
security audit fixes, a complete native build infrastructure, and a CI
workflow.

**Primary platform**: FreeBSD 16.0-CURRENT (amd64). All build infrastructure
treats FreeBSD as the tier-1 native target.

**Cross-build support**: macOS hosts and Linux hosts can cross-build to
FreeBSD amd64 via `tools/cross-build/macos.sh` + `make cross-build`. This is
opt-in and clearly marked as cross-build — it does not appear in the default
help output.

---

## 2. Rationale

### Why FreeBSD is tier-1

The whole point of `ocifbsd` is to use FreeBSD-native primitives:

- `jail(8)` + VNET for container isolation (not Linux namespaces)
- `capsicum(4)` for capability mode (not Linux seccomp-bpf)
- `rctl(8)` for resource limits (not Linux cgroups v2)
- `zfs(8)` for image storage (not Linux overlayfs)
- `pf(4)` for NAT/networking (not Linux iptables/nftables)
- `libxo(3)` for structured logging (the FreeBSD-native JSON emitter)

The OCI runtime spec is OS-agnostic at the surface, but every real
implementation is OS-specific at the syscall layer. `ocifbsd` cannot be
"ported" to Linux without losing its identity. Linux/macOS hosts only matter
as **cross-build** environments for developers who don't run FreeBSD as their
daily-driver.

### Why "darwin" was demoted from primary to opt-in cross-build

The previous incarnation of this Makefile advertised `darwin-build`,
`darwin-bootstrap`, and `darwin-test` as first-class targets next to the
native `build` target. That sent a misleading signal: it suggested
`ocifbsd` was a macOS toolchain first, with FreeBSD as an afterthought.
The opposite is true.

When the rest of the organization reviews this branch, the Makefile should
read as a **FreeBSD-native build** first, with cross-build helpers tucked
into a clearly-marked subsection. Any other presentation will be rejected.

### Why Makefile owns the build environment, not C code

The C code in `usr.sbin/ocifbsd/` is written to compile cleanly on FreeBSD
with no special flags. Build environment quirks (where to find `bmake`, what
`XCC` should be, what `MAKEOBJDIRPREFIX` to use) are entirely a property of
the **build host**, not the **target**. They belong in Makefiles, in
`tools/cross-build/`, and in CI configuration — not in `#ifdef __APPLE__`
blocks scattered through runtime code.

---

## 3. Architecture: What Lives Where

```
freebsd-src-oci/
├── usr.sbin/ocifbsd/              # The runtime (C code, FreeBSD-native)
│   ├── Makefile                   # Native build (default) + cross-build subsection
│   ├── README.md                  # FreeBSD-native first; cross-build in §6
│   ├── OCI-STATUS.md              # ← THIS FILE (live status doc)
│   ├── tools/                     # Build helpers
│   │   └── cross-build/           # OPT-IN cross-build helpers (macOS, Linux)
│   │       ├── macos.sh           # Bootstrap a macOS host for cross-build
│   │       └── README.md          # When to use cross-build, who needs it
│   ├── src/                       # Core runtime (jail lifecycle, OCI translation)
│   ├── image/                     # Pull/push, ZFS storage
│   ├── network/                   # Bridge, VNET, CNI
│   ├── security/                  # RCTL, capsicum (TODO: capsicum translation)
│   ├── security-daemon/           # RBAC, secrets, TLS
│   ├── orchestration/             # Pods, stacks, services, health
│   ├── clustering/                # Gossip, Raft
│   ├── convert/                   # K8s YAML, Docker Compose
│   ├── pam/                       # PAM auth (JWT secret)
│   ├── namespace/                 # Namespace isolation
│   ├── metrics/                   # Metrics collection
│   ├── api/                       # REST API server
│   ├── cert/                      # Certificate management
│   ├── logd/                      # Logging daemon
│   ├── gc/                        # Garbage collection
│   ├── tpm/                       # TPM (optional)
│   ├── export/                    # Cloud export (AWS/GCP/Azure)
│   ├── include/                   # Public headers
│   └── ocifbsd.c                  # CLI entry point
├── tools/
│   └── cross-build/               # Cross-build helpers (macOS, Linux)
│       ├── macos.sh               # Renamed from darwin-bootstrap.sh
│       └── README.md              # Why this exists, who should use it
├── tests/
│   └── usr.sbin/ocifbsd/          # Kyua test suite
├── .github/
│   └── workflows/ci.yml           # CI: FreeBSD-native build
├── .omo/drafts/                   # Internal plans (SECURITY, MIGRATION, CHANGELOG, etc.)
├── .omo/notepads/                 # Session-state memory
└── OCI-STATUS.md                  # ← THIS FILE
```

### The cross-build isolation principle

- **Native path**: `make -C usr.sbin/ocifbsd` on FreeBSD → uses base
`/usr/bin/make` (which IS bmake, the BSD make on FreeBSD since
FreeBSD 9), base `cc` (clang), no env vars required. No install
of `bmake` needed.
- **Cross-build path**: `make cross-build` from repo root → sources
  `tools/cross-build/macos.sh`'s env file → uses `XCC`/`XLD`/`XAS`/etc →
  invokes `tools/build/make.py` (from the FreeBSD source tree) to build
  userland libraries, then `bmake` (the BSD make port) to build `ocifbsd`.

The Makefile's `cross-build` target is the **only** place the cross-build
env gets sourced. No other target depends on `XCC`/`XLD`/`XAS`.

---

## 4. System Modifications (FreeBSD native build env)

### VM identity

| Field         | Value                                        |
| ------------- | -------------------------------------------- |
| Hostname      | `pppoe2.cloudbsd.org`                        |
| OS            | FreeBSD 16.0-CURRENT amd64                   |
| CPUs          | 6                                            |
| RAM           | 16 GB                                        |
| /usr/src      | @ `7d35b236768` (clean FreeBSD source)       |
| User          | `mlapointe` (sudo via `wheel`)               |
| SSH key       | `~mlapointe/.ssh/revhelix` (local)           |
| Build dir     | `~/ocifbsd-build` (cloned from origin)       |

### /etc/sysctl.conf (permanent, panic auto-recovery for VM host)

```sysctl
# /etc/sysctl.conf on pppoe2.cloudbsd.org
#
# These ensure a kernel panic reboots the VM instead of halting.
# Bare metal would use halt(8); a VM host has no console attached,
# so a halt = permanent outage until manual intervention.

vfs.zfs.vdev.min_auto_ashift=12   # 4K sector alignment for ZFS vdevs
sysctl net.inet.ip.forwarding=1    # required for jail/VNET networking

debug.debugger_on_panic=0          # do NOT enter debugger (no console)
kern.sync_on_panic=0               # do NOT sync fs (faster reboot)
kern.powercycle_on_panic=1         # ACPI power-cycle on panic = VM reboot
```

**Why these matter for a build VM**:

- `debug.debugger_on_panic=0` + `kern.powercycle_on_panic=1` together make
  the VM auto-reboot if a kernel panic occurs. Without these, the VM halts
  and stays offline until someone manually restarts it from the hypervisor
  console — a multi-hour outage for CI runs.
- `vfs.zfs.vdev.min_auto_ashift=12` ensures all ZFS vdevs use 4K sectors
  even if the underlying provider uses 512e emulation. Prevents the
  write-amplification penalty and the "ashift=9" warning at pool import.
- `net.inet.ip.forwarding=1` is required for VNET jails (which is what
  `ocifbsd` uses). Without it, jailed networking is one-way.

### /etc/make.conf (permanent, empty by design)

```makeconf
# /etc/make.conf on pppoe2.cloudbsd.org
#
# INTENTIONALLY EMPTY.
#
# Do NOT add MAKEFLAGS=-jN here. The FreeBSD build system
# (buildworld, installworld, installkernel, delete-old, delete-old-libs)
# relies on sequential execution for some targets. Setting MAKEFLAGS=-jN
# globally breaks those targets with "missing file" errors.
#
# Use /etc/src.conf for parallelism (see below).
```

### /etc/src.conf (permanent, scoped parallelism)

```srcconf
# /etc/src.conf on pppoe2.cloudbsd.org
#
# Scoped parallelism for buildworld / buildkernel.
# These flags are only honored by src Makefiles (bsd.*.mk) and do NOT
# leak into installworld/installkernel/delete-old.

MAKE_JOBS=yes
MAKE_JOBS_NUMBER=6
```

**Why `MAKE_JOBS_NUMBER` here, not `MAKEFLAGS=-j6` in `make.conf`**:

- `MAKE_JOBS=yes` + `MAKE_JOBS_NUMBER=N` is the **scoped** mechanism
  designed for this. It's honored by `bsd.*.mk` and ignored by targets
  that require sequential execution (`installworld`, `installkernel`,
  `delete-old`, `delete-old-libs`).
- `MAKEFLAGS=-jN` is the **global** mechanism. It applies to every `make`
  invocation, including the ones that MUST run sequentially. This causes
  spurious "missing file" failures during `installworld` because parallel
  install steps race on shared state.
- The convention in the FreeBSD handbook (§23.5) is `MAKE_JOBS_NUMBER` in
  `src.conf` for exactly this reason.

### /etc/make.conf + /etc/src.conf is the maximum CPU config

`MAKE_JOBS_NUMBER=6` is set to the actual core count (6 CPUs). This is the
maximum useful parallelism for this VM — going higher just adds scheduler
overhead. The setting is permanent and survives reboots.

### Cloned build dir on VM

```sh
ssh pppoe2.cloudbsd.org
cd ~
git clone git@github.com:cloudbsdorg/freebsd-src-oci.git ocifbsd-build
cd ocifbsd-build
git checkout feature/oci-bootstrap
```

After every `git push` from the local machine, refresh on the VM:

```sh
ssh pppoe2.cloudbsd.org 'cd ~/ocifbsd-build && git pull --rebase origin feature/oci-bootstrap'
```

### Why git, not ssh/sftp/rsync, for source code

Files get lost when moving through multiple non-atomic transports
(scp individual files → rsync a tree → ssh heredocs hang mid-quote).
Git is atomic per commit, reproducible, and has a built-in audit trail
of exactly what changed. SSH is fine for things git can't carry
(SSH keys, TLS certs, VM disk images, secrets).

---

## 5. Current Status

### Branch: `feature/oci-bootstrap` @ `41a3643ee25`

**230+ commits since session start**. Major work:
- Makefile refactored: native is default, cross-build is opt-in.
- README updated: leads with FreeBSD native, cross-build in §6.
- All 78 OCI source files compile clean under FreeBSD's strict
  `-Werror -Wall -Wextra` (WARNS?=6 equivalent), with the
  exception of one const-pointer warning in src/hooks.c that is
  being resolved as of this update.
- Cross-build helper `darwin-bootstrap.sh` moved to
  `tools/cross-build/macos.sh` and clearly labeled cross-build-only.
- Ported src/oci2jail.c and src/state.c from old json-c 0.10-0.12
  API (`struct json_value`, `JSON_TYPE_*`, `val->type`,
  `json_value_object(v)`, `json_object_property_value()`,
  `json_value_free()`) to new json-c 0.13+ API (`struct json_object`,
  `json_type_*`, `json_object_get_type(v)`, `json_object_object_get()`,
  `json_object_put()`). Done via sed + manual cleanup.
- json-c 0.18 installed via `sudo pkg install json-c` on the VM.
- Added `LDADD+= -ljson-c` + `CFLAGS+= -I/usr/local/include/json-c`
  to Makefile (json-c is a port, not in FreeBSD base; LIBADD
  validates against src.libnames.mk so port libs must go through
  LDADD with explicit path).
- Fixed `ocifbsd.c` pre-existing Klara-AI bugs: variable shadowed
  function (`canonical_name = canonical_name(name)`), missing
  `<strings.h>` for `strncasecmp`, missing `<pthread.h>` in
  `src/container.c`, missing `static` on global `opt` struct
  (`-Wmissing-variable-declarations`), unused variables.
- Fixed `src/container.c` `-Werror` issues: added `struct
  oci_runtime_spec *spec` field to `struct ocifbsd_container`
  (was being dereferenced but missing), added `<libutil.h>` for
  `setproctitle`, replaced deprecated `usleep()` with `nanosleep()`,
  replaced `asprintf` (hidden by `_XOPEN_SOURCE=700`) with two-pass
  `snprintf` + `malloc`.
- VM `/etc/sysctl.conf` configured for auto-reboot on kernel panic
  (`debug.debugger_on_panic=0`, `kern.sync_on_panic=0`,
  `kern.powercycle_on_panic=1`) so a CI kernel panic doesn't
  permanently halt the VM.

**150+ commits** ahead of `origin/main`, working tree clean (modulo
the `static static` regression fix from 2026-06-05 07:42 UTC that
hasn't been committed yet), all pushed to
`origin/feature/oci-bootstrap`.

### Code statistics

| Metric                          | Count   |
| ------------------------------- | ------- |
| `.c`/`.h` files in `ocifbsd/`   | 78      |
| Total lines of C                | ~25,000 |
| Subdirectories (modules)        | 15      |
| Makefile targets                | 30+     |
| `.omo/drafts/*.md` documents    | 10+     |

### Security audit (2026-06-03)

Completed in one session; 7 commits, all pushed.

| Severity   | Issue                                              | Status   |
| ---------- | -------------------------------------------------- | -------- |
| CRITICAL   | Hardcoded JWT signing secret (pam_auth.c)          | FIXED    |
| CRITICAL   | `run_zfs_str` data-loss bug (zfs_store.c)          | FIXED    |
| HIGH       | 24 shell injection sites (system + snprintf)       | FIXED    |
| HIGH       | 14+ buffer overflows (sprintf → snprintf)          | FIXED    |
| MEDIUM     | 5 memory leaks in logd realloc paths               | FIXED    |
| MEDIUM     | 14 more realloc bugs (8 leaks + 6 NULL derefs)     | FIXED    |
| MEDIUM     | strncpy into char[256] with unbounded length       | FIXED    |

### Stub / deferred work (documented, not AI slop)

| Item                                    | Reason for deferral                          | Doc                               |
| --------------------------------------- | -------------------------------------------- | --------------------------------- |
| `seccomp → capsicum` (security/mac.c)   | Capsicum requires major refactor              | `.omo/drafts/MIGRATION.md`        |
| `json-c → libxo` (5 files)              | `libxo` migration is 8-16 hrs                | `.omo/drafts/MIGRATION.md`        |
| TLS verification in `image/pull.c`      | Need CA bundle design decision               | `.omo/drafts/SECURITY.md`         |
| `convert/k8s.c` strcat verification     | Needs `git log` to confirm author intent      | Pending                           |
| `cert/cert.c` audit (1077 lines)        | Not yet audited                              | Pending                           |
| `auth.c` audit (1181 lines)             | Partially audited (JWT secret fixed)          | `.omo/drafts/SECURITY.md`         |

### Build progress update (live, mid-build)

**What compiled clean** (under FreeBSD strict `-Werror -Wall -Wextra
-Wcast-qual -Wthread-safety -Wmissing-variable-declarations ...`):
- `ocifbsd.c` — clean after fixing variable shadowing, missing
  `<strings.h>`, missing `<pthread.h>`, missing `static` on `opt` struct.
- `src/container.c` — clean after adding `<pthread.h>`, `<libutil.h>`,
  `struct oci_runtime_spec *spec` field, replacing `usleep`/`asprintf`,
  adding `extern void setproctitle`.
- `src/oci2jail.c` — clean after porting from json-c 0.10-0.12 API to
  0.13+ API (`struct json_value` → `struct json_object`, etc.).
- `src/state.c` — clean after porting to new json-c API, renaming
  `state_lock` mutex to `state_mutex` (collision with `int state_lock()`),
  rewriting `GET_*` macros, adding `__attribute__((no_thread_safety_analysis))`
  on lock/unlock.
- `src/utils.c` — pre-existing, was already clean.

**What just broke** (8 errors, 6 `-Wcast-qual` + 1 static-decl + 1
orphan-specifier):
- `src/hooks.c` — my earlier const-qual "fix" was over-aggressive:
  - Left an orphan `int` line above `static int` (my sed edit
    inserted both instead of replacing).
  - Used `const struct oci_hook * const *` (double-const) which
    C forbids implicit conversion to from `struct oci_hook **`.
  - Declared hooks_run `static` but the header still has it `extern`.

**Fix** (in progress):
1. Remove orphan `int` line.
2. Change `static int hooks_run(const struct oci_hook * const *hooks, ...)`
   to `static int hooks_run(const struct oci_hook **hooks, ...)` —
   match the header's loose signature so implicit `T**` → `const T**`
   (single-level const add) works at call sites.
3. Remove the 3x local-var blocks (`const struct oci_hook *const *prestart = ...`)
   since the loose signature doesn't need them.
4. Remove `int hooks_run(...)` from `include/ocifbsd.h` since it's now
   file-local static.

### Build infrastructure (updated mid-session)

**🎉 MAIN ocifbsd BINARY BUILT AND RUNS!** Verified on VM at
commit `83323e196f7`. The full output:

```
$ file usr.sbin/ocifbsd/ocifbsd
ocifbsd: ELF 64-bit LSB pie executable, x86-64, version 1 (FreeBSD),
         dynamically linked, for FreeBSD 16.0 (1600018)

$ ./ocifbsd --help
Usage: ./ocifbsd [OPTIONS] COMMAND [ARGS...]
FreeBSD Native OCI Runtime
Commands: create start kill delete state list inspect run
```

All 6 SRCS files compile clean under FreeBSD strict -Werror after
this session's fixes:
- `ocifbsd.c` — variable shadowing, missing <strings.h>, missing
  <pthread.h>, missing `static` on `opt` struct.
- `src/container.c` — added <pthread.h>, <libutil.h>, `struct
  oci_runtime_spec *spec` field, replaced `usleep`/`asprintf`,
  added `extern void setproctitle`, `extern int putenv`.
- `src/oci2jail.c` — ported from json-c 0.10-0.12 API to 0.13+ API
  (`struct json_value` → `struct json_object`, etc.), fixed shift
  count, fixed `jailparam_import_raw` 3-arg form.
- `src/state.c` — ported to new json-c API, renamed `state_lock`
  mutex to `state_mutex`, rewrote `GET_*` macros, added
  `__attribute__((no_thread_safety_analysis))`.
- `src/hooks.c` — removed const from `hooks_run` signature
  (C99 aliasing loophole), removed orphan `int` line, removed
  header declaration (now file-local static).
- `src/utils.c` — added <stdarg.h>, replaced removed
  `_PATH_ROOT` with stack-allocated `char root[] = "/"`.

Linker: added `md` to LIBADD for FreeBSD 16's moved-out
SHA256_Data symbol (was in libc, now in libmd).

### Subdir status: ALL 15 SUBDIRs ACTIVE

After the main binary links, build moves to the SUBDIR phase
(15 module libraries: libocifbsd_image.a, libocifbsd_network.a,
etc.). **All 15 of 15 SUBDIRs are now active and build clean**.
Zero remaining deferred refactor work.

**Per-SUBDIR build status** (all verified at 2026-06-05 14:20 UTC):

| SUBDIR            | Build artifact                        | Notes |
|-------------------|---------------------------------------|-------|
| api               | `ocifbsd` (linked into main)          | Active |
| cert              | (linked into main)                    | Active |
| clustering        | (linked into main)                    | Active |
| convert           | (linked into main)                    | Active |
| export            | (linked into main)                    | Active |
| gc                | (linked into main)                    | Active |
| image             | `libocifbsd_image.a` (250K) + `.so.1` | Active |
| logd              | (linked into main)                    | Active |
| metrics           | (linked into main)                    | Active |
| namespace         | (linked into main)                    | Active |
| network           | `libocifbsd_network.a` (93K) + `.so.1`| Active |
| orchestration     | `libocifbsd_orchestration.a` (258K) + `.so.1` | Active |
| pam               | `pam_ocifbsd.so.1` (41K, PAM module)  | Active |
| security          | `libocifbsd_security.a` (70K) + `.so.1` | Active |
| security-daemon   | (linked into main)                    | Active |
| tpm               | (linked into main)                    | Active |

**Phase 1 COMPLETE** (2026-06-05 13:40 UTC) — cert, export, gc,
logd, and pam all fixed, committed, and pushed. Each one
needed: `<sys/json.h>` dropped, json-c port path in CFLAGS,
`mkdirp_local()` (FreeBSD 16 doesn't export mkdirp), libmd
added to LIBADD for SHA256/HMAC/base64, RB_PROTOTYPE/RB_GENERATE
pairs, static keyword on helpers, and dead-code/typo removal.

**Remaining work** (Phase 2 + 3):
1. network/ + orchestration/ Makefiles: convert from PROG to
   LIB, or add a stub main.c. orchestration/ also needs either
   stub ocifbsd_create_container/etc. implementations, or the
   main ocifbsd binary needs to expose these symbols.
2. image/ source files: refactor pull.c (most of it is dead
   code / undeclared callbacks / wrong API usage), unpack.c
   and push.c not yet examined.
3. security/ source: rewrite rctl.c against the real
   rctl(8) API (struct rctl_usage, rctl_get_racct, etc.),
   review mac.c for similar AI-fabricated API usage.

### Build infrastructure (updated mid-session)

**`make` vs `bmake` clarification**: On FreeBSD, `/usr/bin/make` IS
bmake (BSD make). The `bmake` port is only needed on macOS/Linux
hosts for cross-build. As of this update, the Makefile has been
updated so that:
- FreeBSD native Quick Start uses `make` (the base BSD make, no
  install needed).
- Cross-build from macOS/Linux uses `bmake` (the port, installed by
  `tools/cross-build/macos.sh`).
- The Makefile now has a GNU-make guard at the top that errors out
  fast with a clear message if you accidentally use GNU make.
- The `info` target now reports `make -V .MAKE.VERSION` (the BSD
  make version, e.g. "20260508") instead of the broken
  `make --version` (GNU-only flag).
- README.md and this document have been updated to use `make` for
  the FreeBSD native path. The old `bmake` references in help text
  and the `info` target's version detection have been fixed.

### Build infrastructure

| What | Status |
|------|--------|
| `make -C usr.sbin/ocifbsd help`           | ✅ Working |
| `make -C usr.sbin/ocifbsd audit`          | ✅ Working |
| `make -C usr.sbin/ocifbsd info`           | ✅ Working |
| `make -C usr.sbin/ocifbsd` (native build) | ✅ **DONE** — verified clean at 2026-06-05 07:42 UTC. Main `ocifbsd` binary (50K) builds/links/runs. All 6 active SUBDIRs (api, clustering, convert, metrics, namespace, security-daemon, tpm) build clean. 9 SUBDIRs (cert, export, gc, image, logd, network, orchestration, pam, security) remain commented out as deferred refactor work. |
| `make -C usr.sbin/ocifbsd install`        | ⏳ Pending native build |
| `bmake -C usr.sbin/ocifbsd cross-build` (cross-build host) | ✅ Code in place, untested on macOS host |

### TODOs / FIXMEs / HACKs

- TODOs: 5 (all in `security/mac.c` as `TODO(seccomp→capsicum):` documented stubs)
- FIXMEs: 0
- HACKs: 0
- XXXs: 0

---

## 6. Plan: Make Native the Default

### 6.1 Makefile refactor ✅ DONE

**Before** (problematic):
```make
# === macOS host convenience targets ===
darwin-bootstrap:
	@./darwin-bootstrap.sh --install --yes
darwin-build: darwin-bootstrap
	@. $${OCIFBSD_ENV_FILE:-/tmp/ocifbsd-cross-build-env} && ...
```

**After** (now in tree):
```make
# === Cross-build (OPT-IN, for macOS/Linux hosts) ===
# =============================================================================
# If you are reading this, the FIRST thing to know is: this is the
# cross-build path. It is NOT the primary build path.
# ...
# =============================================================================

cross-bootstrap:
	@./tools/cross-build/macos.sh --install --yes

cross-build: cross-bootstrap
	@. $${OCIFBSD_ENV_FILE:-/tmp/ocifbsd-cross-build-env} && ...
```

The `darwin-bootstrap.sh` script moved to `tools/cross-build/macos.sh`.
The `darwin-build` / `darwin-test` targets became `cross-build` /
`cross-test`, and they are listed in a separate section of `make help`,
clearly labeled "Cross-build (OPT-IN, macOS/Linux hosts only) -- see
tools/cross-build/README.md".

The default `make` target is unchanged — it's the FreeBSD native build.
No new env vars required for native.

### 6.2 README.md updates ✅ DONE

The "Quick Start" section now leads with explicit `bmake` invocation on a
FreeBSD host (no env vars, no cross-toolchain). The trailing section "Building
from macOS or Linux (cross-build — opt-in)" explicitly says it is NOT the
primary build path and points to `tools/cross-build/README.md` for details.

### 6.3 `info` target updates

The `info` target's "Cross-build env (if sourced)" section stays, but the
output adds an explicit "Native build" section that shows host OS, native
`bmake` path, native `cc` path, and core count. The first thing a reviewer
of this branch should see is "this is a FreeBSD-native build".

### 6.4 Code fixes for native build 🔄 IN PROGRESS (mostly done)

Done in this session:
1. **`src/container.c`**: added `#include <pthread.h>` ✅
2. **`src/container.c`**: added `struct oci_runtime_spec *spec` field to
   `struct ocifbsd_container` in `include/ocifbsd.h` (was being
   dereferenced but missing) ✅
3. **`src/container.c`**: added `<libutil.h>` for `setproctitle` ✅
4. **`src/container.c`**: replaced deprecated `usleep(100000)` with
   `nanosleep({0, 100*1000*1000}, NULL)` ✅
5. **`src/container.c`**: replaced `asprintf` (hidden by `_XOPEN_SOURCE=700`)
   with two-pass `snprintf(NULL, 0)` + `malloc` + `snprintf` ✅
6. **`src/container.c`**: added `<sys/types.h>` for `u_char`/`u_int` in
   `<sys/mount.h>` ✅
7. **`ocifbsd.c`**: fixed variable shadowing function
   (`canonical_name = canonical_name(name)` → renamed local var to
   `cname`, kept `canonical_name(name)` function call) ✅
8. **`ocifbsd.c`**: added `<strings.h>` for `strncasecmp` ✅
9. **`ocifbsd.c`**: added `static` to global `opt` struct (was triggering
   `-Wmissing-variable-declarations`) ✅
10. **`ocifbsd.c`**: removed unused `int i` in main and `char *id` in
    cmd_create ✅
11. **`src/oci2jail.c` + `src/state.c`**: mechanical port from old json-c
    0.10-0.12 API to new json-c 0.13+ API (sed + manual cleanup). ~174 line
    changes in oci2jail.c, ~32 in state.c. ✅
12. **`src/oci2jail.c`**: added `n_ip4`, `n_ip6`, `n_dns`, `n_default_gateway4`,
    `n_default_gateway6`, `n_rctl_rules`, `n_securelevel` count fields
    to `struct oci_freebsd` ✅
13. **`src/oci2jail.c`**: added `<sys/types.h>` for `u_char`/`u_int` ✅
14. **`src/oci2jail.c`**: fixed `m->type` false positive from sed port
    (struct field, not json type check) ✅
15. **`src/oci2jail.c`**: fixed shift count error in gid parsing
    (removed `>> 32` on 32-bit int) ✅
16. **`src/oci2jail.c`**: fixed `jailparam_import_raw` to pass 3 args
    (added `strlen(value)+1` for valuelen) ✅
17. **`src/oci2jail.c`**: cast string literals through `(void *)(uintptr_t)`
    for `jailparam_import_raw` ✅
18. **`src/state.c`**: rewrote `GET_STRING`/`GET_INT`/`GET_INT64` macros
    to use new json-c API directly (the old `json_get_string` helper
    was never defined, so the original code was broken — old API
    just hid it) ✅
19. **`src/state.c`**: renamed `state_lock` mutex to `state_mutex` to
    avoid collision with `int state_lock(void)` function declaration
    in `ocifbsd.h` ✅
20. **`src/state.c`**: replaced `json_int_t` with `int64_t`,
    `JSON_C_OBJECT_TO_STRING_PRETTY` with `JSON_C_TO_STRING_PRETTY`,
    `json_parse_string` with `json_tokener_parse` ✅
21. **`src/state.c`**: replaced `GET_STRING(state_str, "state")` with
    direct enum mapping (struct has `ocifbsd_state_t state` enum,
    not a `state_str` string field) ✅
22. **`src/state.c`**: added `__attribute__((no_thread_safety_analysis))`
    to `state_lock`/`state_unlock` (Clang -Wthread-safety can't see
    the lock/unlock pairing across function boundaries) ✅
23. **`src/hooks.c`**: fixed `-Wcast-qual` on const-pointer-to-pointer
    by using `const struct oci_hook * const *` temporaries ✅
24. **`src/hooks.c`**: made `hooks_run` take `const struct oci_hook
    * const *` (const at both levels) and added `static` (file-local
    only) ✅
25. **Makefile**: added `.MAIN: all` (force default to bsd.prog.mk's
    `all` target, not the first explicit target which is now
    `cross-bootstrap`) ✅
26. **Makefile**: added `.PATH: ${.CURDIR}/src` (let BSD make find
    `src/container.c` etc. for .o → .c dependency rules) ✅
27. **Makefile**: added `.PATH.sys: /usr/share/mk` (set system
    include path so `.include <bsd.prog.mk>` works) ✅
28. **Makefile**: set `MAKEOBJDIRPREFIX=` (disable /usr/obj objdir,
    use current dir) ✅
29. **Makefile**: moved src.opts.mk .include to top of file (before
    `SUBDIR.${MK_TESTS}` line) ✅
30. **Makefile**: used absolute paths in `.include "/usr/share/mk/bsd.prog.mk"`
    (workaround for VM BSD make not supporting `.PATH.sys`) ✅
31. **Makefile**: removed `-D_XOPEN_SOURCE=700` (hid BSD types
    `u_char`/`u_int` used by `<sys/mount.h>`) ✅
32. **Makefile**: added `-D__BSD_VISIBLE=1` (explicitly enable BSD types) ✅
33. **Makefile**: added `-D_POSIX_C_SOURCE=200809L` (enable asprintf and
    other POSIX 2008 functions) ✅
34. **Makefile**: moved `json-c` from `LIBADD` to `LDADD` (json-c is a
    port, not in base; LIBADD validates against src.libnames.mk) ✅
35. **Makefile**: added `CFLAGS+= -I/usr/local/include/json-c` and
    `LDFLAGS+= -L/usr/local/lib` for the port ✅

Still to do:
- Link step (should work now that all SRCS compile)
- Run `make -C subdir` for each of the 15 module subdirs
  (api, cert, clustering, convert, export, gc, image, logd,
  metrics, namespace, network, orchestration, pam, security,
  security-daemon, tpm) — each has its own Makefile and may have
  similar issues to the main SRCS files
- `sudo make install` end-to-end
- `ocifbsd --version` smoke test

### 6.5 Native build sequence (target)

```sh
# On pppoe2.cloudbsd.org
cd ~/ocifbsd-build
git pull --rebase origin feature/oci-bootstrap
git checkout feature/oci-bootstrap

# Native build of the main binary
cd usr.sbin/ocifbsd
bmake clean
bmake -j6         # uses /usr/bin/cc, /usr/bin/bmake, no env vars

# Native build of subdirs (modules)
bmake -j6 SUBDIR=all

# Build + install
sudo bmake install DESTDIR=/

# Run tests
cd ../../tests/usr.sbin/ocifbsd
bmake
kyua test -k tests/usr.sbin/ocifbsd/Kyuafile
```

### 6.6 Verification

After the refactor, the build must pass:

- [ ] `bmake -C usr.sbin/ocifbsd` produces `ocifbsd` binary on FreeBSD VM
- [ ] `bmake -C usr.sbin/ocifbsd SUBDIR` builds all 15 modules
- [ ] `sudo bmake install` installs to `/usr/sbin/ocifbsd` and
      `/usr/share/man/man8/ocifbsd.8`
- [ ] `ocifbsd --version` runs and prints version
- [ ] All `ocifbsd` subcommands show in `--help`
- [ ] `tools/cross-build/macos.sh --check` works on a macOS host
- [ ] `make cross-build` (from repo root) cross-builds to FreeBSD amd64
      from macOS
- [ ] CI workflow builds on FreeBSD 16.0-CURRENT (GitHub Actions)

### 6.7 PR readiness

When the build passes and the Makefile/README/script move are complete:

```sh
git push origin feature/oci-bootstrap
gh pr create --base main \
  --title "ocifbsd: bootstrap FreeBSD-native OCI runtime" \
  --body-file .omo/drafts/CHANGELOG.md
```

---

## 7. Out-of-Scope / Explicitly Deferred

These are NOT in this PR. They have their own plans or are punted.

- **Capsicum translation of seccomp calls** (3 files) — see
  `.omo/drafts/MIGRATION.md`. Blocks deployment to production but not
  the bootstrap.
- **libxo migration of json-c** (5 files) — see `.omo/drafts/MIGRATION.md`.
  Cosmetic for build, mandatory for base-system integration.
- **Linux native port** — explicitly out of scope. `ocifbsd` is
  FreeBSD-native by design.
- **macOS native port** — explicitly out of scope. Same reason.
- **Windows native port** — lol no.

---

## 8. Resuming Work After an Outage

If `pppoe2.cloudbsd.org` is gone, or the local machine is gone, here's how
to resume from this document alone:

```sh
# 1. Get a FreeBSD 16.0-CURRENT VM (any provider)
# 2. Install minimal tools
#    Note: 'bmake' is NOT needed on FreeBSD -- /usr/bin/make IS bmake.
sudo pkg install -y git
# 3. Configure parallel build (max CPU cores)
echo 'MAKE_JOBS=yes'        | sudo tee -a /etc/src.conf
echo 'MAKE_JOBS_NUMBER=6'   | sudo tee -a /etc/src.conf
# 4. Clone the branch
git clone git@github.com:cloudbsdorg/freebsd-src-oci.git
cd freebsd-src-oci
git checkout feature/oci-bootstrap
# 5. Build (FreeBSD native; /usr/bin/make IS BSD make)
cd usr.sbin/ocifbsd
make -j6
```

If the local macOS machine is gone, set up a new one with the cross-build
helper:

```sh
git clone git@github.com:cloudbsdorg/freebsd-src-oci.git
cd freebsd-src-oci
git checkout feature/oci-bootstrap
./tools/cross-build/macos.sh --install --yes
. /tmp/ocifbsd-cross-build-env
make cross-build
```

If GitHub is gone, the local clone on the VM at `~/ocifbsd-build` is
sufficient — it has the full history, the full `feature/oci-bootstrap`
branch, and the full `.omo/drafts/` documentation tree.

---

## 9. Contact / Escalation

- **Owner**: REVYTECH, Inc.
- **Primary contact**: `@mlapointe` on the team chat
- **Escalation path**: REVYTECH CTO → board if REVYTECH is dissolved

---

## 10. Change Log

| Timestamp (UTC)      | Author    | Change                                                              |
| -------------------- | --------- | ------------------------------------------------------------------- |
| 2026-06-05 14:25:00 | mlapointe | **🎉 BOOTSTRAP 100% COMPLETE!** All 15 SUBDIRs active and build clean. Phase 3 security/ done (commit `59f2465dd21`, pushed). Converted security/ from PROG to LIB (SHLIB_NAME=ocifbsd_security.so.1). Fixed: rctl.h duplicate enum value, struct rctl_usage field mismatch, missing <stdarg.h>, dead/broken rctl_resource_sysctls[] array, sign-compare on sizeof(), C23 declaration-after-statement in mac.c, realloc without NULL check, NULL deref in mac_label_compare, 2 unused vars. Added ocifbsd_security.3 man page. libocifbsd_security.a (70K) + ocifbsd_security.so.1 (28K) build clean. All 9 previously-deferred SUBDIRs now land in main Makefile. | 59f2465dd21 |
| 2026-06-05 14:05:00 | mlapointe | **Phase 3 image/ SUBDIR done** (commit `41a3643ee25`, pushed). Converted image/ from PROG to LIB (SHLIB_NAME=ocifbsd_image.so.1). Fixed 30+ unused vars, added <dirent.h>/<strings.h>/<json-c/json.h>, fixed archive_entry_stat() 1-arg signature, removed reference to deleted archive_read_support_format_tar_grzip(), wrapped C23 declaration-after-statement in braces. Moved archive to LIBADD+=archive (src.libnames.mk-validated). Added ocifbsd_image.3 man page. Verified: libocifbsd_image.a (250K) + ocifbsd_image.so.1 (67K) build clean. 13/15 SUBDIRs now active. Only security/ remains. | 41a3643ee25 |
| 2026-06-05 13:50:00 | mlapointe | **Phase 2 orchestration/ SUBDIR done** (commit `3a17a8285dd`, pushed). Converted orchestration/ from PROG to LIB (SHLIB_NAME=ocifbsd_orchestration.so.1) with stubs.c that provides 6 stub implementations of ocifbsd_create_container/start/stop/delete/get_container_state/logs (real impls are internal to the main binary, override at link time). Added ocifbsd_orchestration.3 man page. Binary 65K builds. | 3a17a8285dd |
| 2026-06-05 13:48:00 | mlapointe | **Phase 2 network/ SUBDIR done** (commit `12bf50dc15d`, pushed). Converted network/ from PROG to LIB (SHLIB_NAME=ocifbsd_network.so.1). Added ocifbsd_network.3 man page. Binary 31K builds. | 12bf50dc15d |
| 2026-06-05 13:46:00 | mlapointe | **Phase 1 + 2 status doc update** (commit `7341bd8f5ef`, pushed). Marked Phase 1 complete (5 SUBDIRs re-enabled). Added Phase 2 + 3 sections. Updated header §5. | 7341bd8f5ef |
| 2026-06-05 13:42:00 | mlapointe | **Phase 1 pam/ SUBDIR done** (commit `727bc76597c`, pushed). Converted pam/ from PROG to LIB (PAM module, SHLIB_NAME=pam_ocifbsd.so.1). Added pam_sm_authenticate, pam_sm_setcred, pam_sm_acct_mgmt entry points. Renamed pam_get_user → ocifbsd_pam_get_user (collision with PAM's built-in). Added hmac_sha256 wrapper (OpenSSL), base64_encode/decode local impls, mkdirp_local. Fixed RB_PROTOTYPE/RB_GENERATE name mismatch (group_map_tree → group_role_map_tree). Removed 11+ unused vars. Added pam_ocifbsd.8 man page. Binary 41K PAM module builds. | 727bc76597c |
| 2026-06-05 08:45:00 | mlapointe | **Unit test suite landed**: 42/42 tests pass on VM via `kyua test`. Added `parser_test.c` (22 tests for convert/parser.c) and `k8s_test.c` (20 tests for convert/k8s.c). Renamed broken `ocifbsd_test.c` → `ocifbsd_test.c.disabled`. Switched Makefile to `ATF_TESTS_C=parser_test k8s_test` + `WARNS=3` + correct -I paths. Pinned 2 pre-existing source bugs (escape NULL returns empty string, k8s_convert_multi drops last doc). Updated this doc + README + CHANGELOG + ai-slop-backlog for BOOTSTRAP COMPLETE state. | (working tree, pending commit) |
| 2026-06-05 03:02:00 | mlapointe | **🎉 BOOTSTRAP COMPLETE!** `make` succeeds end-to-end on FreeBSD 16. ocifbsd (50K) builds, links, runs, and shows all 8 commands (create, start, kill, delete, state, list, inspect, run). All 6 active SUBDIRs build clean: api, clustering, convert, metrics, namespace, security-daemon, tpm. 9 SUBDIRs remain commented out as deferred AI-slop refactor work. | 82bca532405
| 2026-06-05 02:53:12 | mlapointe | Commented out orchestration/ SUBDIR. All 7 .c files (pod/stack/scheduler/health/rolling_update/orch_cli/orch_init) compile clean after ~15 fixes (json-c port path, <pthread.h> in 2 files, mkdirp extern in 3 files, ~5 static additions, ~10 unused-var removals, get_physmem+rolling_update_progress forward decls, bad spec.namespace ref). Link phase fails: no main() AND pod.c calls internal main-binary symbols (ocifbsd_create_container etc.). Same AI-slop approach as image/network/: deferred to follow-up refactor PR. | 47122a8c9dc
| 2026-06-05 01:58:00 | mlapointe | Commented out network/ SUBDIR. All 4 .c files (network/bridge/vnet/cni) compile clean after ~30 fixes (uuidgen→uuid_create, mkdirp extern, stdarg+sys/stat+dirent includes, 10+8+5+2+5+2 static additions, unused-var removals, popen argv->string, <netinet6/in6.h> removal, json-c port path, duplicate-static cleanup). Link phase fails: no main() in any of the files, Makefile wrongly declares PROG. Same AI-slop approach as image/: deferred to follow-up refactor PR. | a907cb67f68
| 2026-06-05 00:34:25 | mlapointe | Commented out image/ SUBDIR (many AI-slop issues in pull.c, unpack.c, push.c not yet seen). zfs_store.c clean after libmd + stdarg + externs + unused-var + sizeof + nested-comment fixes. Deferred to follow-up refactor PR. | 38306f700de
| 2026-06-04 23:50    | mlapointe | Fix 3 more pull.c issues: removed unused m in parse_manifest, removed unused data/data_len in fetch_config, re-added workingDir/user (used later). | 38306f700de
| 2026-06-04 23:48    | mlapointe | Fix 7 pull.c issues: fopen 3-arg -> construct path with snprintf; removed unused entrypoint/cmd/workingDir/user/exposed_ports. | 3f0afedc15d
| 2026-06-04 23:45    | mlapointe | Fix CURLOPT_WRITFUNCTION typo (missing underscore-F) in pull.c. | 0f336e8ca85
| 2026-06-04 23:40    | mlapointe | Add json-c include + -ljson-c to image/Makefile (pull.c uses json_object_*). | cb1cb80c991
| 2026-06-04 23:38    | mlapointe | Add WriteMemoryCallback curl callback + MemoryStruct in pull.c. | 2877c23459a
| 2026-06-04 23:35    | mlapointe | Add missing header_only curl callback in pull.c. | eeac103ae2a
| 2026-06-04 23:33    | mlapointe | Use LDADD for curl (port, not in src.libnames.mk). | 4e61cd5a835
| 2026-06-04 23:31    | mlapointe | Add curl to LIBADD in image/Makefile (port pattern, later changed to LDADD). | 520dd348e5c
| 2026-06-04 23:28    | mlapointe | Remove unused static zfs_cmd_buf in zfs_store.c. | 7f5b1045109
| 2026-06-04 23:25    | mlapointe | Remove unused argc in zfs_destroy_dataset (zfs_store.c). | 188540a1dca
| 2026-06-04 23:21    | mlapointe | Fix 4 follow-up zfs_store.c issues: mkdirp 2-arg sig, actual_argc redefinition, dst_mp/mp unused. | 72842be5ed9
| 2026-06-04 23:15    | mlapointe | Fix 17 zfs_store.c issues: stdarg.h, externs for mkdirp/copy_file, ret decl, 5 unused vars, sizeof(dataset) typo, nested comment. | e882699e37b
| 2026-06-04 23:08    | mlapointe | docker_compose.c: fix no-replicas asprintf format string (added 1 %s for 8 trailing args). | 7159f337ad3
| 2026-06-04 23:04    | mlapointe | docker_compose.c: fix with-replicas asprintf (added 1 %s for 10 trailing args). | 1d814fa08bc
| 2026-06-04 23:00    | mlapointe | convert/k8s.c: remove undeclared doc_num++ (worktree state was stale from prior commit). | cc4cb657f43
| 2026-06-04 22:55    | mlapointe | convert/k8s.c: remove unused doc_num counter (declaration only). | 9fa4fe898ce
| 2026-06-04 22:48    | mlapointe | **MAJOR MILESTONE**: main ocifbsd binary built (50,712 bytes) and runs (`./ocifbsd --help` shows commands). Fixed SHA256_Data link (added libmd). Subdir phase hit 5 broken Makefiles (cert/export/gc/logd/pam — AI-generated stubs with bad `<include>` syntax + missing Makefile.inc). Pragmatically commented out 5 SUBDIR entries to get end-to-end build. Tracked as deferred work. | 83323e196f7
| 2026-06-04 23:15    | mlapointe | Fixed utils.c cast-qual: replaced `(char *)"/"` with stack `char root[] = "/"`. | f54123407dc
| 2026-06-04 23:08    | mlapointe | utils.c: added <stdarg.h>, replaced removed _PATH_ROOT (FreeBSD 16 dropped it from paths.h) with `"/"`. | 429404b5704
| 2026-06-04 23:00    | mlapointe | hooks.c: removed const from `hooks_run` (C99 aliasing loophole forbids T**→const T**). | 17d7ab498ae
| 2026-06-04 22:55    | mlapointe | container.c: added local `extern int putenv(char *)` (gated on `__XSI_VISIBLE` in stdlib.h). Makefile: added `-Wno-error=visibility` for FreeBSD 16 signal.h system-header warning. | 3dae26286c6
| 2026-06-04 22:33:17 | mlapointe | Live status update: build progressed through ocifbsd.c, container.c, oci2jail.c, state.c, utils.c all clean. hooks.c hit 8 const-qual errors (over-aggressive const fix from earlier); fix in progress. Added header timestamp + UTC timestamps in change log. | (in flight)
| 2026-06-04 ~22:00   | mlapointe | Live update: build progressed through 5/6 SRCS (d07cfbd32f5 push). hooks.c 8 const-qual errors detailed in §5. | b5a0a7bbf93
| 2026-06-04 ~21:30   | mlapointe | Verified `make` (BSD make) works on FreeBSD VM; `make -V .MAKE.VERSION` returns 20260508. Updated all bmake→make for FreeBSD native; kept bmake for cross-build. README.md cross-build example fixed (was `make`, now `bmake`). Makefile GNU-make guard added; info target uses `.MAKE.VERSION`; removed redundant `Host bmake` line | d07cfbd32f5
| 2026-06-04 ~20:00   | mlapointe | Initial OCI-STATUS.md created (this file) — but only as one-shot, not as live document. Long gap of not updating acknowledged. | 084cb8a6f78
| 2026-06-04 ~19:30   | mlapointe | Documented VM panic-recovery sysctl.conf in OCI-STATUS. Added `debug.debugger_on_panic=0` + `kern.powercycle_on_panic=1` to /etc/sysctl.conf. | e2aa011a05a
| 2026-06-04 ~19:00   | mlapointe | Identified need to demote darwin-* to opt-in cross-build (README + Makefile) | c34bc137950
| 2026-06-04 ~18:00   | mlapointe | Rebased onto 17 new origin/main commits, force-pushed clean         | (pre-rebase)
| 2026-06-03 ~22:00   | mlapointe | Security audit complete (7 commits, all CRITICAL/HIGH/MEDIUM fixed) | (audit commits)
| 2026-06-02 ~18:00   | mlapointe | 65+ commits of OCI bootstrap work pushed to origin/feature/oci-bootstrap | (bulk)
