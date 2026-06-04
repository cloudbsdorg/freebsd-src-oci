# OCI-STATUS — ocifbsd Bootstrap Status

> **Purpose**: Single source of truth for the ocifbsd bootstrap effort. If the
> current systems go offline for any reason, this document is sufficient to
> resume work from any other machine.
>
> **Last updated**: 2026-06-04
> **Branch**: `feature/oci-bootstrap`
> **Owner**: REVYTECH, Inc.
> **Target**: FreeBSD 16.0-CURRENT (native, tier-1)

---

## 1. Executive Summary

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
│   ├── darwin-bootstrap.sh        # → tools/cross-build/macos.sh
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

- **Native path**: `bmake -C usr.sbin/ocifbsd` on FreeBSD → uses system
  `bmake`, system `cc` (clang), no env vars required.
- **Cross-build path**: `make cross-build` from repo root → sources
  `tools/cross-build/macos.sh`'s env file → uses `XCC`/`XLD`/`XAS`/etc →
  invokes `tools/build/make.py` (from the FreeBSD source tree) to build
  userland libraries, then `bmake` to build `ocifbsd`.

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

### Branch: `feature/oci-bootstrap` @ `370253528c0` (rebased)

**65+ commits** ahead of `origin/main`, working tree clean, all pushed
to `origin/feature/oci-bootstrap`.

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

### Build infrastructure

| Item                                            | State          |
| ----------------------------------------------- | -------------- |
| `bmake -C usr.sbin/ocifbsd info`                | Works on VM    |
| `bmake -C usr.sbin/ocifbsd` (native build)      | **BLOCKED**: missing `#include <pthread.h>` in `src/container.c` |
| Cross-build from macOS                          | Working, will be demoted to opt-in target |
| `make cross-build` (renamed)                    | Will be implemented in this session        |

### TODOs / FIXMEs / HACKs

- TODOs: 5 (all in `security/mac.c` as `TODO(seccomp→capsicum):` documented stubs)
- FIXMEs: 0
- HACKs: 0
- XXXs: 0

---

## 6. Plan: Make Native the Default

### 6.1 Makefile refactor

**Before** (current, problematic):
```make
# === macOS host convenience targets ===
darwin-bootstrap:
	@./darwin-bootstrap.sh --install --yes
darwin-build: darwin-bootstrap
	@. $${OCIFBSD_ENV_FILE:-/tmp/ocifbsd-cross-build-env} && ...
```

**After** (target):
```make
# Native build (FreeBSD) is the default.
# Just run: bmake -C usr.sbin/ocifbsd
# or:     make -C usr.sbin/ocifbsd
#
# === Cross-build (OPT-IN, for macOS/Linux developers) ===
# Most developers should not need this. If you ARE on macOS/Linux and
# want to cross-build to FreeBSD, see tools/cross-build/README.md.
#
#   make cross-build           # cross-build userland + ocifbsd + tests
#   make cross-test            # cross-build + deploy to VM + run kyua tests
```

The `darwin-bootstrap.sh` script moves to `tools/cross-build/macos.sh`.
The `darwin-build` / `darwin-test` targets become `cross-build` /
`cross-test`, and they are listed in a separate section of `make help`,
clearly labeled "Cross-build (macOS/Linux hosts)".

### 6.2 README.md updates

The "Quick Start" section currently leads with `cd usr.sbin/ocifbsd && make`.
That stays. A new section §6 "Building from macOS or Linux" appears at the
bottom with a clear cross-reference to `tools/cross-build/README.md`.

### 6.3 `info` target updates

The `info` target's "Cross-build env (if sourced)" section stays, but the
output adds an explicit "Native build" section that shows host OS, native
`bmake` path, native `cc` path, and core count. The first thing a reviewer
of this branch should see is "this is a FreeBSD-native build".

### 6.4 Code fixes for native build

Required to unblock native build on FreeBSD 16.0-CURRENT:

1. **`src/container.c`**: add `#include <pthread.h>` (and audit other files
   for the same gap — `src/state.c`, `src/oci2jail.c`, `src/hooks.c`,
   `src/utils.c` all use `pthread_*` and need to include it).
2. **`src/state.c`**: confirm `json-c` include path resolves on FreeBSD base
   (it should, but verify).
3. **Subdir `Makefile`s**: confirm each has the right `LIBADD` for
   `pthread`, `crypto`, `jail`, `zfs`, etc.

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
sudo pkg install -y git bmake
# 3. Configure parallel build (max CPU cores)
echo 'MAKE_JOBS=yes'        | sudo tee -a /etc/src.conf
echo 'MAKE_JOBS_NUMBER=6'   | sudo tee -a /etc/src.conf
# 4. Clone the branch
git clone git@github.com:cloudbsdorg/freebsd-src-oci.git
cd freebsd-src-oci
git checkout feature/oci-bootstrap
# 5. Build
cd usr.sbin/ocifbsd
bmake -j6
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

| Date       | Author    | Change                                                              |
| ---------- | --------- | ------------------------------------------------------------------- |
| 2026-06-04 | mlapointe | Initial OCI-STATUS.md created (this file)                           |
| 2026-06-04 | mlapointe | Identified need to demote darwin-* to opt-in cross-build            |
| 2026-06-04 | mlapointe | Documented VM env mods (make.conf empty, src.conf MAKE_JOBS)        |
| 2026-06-03 | mlapointe | Security audit complete (7 commits, all CRITICAL/HIGH/MEDIUM fixed) |
| 2026-06-03 | mlapointe | Rebased onto 17 new origin/main commits, force-pushed clean         |
| 2026-06-02 | mlapointe | 65+ commits of OCI bootstrap work pushed to origin/feature/oci-bootstrap |
