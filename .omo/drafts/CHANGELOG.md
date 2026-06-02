# Changelog

All notable changes to `ocifbsd` are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased] - feature/oci-bootstrap

### Added

#### OCI Source Code (Wave 1)
- 77 .c/.h source files (33,182 lines, 1.1 MB) cherry-picked from `origin/devel`
- 6 top-level modules: `api/`, `cert/`, `clustering/`, `convert/`, `export/`, `gc/`
- 12 sub-modules: `image/`, `logd/`, `metrics/`, `namespace/`, `network/`, `orchestration/`
- Security modules: `security/` (RCTL, MAC labels), `security-daemon/` (RBAC, secrets, TLS)
- Optional modules: `tpm/`, `pam/`
- Core runtime: `ocifbsd.c`, `src/container.c`, `src/oci2jail.c`, `src/state.c`, `src/hooks.c`, `src/utils.c`
- 3 test files in `tests/usr.sbin/ocifbsd/`

#### Build Infrastructure (Wave 1, T8)
- 18 per-module Makefiles in `usr.sbin/ocifbsd/*/`
- Top-level `usr.sbin/ocifbsd/Makefile` with PROG, MAN, SRCS, SUBDIR
- `usr.sbin/Makefile` updated to include `ocifbsd` in SUBDIR
- Dependency on: jail, util, zfs, m, pthread, crypto, json-c

#### AI Slop Audit (Wave 2)
- Comprehensive audit of 96 OCI source files
- Identified 36 TODO/FIXME markers across 11 files
- Identified 5 seccomp references (need capsicum translation)
- Identified 8 json-c references (not in FreeBSD base)
- Created `.omo/drafts/ai-slop-backlog.md` with 12 deferred work items
- Removed `-Werror` from `usr.sbin/ocifbsd/Makefile` (B1 fix)

#### Cross-Build Documentation (Wave 3+4)
- Documented cross-build blockers in `.omo/drafts/`
- `tools/build/make.py` analysis (FreeBSD build driver)
- Identified `lld` as separate Homebrew package (LLVM 19+)
- Identified macOS SDK header leak in `lib/libgcc_eh`
- Identified need for FreeBSD sysroot or VM for full cross-build

#### CLion IDE Configuration (Wave 3+4)
- 2 toolchains in `.idea/tools/Toolchains.xml`
- 6 custom build targets in `.idea/misc.xml`
- 6 run configurations in `.idea/runConfigurations/`
  - Run ocifbsd (default args)
  - Debug ocifbsd (lldb attach)
  - Run create/start/list (subcommand tests)
  - Run tests (ATF/kyua)
  - + 2 shell script configs

#### Plan Documentation (Wave 5)
- `.plan/000.0-OCI-Jail-TOC.md` updated with Document Dependencies
- `.plan/005.0-Risks-TODO.md` state preamble + Phase 0 IN PROGRESS
- `.plan/020.0-Developer-Setup.md` (482 lines, 9 sections, ATF/kyua)
- `.omo/drafts/oci-bootstrap-tasks.md` (149 lines, live tracker)
- `.omo/drafts/vm-provisioning.md` (376 lines, 3 VM backends)
- `.omo/drafts/vm-smoke-test.sh` (POSIX sh, executable)
- `.omo/evidence/` (17 per-task evidence files)
- `.gitignore` updated with 20 patterns

#### Darwin Build Tooling (T27)
- `usr.sbin/ocifbsd/darwin-bootstrap.sh` (289 lines, POSIX sh)
  - 5-step check: Xcode CLT, Homebrew, bmake, LLVM, Python 3
  - Modes: `--check`, `--install`, `--yes`
  - Generates `/tmp/ocifbsd-cross-build-env` with XCC/XLD/XAS env vars
  - Handles LLVM 19+ lld split (separate Homebrew package)
- 3 Makefile targets in `usr.sbin/ocifbsd/Makefile`:
  - `darwin-bootstrap` - run the bootstrap script
  - `darwin-build` - bootstrap + cross-build userland + ocifbsd + tests
  - `darwin-test` - build + scp to VM + run kyua tests

#### Makefile Inspection Targets (this branch)
- `help` - show all targets with descriptions
- `info` - print build environment (host, target, tools)
- `audit` - AI slop markers (TODO/FIXME/HACK + hotspots)
- `lint` - license headers, seccomp, json-c refs
- `smoke` - host-side smoke test (shell/Makefile syntax + license)
- `sources` - file inventory with LOC
- `size` - tree size breakdown
- `docs` - list .plan/ documentation
- `find-hack` - show HACK markers with context
- `find-todos` - TODO/FIXME/XXX grouped by file
- `all-checks` - run all inspection targets, save report
- `release-manifest` - what would go in a release tarball
- `test-cross-build` - cross-build env diagnostic (no build)
- `check-deps` - verify required tools
- `verify` - pre-commit verification
- `dev` - one-command developer setup
- `vm-snapshot`, `vm-restore`, `vm-status` - VM management
- `clean-all` - remove all build artifacts including obj/
- Conditional `.include <src.opts.mk>` and `.include <bsd.prog.mk>` so
  the Makefile works on macOS hosts (where the FreeBSD .mk files don't exist)

#### CI Workflow
- `.github/workflows/ocifbsd-inspection.yml` (96 lines)
  - Runs on macos-latest
  - Installs bmake, llvm, lld via brew
  - Runs smoke, audit, lint, find-hack, find-todos targets
  - Runs all-checks to generate full inspection report
  - Runs test-cross-build diagnostic
  - Uploads inspection report as artifact (30-day retention)

#### Documentation
- `.omo/drafts/TROUBLESHOOTING.md` (271 lines)
  - Quick diagnostics
  - Cross-build from macOS issues
  - Source code issues (TODOs, HACKs, seccomp, json-c)
  - License header format
  - VM testing
  - Make targets reference table
  - State summary table
- `.omo/drafts/CONTRIBUTING.md` (276 lines)
  - Quick start
  - Development workflow
  - Make targets reference
  - Code conventions
  - Anti-patterns (TODO, HACK, seccomp, json-c)
  - Testing
  - Documentation conventions
  - CI
  - Common issues
- `.omo/drafts/ai-slop-backlog.md` (12 deferred work items)

### Changed

#### Build System
- `usr.sbin/ocifbsd/Makefile`: removed `-Werror` (was blocking compile with Klara-era code)
- `usr.sbin/Makefile`: added `ocifbsd` to SUBDIR (line 58, alphabetical)
- `.idea/` configurations: enhanced with OCI-specific build targets and run configs

#### Documentation
- `.plan/000.0-OCI-Jail-TOC.md`: added Document Dependencies section, OCI document rows
- `.plan/005.0-Risks-TODO.md`: added state preamble, Phase 0 IN PROGRESS

### Known Issues

#### Critical
- **macOS SDK header leak** in `lib/libgcc_eh` during cross-build
  - Workaround: Build inside FreeBSD VM (see vm-provisioning.md)
  - Workaround: Use FreeBSD sysroot (complex setup)
  - Status: Documented, not fixed

#### Medium
- **json-c dependency** not in FreeBSD base
  - Status: Deferred (8-16 hours to migrate to libxo or vendor)
- **seccomp → capsicum translation** needed in 3 files
  - Status: Deferred (12-24 hours)

#### Low
- **35 TODO markers** across 5 hotspot files
  - network/network.c: 9
  - security/rctl.c: 7
  - security/mac.c: 5
  - image/zfs_store.c: 4
  - src/container.c: 3
  - Status: Backlogged in ai-slop-backlog.md
- **1 HACK marker** in cert/cert.c:521
  - `strlcpy((char[]){0}, name, 256);` to silence unused warning
  - Status: Documented, not fixed
- **0 SPDX identifiers** (all use traditional BSD-style copyright)
  - Status: Not an issue (BSD 2-Clause is valid)

### State

| Metric | Value |
|--------|------:|
| Branch | `feature/oci-bootstrap` |
| Commits ahead of main | 15 |
| Files added/modified | 129 |
| Lines added | 38,815 |
| Source files (.c/.h) | 77 |
| Total source lines | 33,182 |
| Source size | 1.1 MB |
| Subdirectories | 18 |
| TODO markers | 35 |
| FIXME markers | 0 |
| XXX markers | 0 |
| HACK markers | 1 |
| SPDX coverage | 0/77 (all BSD-style) |
| Copyright coverage | 77/77 |
| Files with seccomp | 3 |
| Files with json-c | 5 |
| Largest file | logd/logd.c (1392 lines) |
| Largest subdir | image/ (8 files, 3906 lines) |

## [0.1.0] - Initial (in origin/devel)

### Added
- Initial OCI runtime implementation by Klara, Inc.
- 96 source files across 16 modules
- 3 test files
- BSD 2-Clause license (Klara, Inc. / FreeBSD Foundation)

---

## Commit History (feature/oci-bootstrap)

```
a6fa8af38ff Add CONTRIBUTING.md with full developer workflow and make target reference
45ce924ebc4 Makefile: add check-deps, dev, verify targets
aee6b09ef3d ci: add ocifbsd-inspection workflow
fe3aafb6521 Makefile: add all-checks, find-hack, find-todos, test-cross-build, release-manifest
f02cb7c0d88 Add TROUBLESHOOTING.md documenting known issues and workarounds
0634072abe4 Makefile: add inspection/audit/lint/docs targets for macOS hosts
bc129da1120 darwin-bootstrap: fix env file generation (set _XLD before heredoc)
ff33ed3cb46 darwin-bootstrap: handle LLVM 19+ lld split (separate package)
9fe48db2a84 Add darwin-build tooling: bootstrap script + make targets
25cba72b6b6 Final Wave: F1-F4 APPROVE (self-review)
49179f2b580 Wave 5: Task tracker, .gitignore, TOC cross-refs (T21-T24)
ff60793b8bc Wave 3+4: Cross-build blockers documented, CLion IDE configured
73a339ff57d Wave 2: Audit OCI source, fix Makefiles, backlog AI slop
f36d37821f9 Wave 1: Bootstrap ocifbsd onto feature/oci-bootstrap branch
0f54df18a11 plan: record oci-bootstrap work plan and interview draft
```
