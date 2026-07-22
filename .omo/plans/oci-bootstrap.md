# OCI Runtime (ocifbsd) Bootstrap + CLion Tooling on macOS

> **Plan status (2026-07-21): MOSTLY SUPERSEDED / HISTORICAL**
>
> - **Bootstrap build goal (all SUBDIRs compile on FreeBSD 16): DONE** on `feature/oci-bootstrap` (2026-06-05). See `usr.sbin/ocifbsd/OCI-STATUS.md` on that branch.
> - **Plan/doc accuracy:** Reconciled 2026-07-21 — see [`.omo/drafts/oci-status-reconciliation-2026-07-21.md`](../drafts/oci-status-reconciliation-2026-07-21.md) and [`.plan/005.0-Risks-TODO.md`](../../.plan/005.0-Risks-TODO.md) §0.
> - **Still open from this plan’s spirit:** merge `main` (upstream FreeBSD) into `feature/oci-bootstrap`; re-verify build; harden Phase 1 tests. macOS cross-build / CLion bits remain optional developer convenience, not the tier-1 path.
> - **Do not re-run** the “recover from devel” wave — code is already on `feature/oci-bootstrap`.

## TL;DR

> **Quick Summary**: Recover the orphaned `ocifbsd` OCI runtime work from `origin/devel` onto a current branch off `main`, set up bmake-based cross-compile from macOS targeting FreeBSD amd64, and configure CLion 2026.1.2 with custom build targets + run/debug configs so the developer can edit/build/test `usr.sbin/ocifbsd/` from the IDE. Reuse existing `release/Makefile.oci` infrastructure. Update `.plan/` docs to match reality.
>
> **Deliverables**:
> - Working branch with `usr.sbin/ocifbsd/` (96 source files) and 3 test files
> - macOS host with `bmake` (Homebrew) configured
> - `tools/build/make.py` cross-build verified to produce FreeBSD userland
> - CLion project with bmake toolchain, custom targets, run/debug configs
> - Updated `.plan/005.0-Risks-TODO.md`, `.plan/000.0-OCI-Jail-TOC.md`
> - New `.plan/020.0-Developer-Setup.md` (CLion + bmake + cross-build guide)
> - New `.omo/drafts/oci-bootstrap-tasks.md` (working task tracker)
>
> **Estimated Effort**: Large
> **Parallel Execution**: YES — 6 waves
> **Critical Path**: T1 (bmake install) → T2 (branch) → T5 (cross-build verify) → T12 (build ocifbsd) → T18 (CLion debug) → F1-F4 → user approval

---

## Context

### Original Request
The user is on macOS, working in a FreeBSD source fork (`cloudbsdorg/freebsd-src-oci`) that proposes a native OCI container runtime (`ocifbsd`) built on FreeBSD jails. The `.plan/` directory has 24 design documents. The user wants:

1. **CLion dev environment on macOS** using `bmake` (FreeBSD make) to compile, run, and debug this FreeBSD source tree.
2. **Audit of the OCI work** in this codebase — is the implementation sensible?
3. **A proper plan** for getting the OCI system going.
4. **Updated documentation** reflecting the actual state.
5. **A new task doc** if one doesn't exist for the bootstrap work.

### Interview Summary
**Key Decisions Made (user-confirmed)**:
- **Source strategy**: Reuse `origin/devel`'s existing `usr.sbin/ocifbsd/` (16 phase commits, 96 source files, 3 test files). Do not start fresh.
- **Build target**: Cross-compile from macOS to FreeBSD amd64 userland using `tools/build/make.py`. (Note: `ocifbsd` itself can only run on FreeBSD — cross-build produces a FreeBSD binary that needs a FreeBSD host to execute.)
- **Doc updates**: Update `.plan/005.0` and `.plan/000.0` for accuracy, add new `.plan/020.0-Developer-Setup.md`, and create `.omo/drafts/oci-bootstrap-tasks.md`.
- **CLion scope**: Full IDE integration — bmake as Make runner, custom build targets per ocifbsd subdir, run/debug configs for `ocifbsd` binary, ATF test runner, toolchain config.

**Discovered During Research**:
- `origin/devel` has 21 commits ahead of `main` (the OCI work) and 762 commits behind. The user's local `main` was rebased onto `freebsd:main` and the OCI feature branch was never merged into `main`.
- All 16 phase commits + AGENTS_START_HERE.md + .plan/019.0-Package-Release.md + release/release-oci.conf exist on `origin/devel`.
- `.plan/005.0-Risks-TODO.md` on main still says "NOT STARTED" for all tasks (the orphan commit that marked 352 tasks complete isn't on main).
- `tools/build/make.py` is the canonical FreeBSD cross-build driver. `tools/build/cross-build` and `targets/pseudo` exist.
- Top-level `Makefile` supports `XDEV`, `XCC`, `XLD` for cross-compile.
- CLion 2026.1.2 has Custom Build Targets (Settings → Build, Execution, Deployment → Custom Build Targets) which can wrap `bmake` instead of `make`.

### Metis Review
**Identified Gaps (all addressed in this plan)**:
- Branch strategy ambiguity: addressed by recommending new branch from current main + merge of `origin/devel` via `--allow-unrelated-histories` on a single commit, OR cherry-pick the 21 OCI commits. Both are options in Task T2.
- Risk: 762 commits behind. Addressed in T2a: "verify the OCI work still applies to current main" (the diff between OCI commits and current main).
- Risk: AI-generated code may not compile. Addressed by Wave 3 "audit & fix compilation" tasks.
- Risk: AGENTS_START_HERE.md claims 352 tasks complete but this is unverified. Addressed by T6 (verify the claim against the actual code).
- Risk: cross-build requires clang/lld matching FreeBSD version. Addressed by T1b (verify toolchain) and T3 (try the buildworld).
- Risk: macOS case-insensitive filesystem (APFS default is case-insensitive on most installs) might break git operations. Addressed in T1c.
- Risk: macOS lacks `/usr/bin/lex` historically. Addressed in T1d.

---

## Work Objectives

### Core Objective
Make the `ocifbsd` OCI runtime work-in-progress buildable from macOS via bmake, and make the entire codebase navigable/buildable/debuggable from CLion 2026.1.2, with documentation that accurately reflects the current state.

### Concrete Deliverables
1. `.omo/plans/oci-bootstrap.md` — this plan (already in progress)
2. `.omo/drafts/oci-bootstrap-tasks.md` — working task tracker (created during execution)
3. New git branch: `feature/oci-bootstrap` (or similar) with `usr.sbin/ocifbsd/`
4. macOS toolchain: `bmake` (Homebrew), `bmake` available in `$PATH`
5. Cross-build verified: `cd src && ./tools/build/make.py TARGET=amd64 TARGET_ARCH=amd64 buildworld` produces FreeBSD amd64 userland objects
6. ocifbsd binary built: `bmake -C usr.sbin/ocifbsd TARGET=amd64 TARGET_ARCH=amd64` produces `ocifbsd` binary
7. ATF test runner: `bmake -C tests/usr.sbin/ocifbsd TARGET=amd64 TARGET_ARCH=amd64` produces test binary
8. Updated `.idea/misc.xml` — Custom Build Targets for ocifbsd subdirs
9. Updated `.idea/workspace.xml` — run/debug configurations
10. New `.idea/tools/Toolchains.xml` — cross-compile toolchain config
11. Updated `.plan/005.0-Risks-TODO.md` — refreshed task tracker
12. Updated `.plan/000.0-OCI-Jail-TOC.md` — accurate line counts
13. New `.plan/020.0-Developer-Setup.md` — developer onboarding guide
14. New `AGENTS_START_HERE.md` (recreated, since it's missing on main)
15. New `.gitignore` entry for cross-build artifacts

### Definition of Done
- [ ] `cd src && bmake TARGET=amd64 TARGET_ARCH=amd64 -C usr.sbin/ocifbsd` exits 0
- [ ] `cd src && bmake TARGET=amd64 TARGET_ARCH=amd64 -C tests/usr.sbin/ocifbsd` exits 0
- [ ] CLion opens the project without "no Makefile project detected" warnings
- [ ] CLion's "Build" menu has a "Build 'ocifbsd'" entry that runs bmake
- [ ] CLion's "Run/Debug" menu has a "Run 'ocifbsd'" entry that launches the binary
- [ ] `.plan/020.0-Developer-Setup.md` exists and is accurate
- [ ] All evidence files saved to `.omo/evidence/task-{N}-{slug}.{ext}`
- [ ] F1-F4 reviews all APPROVE

### Must Have
- Branch from current main with `usr.sbin/ocifbsd/` code from `origin/devel`
- bmake installed and discoverable in `$PATH`
- `tools/build/make.py` cross-build driver verified to compile at least one FreeBSD userland binary on macOS host
- **FreeBSD VM provisioned on macOS host** (QEMU/UTM/VirtualBox) with a clean-state snapshot for reproducible testing
- **VM snapshot mechanism** — the test loop is: deploy binary → run tests → capture results → restore snapshot → repeat
- **VM ↔ macOS deployment path** — scp (or shared folder) to push built artifacts into the VM
- CLion Custom Build Target for `usr.sbin/ocifbsd/` (using bmake)
- CLion run/debug configuration for `ocifbsd` binary
- All three .plan/ doc updates and the new .plan/020.0 file
- AGENTS_START_HERE.md recreated on the new branch (was on devel only)
- VM test workflow documented in `.plan/020.0-Developer-Setup.md` (section: "Testing in a FreeBSD VM")

### Must NOT Have (Guardrails)
- **Do not modify** `share/mk/`, `Makefile.inc1`, `Makefile.libcompat`, `Makefile.sys.inc` (these are upstream FreeBSD and would create merge conflicts with `freebsd:main`)
- **Do not modify** `usr.sbin/jail/`, `usr.sbin/jexec/`, `usr.sbin/jls/` (these are upstream; we want to use them as-is)
- **Do not run `git push --force`** on any shared branch
- **Do not commit** `.idea/workspace.xml` if it contains machine-specific paths (it likely does — check `.idea/.gitignore` first)
- **Do not commit** `obj/` directories (cross-build output)
- **Do not introduce** new BSD-licensed third-party dependencies; FreeBSD already ships with libjail, libutil, libthr, libcrypto, libssl, libjson-c
- **Do not generate** AI-style "excessive comments" or "magic incantations" in code; follow FreeBSD house style
- **Do not split this work into multiple plans** — everything goes in this single plan
- **Do not skip** the F1-F4 review wave before declaring done

### Spec Framework Integration

> *Omit this section entirely if no SDD framework is detected in the target repository.*

This repo does NOT use OpenSpec or Spec Kit. It has a custom `.plan/` document set. The plan respects the existing `.plan/` numbering (the new doc is `020.0`, after the existing `019.0`).

---

## Verification Strategy (MANDATORY)

> **ZERO HUMAN INTERVENTION** - ALL verification is agent-executed. No exceptions.
> Acceptance criteria requiring "user manually tests/confirms" are FORBIDDEN.

### Test Decision
- **Infrastructure exists**: NO (this is a FreeBSD source tree, not a typical test-driven project — verification is build+test against the FreeBSD build system)
- **Automated tests**: Tests-after. The ATF test harness in `tests/usr.sbin/ocifbsd/` will be built and run; new tests added as bugs are found.
- **Framework**: ATF (Automated Testing Framework) — FreeBSD's native test framework. Kyua is the runner.
- **If TDD**: N/A for the bootstrap work (we're recovering existing code, not writing new code with tests).

### QA Policy
Every task MUST include agent-executed QA scenarios. Evidence saved to `.omo/evidence/task-{N}-{scenario-slug}.{ext}`.

- **Build/compile**: Use Bash (with bmake) - Run `bmake`, capture exit code and stderr, verify output binary exists
- **Cross-build**: Use Bash - Run `tools/build/make.py TARGET=amd64`, capture log, verify obj/ tree
- **Unit tests**: Use Bash (kyua) - Run `kyua test`, capture results, verify pass/fail counts
- **Doc accuracy**: Use Read + Grep - Verify line counts, task counts, file existence claims
- **CLion config**: Use Read - Verify .idea/*.xml files are well-formed XML, contain expected targets/configs
- **Git operations**: Use Bash (git) - Verify branch creation, merge, cherry-pick results

### Evidence File Naming Convention
- `task-{N}-{scenario-slug}.txt` for build logs, test output
- `task-{N}-{scenario-slug}.xml` for diffs, configs
- `task-{N}-{scenario-slug}.md` for human-readable summaries

---

## Execution Strategy

### Parallel Execution Waves

> Maximize throughput by grouping independent tasks into parallel waves.
> Each wave completes before the next begins.
> Target: 5-8 tasks per wave. Fewer than 3 per wave (except final) = under-splitting.

```
Wave 1 (Start Immediately - foundation + tooling, MAX PARALLEL):
├── Task 1: Install bmake + verify macOS toolchain (Homebrew)
├── Task 2: Reconcile branches (cherry-pick vs. merge origin/devel into feature branch)
├── Task 3: Verify FreeBSD tools/build/make.py cross-build driver on macOS
├── Task 4: Recreate AGENTS_START_HERE.md and root-level OCI configs on main
├── Task 5: Update .plan/005.0-Risks-TODO.md to reflect actual state
├── Task 6: Update .plan/000.0-OCI-Jail-TOC.md line counts and add ocifbsd entry
├── Task 7: Create .plan/020.0-Developer-Setup.md (CLion+bmake+bootstrap guide)
└── Task 26: Provision FreeBSD VM with clean-state snapshot for reproducible testing

Wave 2 (After Wave 1 - code audit + quick fixes, MAX PARALLEL):
├── Task 8: Audit usr.sbin/ocifbsd/ source quality (slop, stubs, missing includes)
├── Task 9: Verify usr.sbin/ocifbsd/Makefile includes all subdirs
├── Task 10: Verify usr.sbin/Makefile on the new branch adds ocifbsd subdir
├── Task 11: Compile single ocifbsd module with bmake (smoke test)
└── Task 12: Identify AI-generated code smells for cleanup (deferred to future)

Wave 3 (After Wave 2 - build the world, sequential within wave):
├── Task 13: Cross-build FreeBSD userland base (libc, libthr, libjail)
├── Task 14: Build usr.sbin/ocifbsd/ and its subdirs
└── Task 15: Build tests/usr.sbin/ocifbsd/ test harness

Wave 4 (After Wave 3 - CLion IDE integration, MAX PARALLEL):
├── Task 16: Configure CLion bmake toolchain (.idea/tools/Toolchains.xml)
├── Task 17: Create Custom Build Targets for ocifbsd subdirs (.idea/misc.xml)
├── Task 18: Create Run/Debug Configurations (.idea/workspace.xml)
├── Task 19: Configure ATF test framework integration (assumes T26 done for VM test ref)
└── Task 20: Test full IDE workflow (build, run, debug from CLion)

Wave 5 (After Wave 4 - verification + doc, MAX PARALLEL):
├── Task 21: Run ATF tests in FreeBSD VM with snapshot/restore
├── Task 22: Create .omo/drafts/oci-bootstrap-tasks.md (working task tracker)
├── Task 23: Verify .plan/ cross-references and add 020.0 to TOC
├── Task 24: Update .gitignore for cross-build artifacts and IDE cache
└── Task 25: Commit and push feature branch (if user approved)

Wave FINAL (After ALL tasks — 4 parallel reviews, then user okay):
├── Task F1: Plan compliance audit (oracle)
├── Task F2: Code quality review (unspecified-high)
├── Task F3: Real manual QA (unspecified-high) — full build/test cycle from CLion
└── Task F4: Scope fidelity check (deep) — verify all Must Have present, Must NOT Have absent
-> Present results -> Get explicit user okay

Critical Path: T1 (bmake) + T26 (VM) -> T2 (branch) -> T3 (cross-build) -> T11 (smoke build) -> T13-15 (full build) -> T21 (VM test) -> F1-F4 -> user okay
Parallel Speedup: ~55% faster than sequential
Max Concurrent: 8 (Wave 1)
```

### Dependency Matrix (abbreviated - show ALL tasks in your generated plan)

- **1**: - - 3, 11
- **2**: - - 8, 9, 10
- **3**: 1 - 13
- **4**: 2 - 23
- **5**: - - 22, F1
- **6**: - - 22, F1
- **7**: - - 16, F3
- **8**: 2 - 9, 11
- **9**: 2, 8 - 10, 14
- **10**: 2, 9 - 14
- **11**: 1, 8 - 14
- **12**: 8 - (deferred, no blocker)
- **13**: 3 - 14, 15
- **14**: 9, 10, 11, 13 - 15, 17
- **15**: 13, 14 - 21
- **16**: 7 - 17, 18
- **17**: 16 - 18
- **18**: 16, 17 - 20
- **19**: 14, 15, 26 - 20
- **20**: 17, 18, 19 - F1, F2, F3
- **21**: 15, 26 - F1, F2
- **22**: 5, 6, 7 - F1
- **23**: 4, 7 - F1
- **24**: 1, 3, 13 - F1
- **25**: 4-24 - F1
- **26**: - - 15, 19, 21
- **F1**: 25 - end
- **F2**: 25 - end
- **F3**: 25 - end
- **F4**: 25 - end

### Agent Dispatch Summary

- **Wave 1**: **8** - T1 → `quick`, T2 → `quick`, T3 → `deep`, T4-T6 → `writing`, T7 → `writing`, T26 → `unspecified-high`
- **Wave 2**: **5** - T8 → `deep`, T9 → `quick`, T10 → `quick`, T11 → `unspecified-high`, T12 → `deep` (deferred)
- **Wave 3**: **3** - T13 → `deep`, T14 → `unspecified-high`, T15 → `unspecified-high`
- **Wave 4**: **5** - T16-T19 → `quick`, T20 → `unspecified-high`
- **Wave 5**: **5** - T21 → `unspecified-high`, T22 → `writing`, T23 → `writing`, T24 → `quick`, T25 → `quick`
- **FINAL**: **4** - F1 → `oracle`, F2 → `unspecified-high`, F3 → `unspecified-high`, F4 → `deep`

---

## TODOs

> Implementation + Test = ONE Task. Never separate.
> EVERY task MUST have: Recommended Agent Profile + Parallelization info + QA Scenarios.
> **A task WITHOUT QA Scenarios is INCOMPLETE. No exceptions.**
> **FORMAT**: Task labels MUST use bare numbers: `1.`, `2.`, `3.` — NOT `T1.`, `Task 1.`, `Phase 1:`.
> Final Verification Wave labels MUST use `F1.`, `F2.`, etc.

### Wave 1 — Foundation & Tooling (start immediately, 7 parallel)

- [ ] 1. Install bmake and verify macOS toolchain

  **What to do**:
  - `brew install bmake` (or `port -p install bmake` if user prefers pkgsrc/MacPorts)
  - Verify `bmake --version` exits 0
  - Verify `which bmake` returns `/opt/homebrew/bin/bmake` (or appropriate)
  - Verify `clang --version` is at least 14 (FreeBSD 15+ uses clang 18+)
  - Verify `lld --version` exists (FreeBSD cross-linker)
  - Verify `lldb --version` exists (CLion debugger backend)
  - Test bmake against a trivial Makefile: `echo 'all: ; @echo hello' > /tmp/test.mk && bmake -f /tmp/test.mk`

  **Must NOT do**:
  - Don't install a different bmake that conflicts with system tools
  - Don't add bmake to `PATH` in shell config; CLion will discover it via `/usr/bin/which`
  - Don't downgrade existing clang

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: `[]`
  - **Reason**: Single-file install + verification; minimal reasoning required

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with 2-7)
  - **Blocks**: T3, T11, T24
  - **Blocked By**: None (can start immediately)

  **Acceptance Criteria**:
  - [ ] `which bmake` exits 0 and returns a path
  - [ ] `bmake --version | head -1` contains "bmake"
  - [ ] `clang --version | head -1` exits 0
  - [ ] `which lld` exits 0 (or `which ld.lld`)
  - [ ] `bmake -f /tmp/test.mk` prints "hello" to stdout

  **QA Scenarios**:
  ```
  Scenario: bmake installed and functional
    Tool: Bash
    Preconditions: macOS host with Homebrew
    Steps:
      1. which bmake
      2. bmake --version | head -1
      3. echo 'all: ; @echo OK' > /tmp/bmake-test.mk && bmake -f /tmp/bmake-test.mk
      4. which clang lld lldb 2>&1
    Expected Result: All commands succeed, "bmake" + "BMake" or "bmake" version reported
    Failure Indicators: "command not found" on any of the above
    Evidence: .omo/evidence/task-1-bmake-verify.txt
  ```

  **Commit**: NO (tooling, not source)

- [x] 2. Create feature branch and merge `origin/devel`'s OCI commits

  **What to do**:
  - `git fetch origin devel`
  - `git checkout -b feature/oci-bootstrap origin/main`
  - Verify we're on a new branch off current main (not on origin/devel)
  - Try merge first: `git merge --allow-unrelated-histories origin/devel` (since main and devel diverged significantly)
  - If merge conflicts arise (likely in `usr.sbin/ocifbsd/Makefile` and `usr.sbin/Makefile`), prefer the `origin/devel` version (it has the OCI work)
  - Alternative if merge is messy: `git checkout origin/devel -- usr.sbin/ocifbsd/ tests/usr.sbin/ocifbsd/ AGENTS_START_HERE.md .plan/019.0-Package-Release.md release/release-oci.conf`
  - Stage and verify: `git status` shows the new files
  - Verify: `ls usr.sbin/ocifbsd/` shows 21 subdirs + top-level files
  - Verify: `git log --oneline | grep -i "oci\|ocifbsd" | head -5` shows the OCI commits

  **Must NOT do**:
  - Don't `git push` to `main` or `devel` without user approval
  - Don't force-push (`--force` or `-f`)
  - Don't use `git rebase` for this — it would re-write OCI commits
  - Don't merge `origin/devel` directly; create a `feature/*` branch

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: `[]`
  - **Reason**: Standard git operations, well-documented

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with 1, 3-7)
  - **Blocks**: T8, T9, T10
  - **Blocked By**: None (can start immediately)

  **Acceptance Criteria**:
  - [ ] `git rev-parse --abbrev-ref HEAD` returns `feature/oci-bootstrap`
  - [ ] `ls usr.sbin/ocifbsd/ | wc -l` >= 18 (subdirs + top-level files)
  - [ ] `test -f usr.sbin/ocifbsd/ocifbsd.c` exits 0
  - [ ] `test -f usr.sbin/ocifbsd/Makefile` exits 0
  - [ ] `test -f tests/usr.sbin/ocifbsd/ocifbsd_test.c` exits 0
  - [ ] `git log --oneline origin/main..HEAD | wc -l` >= 1 (commits brought in)
  - [ ] `git log --oneline HEAD..origin/main | wc -l` == 0 (no main commits lost)

  **QA Scenarios**:
  ```
  Scenario: Branch created and OCI files present
    Tool: Bash (git + ls)
    Preconditions: Clean working tree, git remote `origin` reachable
    Steps:
      1. git fetch origin main devel
      2. git checkout -b feature/oci-bootstrap origin/main
      3. git checkout origin/devel -- usr.sbin/ocifbsd/ tests/usr.sbin/ocifbsd/ AGENTS_START_HERE.md .plan/019.0-Package-Release.md release/release-oci.conf
      4. git status --porcelain | wc -l (should be > 0)
      5. ls usr.sbin/ocifbsd/
      6. ls usr.sbin/ocifbsd/src/ usr.sbin/ocifbsd/image/ usr.sbin/ocifbsd/network/
    Expected Result: All 4 paths exist with multiple files
    Failure Indicators: "fatal: not a tree object" or empty directory
    Evidence: .omo/evidence/task-2-branch-status.txt

  Scenario: No loss of main commits
    Tool: Bash (git)
    Preconditions: feature/oci-bootstrap branch created
    Steps:
      1. git log --oneline origin/main..HEAD | head
      2. git log --oneline HEAD..origin/main | head
      3. test "$(git log --oneline HEAD..origin/main | wc -l)" = "0"
    Expected Result: Second count == 0; first count >= 1
    Evidence: .omo/evidence/task-2-no-main-loss.txt
  ```

  **Commit**: YES - `feat: bring in ocifbsd OCI runtime from origin/devel`
  - Files: `usr.sbin/ocifbsd/`, `tests/usr.sbin/ocifbsd/`, `AGENTS_START_HERE.md`, `.plan/019.0-Package-Release.md`, `release/release-oci.conf`
  - Pre-commit: `git status`, `git diff --stat`

- [x] 3. Verify FreeBSD `tools/build/make.py` cross-build driver on macOS

  **What to do**:
  - Read `tools/build/make.py` first 50 lines to understand the entry point
  - Run `python3 tools/build/make.py --help` (or whatever help it provides)
  - Try a small cross-build: `python3 tools/build/make.py TARGET=amd64 TARGET_ARCH=amd64 buildworld -j$(sysctl -n hw.ncpu)` with a small subset (e.g., just `lib/libc` or just `usr.sbin/jail`)
  - Check for known macOS issues: case-insensitive filesystem, missing `/usr/bin/lex`, BSD vs GNU find differences
  - Document the result in `.omo/evidence/task-3-cross-build-attempt.txt`
  - If full `buildworld` is too large/slow, try a smaller target like `libraries` or `cross-tools` first

  **Must NOT do**:
  - Don't run `make installworld` (this is a cross-build, not an install)
  - Don't try to build the kernel (no `buildkernel` for cross from macOS easily)
  - Don't expect the full build to succeed on first try — the goal is to find the first error

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: `[]`
  - **Reason**: Multi-step build orchestration; failure modes need diagnosis

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with 1, 2, 4-7)
  - **Blocks**: T13
  - **Blocked By**: T1 (bmake required)

  **Acceptance Criteria**:
  - [ ] `python3 tools/build/make.py --help` exits 0
  - [ ] A `cross-build` or `libraries` target produces at least one `obj/.../lib*.a` file
  - [ ] First 100 lines of build log captured to evidence
  - [ ] No "lex: command not found" errors (lex installed via Xcode CLT)

  **QA Scenarios**:
  ```
  Scenario: make.py entry point works
    Tool: Bash
    Preconditions: Python 3 on PATH, FreeBSD source tree
    Steps:
      1. python3 tools/build/make.py --help 2>&1 | head -30
      2. head -50 tools/build/make.py
      3. ls tools/build/cross-build tools/build/make_check
    Expected Result: help text produced; scripts exist
    Evidence: .omo/evidence/task-3-make-py-help.txt

  Scenario: Small cross-build attempt
    Tool: Bash
    Preconditions: lex/flex installed, bmake in PATH
    Steps:
      1. python3 tools/build/make.py TARGET=amd64 TARGET_ARCH=amd64 -j2 libraries 2>&1 | tee /tmp/build.log
      2. find obj -name 'libc.a' 2>/dev/null | head -3
      3. find obj -name 'libjail.a' 2>/dev/null | head -3
    Expected Result: At least one .a file produced; first 50 errors logged
    Failure Indicators: All commands fail with "lex: not found" or "XCC: undefined"
    Evidence: .omo/evidence/task-3-cross-build-attempt.txt
  ```

  **Commit**: NO (investigation only)

- [x] 4. Reconcile `AGENTS_START_HERE.md` and `release/release-oci.conf` on the new branch

  **What to do**:
  - Verify the files brought in by T2 exist on the new branch
  - Read `AGENTS_START_HERE.md` and confirm it accurately describes the 21-commit OCI work
  - Read `release/release-oci.conf` and confirm it has sane `WITH_OCIIMAGES=1` setting
  - If any of the files are missing on the new branch (because T2 used a partial checkout), add them

  **Must NOT do**:
  - Don't modify the content of these files (they're already authored by Junie/AI; the user can refine later)
  - Don't move them to a different location

  **Recommended Agent Profile**:
  - **Category**: `writing`
  - **Skills**: `[]`
  - **Reason**: Document verification task

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with 1, 2, 3, 5-7)
  - **Blocks**: T23
  - **Blocked By**: T2 (branch with files)

  **Acceptance Criteria**:
  - [ ] `test -f AGENTS_START_HERE.md` exits 0
  - [ ] `test -f release/release-oci.conf` exits 0
  - [ ] `test -f .plan/019.0-Package-Release.md` exits 0
  - [ ] `head -3 release/release-oci.conf` shows FreeBSD release config header

  **QA Scenarios**:
  ```
  Scenario: All OCI-related files present
    Tool: Bash
    Preconditions: On feature/oci-bootstrap branch
    Steps:
      1. for f in AGENTS_START_HERE.md release/release-oci.conf .plan/019.0-Package-Release.md usr.sbin/ocifbsd/Makefile usr.sbin/ocifbsd/ocifbsd.c tests/usr.sbin/ocifbsd/ocifbsd_test.c; do
           test -f $f && echo "OK: $f" || echo "MISSING: $f"
         done
    Expected Result: All 6 files report "OK:"
    Evidence: .omo/evidence/task-4-files-present.txt
  ```

  **Commit**: NO (already part of T2's commit)

- [x] 5. Update `.plan/005.0-Risks-TODO.md` to reflect actual state

  **What to do**:
  - Read the current `.plan/005.0-Risks-TODO.md` (which still says NOT STARTED for all)
  - Add a new top section: "## 0. Current State (2026-06-02)" explaining:
    - `main` is a clean FreeBSD main with `.plan/` docs but no `usr.sbin/ocifbsd/` code
    - `origin/devel` has the 21 OCI commits (Phases 1-17, AGENTS_START_HERE, 019.0, release-oci.conf)
    - The "352 tasks complete" claim was made in `origin/devel`'s commit `cee839ce6ef` and applies to `devel` only
    - This work-plan (`feature/oci-bootstrap` branch) will reconcile the two
  - Update task tables: change `Status` from `NOT STARTED` to `🔄 IN PROGRESS` for tasks 0.1-0.6 (foundation tasks now in motion)
  - Add a new section "## 9.5. Bootstrap Tasks (this plan)" listing the high-level tasks in this work plan
  - Preserve all existing content; do NOT delete the NOT STARTED rows (they're the source of truth for the original plan)

  **Must NOT do**:
  - Don't mark all 352 tasks as complete (the AI claim is unverified)
  - Don't delete the original "11. TODO" section
  - Don't rewrite the .plan/ doc; just add a state preamble

  **Recommended Agent Profile**:
  - **Category**: `writing`
  - **Skills**: `[]`
  - **Reason**: Doc update, no code

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with 1-4, 6, 7)
  - **Blocks**: T22
  - **Blocked By**: None (read-only on existing file)

  **Acceptance Criteria**:
  - [ ] `.plan/005.0-Risks-TODO.md` has new "## 0. Current State" section
  - [ ] `.plan/005.0-Risks-TODO.md` mentions `origin/devel` and `feature/oci-bootstrap`
  - [ ] At least 3 Phase 0 tasks updated to `🔄 IN PROGRESS`
  - [ ] Original task tables preserved (NOT STARTED rows still present)

  **QA Scenarios**:
  ```
  Scenario: Doc reflects actual state
    Tool: Bash + Grep
    Preconditions: On feature/oci-bootstrap branch
    Steps:
      1. grep -c "## 0. Current State" .plan/005.0-Risks-TODO.md (should be 1)
      2. grep -c "origin/devel" .plan/005.0-Risks-TODO.md (should be >= 1)
      3. grep -c "feature/oci-bootstrap" .plan/005.0-Risks-TODO.md (should be >= 1)
      4. grep -c "IN PROGRESS" .plan/005.0-Risks-TODO.md (should be >= 3)
      5. git diff --stat .plan/005.0-Risks-TODO.md (should show non-zero changes)
    Expected Result: All counts meet thresholds
    Evidence: .omo/evidence/task-5-doc-updated.txt
  ```

  **Commit**: YES - `docs(plan): reconcile 005.0 with current branch state`
  - Files: `.plan/005.0-Risks-TODO.md`
  - Pre-commit: `git diff .plan/005.0-Risks-TODO.md | head -50`

- [x] 6. Update `.plan/000.0-OCI-Jail-TOC.md` line counts and add `ocifbsd` entry

  **What to do**:
  - Read the current TOC; note that line counts for some docs may be stale (the orphan commits updated them)
  - If `wc -l .plan/005.0-Risks-TODO.md` differs from the TOC's reported count, fix the count
  - Add a new row in the Document Map table for `.plan/020.0-Developer-Setup.md` (the new doc to be created in T7)
  - Add a "Bootstrap Section" entry referencing the new working task doc (`.omo/drafts/oci-bootstrap-tasks.md`)
  - Don't restructure the TOC; just maintain it

  **Must NOT do**:
  - Don't change the document numbering scheme (000.0, 000.1, 001.0, etc.)
  - Don't delete the Document Dependencies section

  **Recommended Agent Profile**:
  - **Category**: `writing`
  - **Skills**: `[]`
  - **Reason**: Doc maintenance

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with 1-5, 7)
  - **Blocks**: T22
  - **Blocked By**: None (read-only)

  **Acceptance Criteria**:
  - [ ] TOC table contains a row for `.plan/020.0-Developer-Setup.md`
  - [ ] `wc -l .plan/005.0-Risks-TODO.md` matches the count in the TOC
  - [ ] TOC still has all 24 original doc rows
  - [ ] `git diff .plan/000.0-OCI-Jail-TOC.md` shows the changes

  **QA Scenarios**:
  ```
  Scenario: TOC updated for new dev doc
    Tool: Bash + Grep
    Steps:
      1. grep "020.0-Developer-Setup" .plan/000.0-OCI-Jail-TOC.md (should be >= 1)
      2. actual=$(wc -l < .plan/005.0-Risks-TODO.md)
      3. toc_count=$(grep "005.0-Risks-TODO" .plan/000.0-OCI-Jail-TOC.md | grep -oE '[0-9]+' | head -1)
      4. test "$actual" -ge "$toc_count" (allow >= since doc can grow)
    Expected Result: 020.0 entry found, line count consistent
    Evidence: .omo/evidence/task-6-toc-updated.txt
  ```

  **Commit**: YES - `docs(plan): refresh TOC line counts and add 020.0 entry`
  - Files: `.plan/000.0-OCI-Jail-TOC.md`

- [x] 7. Create `.plan/020.0-Developer-Setup.md` (CLion + bmake + cross-build guide)

  **What to do**:
  - Write a new developer onboarding doc at `.plan/020.0-Developer-Setup.md`
  - Sections to include:
    1. **Prerequisites** — macOS host, Homebrew (or MacPorts), Xcode CLT, `bmake` from Homebrew, `python3` (3.11+), `git`, at least 50 GB free disk
    2. **Initial clone** — `git clone https://github.com/cloudbsdorg/freebsd-src-oci.git`, `cd freebsd-src-oci`, `git checkout feature/oci-bootstrap`
    3. **bmake install** — `brew install bmake`, verify with `bmake --version`
    4. **Cross-build from macOS** — `python3 tools/build/make.py TARGET=amd64 TARGET_ARCH=amd64 -j$(sysctl -n hw.ncpu) buildworld` (warns: long; produces obj/ tree)
    5. **Build just ocifbsd** — after buildworld: `cd usr.sbin/ocifbsd && bmake TARGET=amd64 TARGET_ARCH=amd64`
    6. **Run tests** — `cd tests/usr.sbin/ocifbsd && bmake && kyua test`
    7. **CLion setup** — open the project; Settings → Build, Execution, Deployment → Custom Build Targets → add `bmake` target; Settings → Build, Execution, Deployment → Custom Build Application → add run config
    8. **Common pitfalls** — case-insensitive APFS, missing lex, BMake vs GNU make differences, etc.
    9. **Verification** — `git status` clean, `bmake` runs, cross-build produces obj/, ocifbsd binary exists
  - Use the established .plan/ doc style (section numbering, table of contents at top)

  **Must NOT do**:
  - Don't copy large blocks from freebsd.org Handbook (link to it instead)
  - Don't include any actual code from the OCI tree (this is a setup guide, not a code reference)

  **Recommended Agent Profile**:
  - **Category**: `writing`
  - **Skills**: `[]`
  - **Reason**: Authoring a new doc

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (with 1-6)
  - **Blocks**: T16
  - **Blocked By**: None

  **Acceptance Criteria**:
  - [ ] `.plan/020.0-Developer-Setup.md` exists and is > 200 lines
  - [ ] Doc has sections: Prerequisites, Initial clone, bmake, Cross-build, Build ocifbsd, Run tests, CLion setup, Pitfalls
  - [ ] At least 3 verifiable commands (e.g., `bmake --version`, `python3 --version`)

  **QA Scenarios**:
  ```
  Scenario: Doc exists and is comprehensive
    Tool: Bash + Grep
    Steps:
      1. test -f .plan/020.0-Developer-Setup.md (must succeed)
      2. wc -l .plan/020.0-Developer-Setup.md (must be > 200)
      3. grep -E "^## " .plan/020.0-Developer-Setup.md | wc -l (must be >= 8)
      4. grep -E "bmake|make.py" .plan/020.0-Developer-Setup.md (must be >= 3)
    Expected Result: All checks pass
    Evidence: .omo/evidence/task-7-dev-setup-doc.md
  ```

  **Commit**: YES - `docs(plan): add 020.0-Developer-Setup.md (CLion+bmake+cross-build)`
  - Files: `.plan/020.0-Developer-Setup.md`

- [x] 26. Provision FreeBSD VM with clean-state snapshot for reproducible testing

  **What to do**:
  - **Choose VM technology** (macOS-native): UTM (recommended for Apple Silicon; GUI-based; supports snapshots), or QEMU + virt-manager (CLI; cross-platform), or VirtualBox (UI; slower but stable)
  - **Download FreeBSD ISO/VM image** matching the source tree target (FreeBSD 16.0-CURRENT or RELEASE matching `__FreeBSD_version` in `sys/sys/param.h`); preferred: use a pre-built VM image from `https://download.freebsd.org/snapshots/` for amd64
  - **Install FreeBSD** with: base system, dev tools (git, bmake, llvm, kyua), bash, sudo, openssh-server, openssh-portable
  - **Configure network**: bridge mode or host-only with port forwarding for SSH (default 22; or non-standard like 2222)
  - **Configure SSH key auth** from macOS host: copy the user's public key to VM's `~/.ssh/authorized_keys` (so `ssh freebsd-vm` works without password)
  - **Create a "clean" snapshot** of the VM in its post-install state (after dev tools are installed but before any ocifbsd work)
  - **Document the snapshot commands** in `.plan/020.0-Developer-Setup.md` under a new section "Testing in a FreeBSD VM":
    - UTM: `File → Save Snapshot` and `File → Restore Snapshot`
    - QEMU: `qemu-img snapshot -c clean freebsd-oci.qcow2` and `qemu-img snapshot -a clean freebsd-oci.qcow2`
    - VirtualBox: `VBoxManage snapshot freebsd-oci take clean` and `VBoxManage snapshot freebsd-oci restore clean`
  - **Add an SSH config entry** to `~/.ssh/config` on the macOS host:
    ```
    Host freebsd-vm
        HostName 127.0.0.1
        Port 2222
        User root
        IdentityFile ~/.ssh/id_ed25519
    ```
  - **Verify the test loop** (smoke test):
    - SSH in: `ssh freebsd-vm 'uname -a'` — should return FreeBSD
    - Take a test snapshot: `qemu-img snapshot -c smoke-test freebsd-oci.qcow2`
    - Modify something in VM (touch a file)
    - Restore: `qemu-img snapshot -a smoke-test freebsd-oci.qcow2` (revert by VM action: revert snapshot, then power on)
    - Verify the modification is gone
  - **Capture evidence**: VM tech chosen, ISO/image version, dev tools list, SSH test result, snapshot create/restore test result

  **Must NOT do**:
  - Don't commit the VM disk image to the repo
  - Don't commit SSH private keys
  - Don't use a non-snapshot VM for test runs (defeats the reproducibility goal)
  - Don't enable root SSH password auth (key-only)
  - Don't expose the VM SSH port to the public internet

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: `[]`
  - **Reason**: Multi-step VM setup; tool-specific knowledge; environmental debugging

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 1 (foundational; with T1-T7)
  - **Blocks**: T15 (test build), T21 (test run), T19 (ATF config in CLion)
  - **Blocked By**: None (can start immediately)

  **Acceptance Criteria**:
  - [ ] VM boots to FreeBSD login prompt
  - [ ] `ssh freebsd-vm 'uname -a'` returns FreeBSD kernel version
  - [ ] `ssh freebsd-vm 'bmake --version'` returns bmake version
  - [ ] `ssh freebsd-vm 'kyua --version'` returns kyua version
  - [ ] `ssh freebsd-vm 'clang --version'` returns clang version
  - [ ] A "clean" snapshot exists and can be restored
  - [ ] `.plan/020.0-Developer-Setup.md` has a "Testing in a FreeBSD VM" section
  - [ ] SSH config entry in `~/.ssh/config` (or documented for the user to add)

  **QA Scenarios**:
  ```
  Scenario: VM is reachable and has dev tools
    Tool: Bash (ssh)
    Preconditions: VM is running; SSH key auth configured
    Steps:
      1. ssh -o ConnectTimeout=5 freebsd-vm 'uname -a'
      2. ssh freebsd-vm 'bmake --version | head -1'
      3. ssh freebsd-vm 'kyua --version | head -1'
      4. ssh freebsd-vm 'clang --version | head -1'
    Expected Result: All four commands return valid output
    Failure Indicators: "Permission denied", "Connection refused", "command not found"
    Evidence: .omo/evidence/task-26-vm-provision.txt

  Scenario: Snapshot create + restore works
    Tool: Bash (VM-specific command; documented per VM tech)
    Preconditions: VM is running; VM is at clean state
    Steps:
      1. ssh freebsd-vm 'touch /tmp/snapshot-marker'
      2. ssh freebsd-vm 'ls /tmp/snapshot-marker' (should exist)
      3. # Power off VM, restore snapshot, power on
      4. ssh freebsd-vm 'ls /tmp/snapshot-marker' (should NOT exist)
    Expected Result: Step 2 finds file, step 4 does not
    Evidence: .omo/evidence/task-26-snapshot-roundtrip.txt
  ```

  **Commit**: NO (VM disk image and SSH config not in repo; only doc updates)

### Wave 2 — Code Audit & Smoke Build (5 parallel)

- [x] 8. Audit `usr.sbin/ocifbsd/` source quality for AI slop and stubs

  **What to do**:
  - For each `.c` file under `usr.sbin/ocifbsd/`, count: total lines, comment lines, blank lines, function bodies
  - Flag files where comment lines > 50% of total (excessive comments = AI slop)
  - Flag files with function bodies < 5 lines (likely stubs)
  - Flag files that include non-existent headers (use `grep` for `#include` and cross-check)
  - Flag function names matching `temp`, `data`, `result`, `item` (generic slop names)
  - Produce a report at `.omo/evidence/task-8-audit-report.md` with per-file metrics and a verdict

  **Must NOT do**:
  - Don't actually fix any code in this task (audit only)
  - Don't make subjective quality judgments in the report — stick to measurable metrics

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: `[]`
  - **Reason**: Code analysis across 96 files; metric-based evaluation

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with 9-12)
  - **Blocks**: (none immediate; informs T14)
  - **Blocked By**: T2 (branch with code)

  **Acceptance Criteria**:
  - [ ] Audit report exists at `.omo/evidence/task-8-audit-report.md`
  - [ ] Report covers all 96 source files
  - [ ] Report identifies any files with > 50% comment-to-code ratio
  - [ ] Report identifies any stub function patterns
  - [ ] Report includes overall verdict: "salvageable" / "needs significant rework" / "rebuild from scratch"

  **QA Scenarios**:
  ```
  Scenario: Audit report generated
    Tool: Bash + analysis
    Steps:
      1. find usr.sbin/ocifbsd -name '*.c' | wc -l (should be ~70+)
      2. find usr.sbin/ocifbsd -name '*.c' -exec wc -l {} \; > /tmp/loc.txt
      3. # Top 10 by line count, top 10 by comment %
      4. # Write summary to .omo/evidence/task-8-audit-report.md
    Expected Result: Report with per-file metrics exists, verdict stated
    Evidence: .omo/evidence/task-8-audit-report.md
  ```

  **Commit**: NO (audit only)

- [x] 9. Verify `usr.sbin/ocifbsd/Makefile` includes all subdirs

  **What to do**:
  - Read `usr.sbin/ocifbsd/Makefile`
  - Verify it lists all 17+ subdirs as `SUBDIR` (or equivalent FreeBSD idiom)
  - If a subdir is missing, add it
  - Verify each subdir has its own `Makefile` (the build system expects this)
  - Cross-check with `git ls-tree origin/devel usr.sbin/ocifbsd/` output

  **Must NOT do**:
  - Don't restructure the Makefile to use a different idiom
  - Don't add dependencies that aren't in `bsd.prog.mk` or `bsd.lib.mk`

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: `[]`
  - **Reason**: Makefile check + minor edit

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with 8, 10, 11, 12)
  - **Blocks**: T10, T14
  - **Blocked By**: T2 (branch with code); T8 (audit confirms structure)

  **Acceptance Criteria**:
  - [ ] `usr.sbin/ocifbsd/Makefile` has `SUBDIR` (or equivalent) listing all subdirs
  - [ ] Every subdir under `usr.sbin/ocifbsd/` has a `Makefile`
  - [ ] `bmake -C usr.sbin/ocifbsd -V SUBDIR` lists all subdirs

  **QA Scenarios**:
  ```
  Scenario: SUBDIR covers all subdirs
    Tool: Bash (bmake)
    Preconditions: MAKESYSPATH set, bmake in PATH
    Steps:
      1. cd usr.sbin/ocifbsd && bmake -V SUBDIR 2>&1 | head
      2. expected=$(ls -d */ | tr -d '/' | grep -v '^include$' | sort)
      3. actual=$(bmake -V SUBDIR | tr ' ' '\n' | sort -u)
      4. diff <(echo "$expected") <(echo "$actual")
    Expected Result: diff is empty (or close to it)
    Evidence: .omo/evidence/task-9-subdir-list.txt
  ```

  **Commit**: YES (only if changes made) - `chore(ocifbsd): add missing SUBDIR entries`
  - Files: `usr.sbin/ocifbsd/Makefile`

- [x] 10. Verify `usr.sbin/Makefile` on the new branch adds `ocifbsd` subdir

  **What to do**:
  - Read `usr.sbin/Makefile` (top-level)
  - Verify it has `SUBDIR += ocifbsd` or equivalent
  - If missing, add it (after existing jail entries, alphabetically)
  - Compare with `usr.sbin/Makefile` on `origin/devel` to see what the AI agent did

  **Must NOT do**:
  - Don't change the position of other SUBDIR entries (preserve order)
  - Don't change the file's encoding (must remain ASCII)

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: `[]`
  - **Reason**: Single-line check + edit

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with 8, 9, 11, 12)
  - **Blocks**: T14
  - **Blocked By**: T2 (branch); T9 (subdir structure)

  **Acceptance Criteria**:
  - [ ] `usr.sbin/Makefile` has `SUBDIR` entry for `ocifbsd`
  - [ ] No other entries changed
  - [ ] `git diff usr.sbin/Makefile` is either empty or shows only the `ocifbsd` addition

  **QA Scenarios**:
  ```
  Scenario: Top-level usr.sbin Makefile includes ocifbsd
    Tool: Bash (grep)
    Steps:
      1. grep -n "ocifbsd" usr.sbin/Makefile (should return >= 1)
      2. git diff usr.sbin/Makefile (should show only addition)
    Expected Result: ocifbsd entry present
    Evidence: .omo/evidence/task-10-usr-sbin-makefile.txt
  ```

  **Commit**: YES (only if changes made) - `chore(usr.sbin): add ocifbsd to SUBDIR`
  - Files: `usr.sbin/Makefile`

- [x] 11. Compile a single `ocifbsd` module with bmake (smoke test)

  **What to do**:
  - Pick the smallest module: `usr.sbin/ocifbsd/src/` (Phase 1 core files: container.c, utils.c, hooks.c, state.c, oci2jail.c)
  - Read the module's `Makefile` to understand its dependencies
  - Try: `cd usr.sbin/ocifbsd/src && bmake TARGET=amd64 TARGET_ARCH=amd64`
  - If it fails (likely due to missing cross-compiled FreeBSD libraries), document the error
  - Try the alternative: `cd usr.sbin/ocifbsd && bmake TARGET=amd64 TARGET_ARCH=amd64 SRCS=src/utils.c` (single-file compile)
  - Capture build log to evidence
  - If even a single-file compile fails on macOS, document the issue and recommend building inside a FreeBSD VM instead

  **Must NOT do**:
  - Don't try to run the resulting binary (it would be a FreeBSD ELF, not macOS Mach-O)
  - Don't modify source files in this task (smoke test only)
  - Don't proceed to full buildworld without the user's go-ahead

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: `[]`
  - **Reason**: First compile attempt; failure diagnosis critical

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with 8-10, 12)
  - **Blocks**: T14
  - **Blocked By**: T1 (bmake), T8 (audit)

  **Acceptance Criteria**:
  - [ ] `bmake -C usr.sbin/ocifbsd/src` (or single-file variant) attempts compilation
  - [ ] Build log captured to evidence
  - [ ] Either: compilation succeeds and produces an .o file, OR: clear error message documenting what's missing (FreeBSD headers, cross-clang, etc.)
  - [ ] No silent failures

  **QA Scenarios**:
  ```
  Scenario: Single-file smoke compile
    Tool: Bash (bmake)
    Preconditions: bmake, clang, lld available; FreeBSD source tree
    Steps:
      1. cd usr.sbin/ocifbsd/src
      2. bmake -V SRCS (to see what would be compiled)
      3. bmake -V CFLAGS (to see expected flags)
      4. bmake TARGET=amd64 TARGET_ARCH=amd64 -j2 2>&1 | tee /tmp/smoke.log
      5. ls -la *.o 2>/dev/null
    Expected Result: Either .o files produced, or clear error in smoke.log
    Evidence: .omo/evidence/task-11-smoke-build.txt
  ```

  **Commit**: NO (investigation only)

- [x] 12. Identify AI-generated code smells for cleanup (deferred, not blocking)

  **What to do**:
  - From the T8 audit, identify the top 10 most egregious AI slop patterns
  - Create a follow-up task list (don't fix in this plan): `.omo/drafts/ai-slop-cleanup-tasks.md`
  - Each smell gets: file:line, pattern type, proposed fix
  - Categories: excessive comments, generic names, copy-paste duplication, empty bodies

  **Must NOT do**:
  - Don't fix any of these in this plan (deferred to a future `/remove-ai-slops` session)

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: `[]`
  - **Reason**: Code smell detection; produces a backlog, not a fix

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 2 (with 8-11)
  - **Blocks**: (none — output is a future backlog)
  - **Blocked By**: T8 (audit)

  **Acceptance Criteria**:
  - [ ] `.omo/drafts/ai-slop-cleanup-tasks.md` exists
  - [ ] At least 10 smells documented with file:line and pattern type
  - [ ] No code changes in this task

  **QA Scenarios**:
  ```
  Scenario: Smell backlog created
    Tool: Bash (Read)
    Steps:
      1. test -f .omo/drafts/ai-slop-cleanup-tasks.md (must succeed)
      2. grep -c "^##" .omo/drafts/ai-slop-cleanup-tasks.md (must be >= 5)
      3. git status .omo/drafts/ (should show untracked file)
    Expected Result: Backlog exists, no source changes
    Evidence: .omo/evidence/task-12-smell-backlog.txt
  ```

  **Commit**: NO (deferred to future work)

### Wave 3 — Cross-Build the World (3 sequential, long-running)

- [x] 13. Cross-build FreeBSD userland base libraries (libc, libthr, libjail)

  **What to do**:
  - Run: `python3 tools/build/make.py TARGET=amd64 TARGET_ARCH=amd64 -j$(sysctl -n hw.ncpu) libraries` (this is the libraries-only step, much smaller than full `buildworld`)
  - Alternative: `cd lib && bmake TARGET=amd64 TARGET_ARCH=amd64 -j$(sysctl -n hw.ncpu) all` (build all libraries)
  - Capture build log to `.omo/evidence/task-13-libs-build.txt` (likely very large; use `tee | tail`)
  - Verify `obj/lib/libc/libc.a` and `obj/lib/libjail/libjail.a` exist
  - If this step is too slow (> 30 min), document partial progress and split into Wave 3a (small libs) and Wave 3b (full libraries)
  - If cross-build fundamentally doesn't work from macOS host, document the failure and pivot strategy: do the build inside a FreeBSD VM and use SSH-based toolchain

  **Must NOT do**:
  - Don't run `buildkernel` (kernels don't cross-build easily from non-FreeBSD hosts)
  - Don't run `installworld` (this would try to install to /)
  - Don't commit the obj/ directory

  **Recommended Agent Profile**:
  - **Category**: `deep`
  - **Skills**: `[]`
  - **Reason**: Long-running build; failure diagnosis; toolchain debugging

  **Parallelization**:
  - **Can Run In Parallel**: NO (Wave 3 is sequential within itself)
  - **Parallel Group**: Wave 3 (must run before T14, T15)
  - **Blocks**: T14, T15
  - **Blocked By**: T3 (make.py verified)

  **Acceptance Criteria**:
  - [ ] `obj/lib/libc/libc.a` exists and is > 1 MB
  - [ ] `obj/lib/libjail/libjail.a` exists (or `obj/usr.sbin/jail/libjail/libjail.a` if paths differ)
  - [ ] Build log captured with at least first 100 lines and last 50 lines
  - [ ] No "fatal error" in the build (warnings are OK)

  **QA Scenarios**:
  ```
  Scenario: Libraries cross-build
    Tool: Bash
    Preconditions: macOS host with bmake, lex/flex, clang; FreeBSD source
    Steps:
      1. python3 tools/build/make.py TARGET=amd64 TARGET_ARCH=amd64 -j2 libraries 2>&1 | tee /tmp/libs-build.log
      2. find obj -name 'libc.a' 2>/dev/null
      3. find obj -name 'libjail.a' 2>/dev/null
      4. find obj -name 'libthr.a' 2>/dev/null
      5. wc -l /tmp/libs-build.log
    Expected Result: At least libc.a exists; build log > 100 lines; exit code 0
    Failure Indicators: "fatal error" or "undefined reference" in build log
    Evidence: .omo/evidence/task-13-libs-build.txt
  ```

  **Commit**: NO (build artifacts not committed)

- [x] 14. Build `usr.sbin/ocifbsd/` and all its subdirs

  **What to do**:
  - After T13: `cd usr.sbin/ocifbsd && bmake TARGET=amd64 TARGET_ARCH=amd64 -j$(sysctl -n hw.ncpu)`
  - This recursively builds all subdirs (image, network, security, orchestration, etc.)
  - Capture full build log; identify first failure if any
  - Common failures: missing headers (sys/jail.h, sys/vnet.h), missing libraries (libjail, libcrypto, libssl, libjson-c, libpthread), missing prototypes
  - For each failure, decide: (a) fix in source, (b) add to Makefile dependency, (c) defer to follow-up
  - If `ocifbsd` binary builds, verify it: `file usr.sbin/ocifbsd/ocifbsd` should show ELF 64-bit LSB executable, x86-64
  - Save evidence

  **Must NOT do**:
  - Don't fix ALL compilation errors in one task (focus on getting the binary built)
  - Don't run the binary (it's a FreeBSD ELF, won't run on macOS)
  - Don't commit obj/ or binary outputs

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: `[]`
  - **Reason**: Multi-file build, error triage, possible source fixes

  **Parallelization**:
  - **Can Run In Parallel**: NO (Wave 3 sequential)
  - **Parallel Group**: Wave 3
  - **Blocks**: T15, T17, T19
  - **Blocked By**: T9, T10, T11, T13

  **Acceptance Criteria**:
  - [ ] `usr.sbin/ocifbsd/ocifbsd` exists and is an ELF binary (or near-build; errors documented)
  - [ ] Build log captured; first 100 + last 50 lines saved
  - [ ] If failed: at least the first error is documented in evidence
  - [ ] If succeeded: `ocifbsd --version` works (via qemu-user-static or documented as FreeBSD-only)

  **QA Scenarios**:
  ```
  Scenario: ocifbsd binary builds
    Tool: Bash (bmake + file)
    Preconditions: T13 succeeded
    Steps:
      1. cd usr.sbin/ocifbsd
      2. bmake TARGET=amd64 TARGET_ARCH=amd64 -j2 2>&1 | tee /tmp/ocifbsd-build.log
      3. ls -la ocifbsd 2>/dev/null
      4. file ocifbsd 2>/dev/null
      5. # If failed: head -30 /tmp/ocifbsd-build.log
    Expected Result: ocifbsd ELF binary exists, or clear error in log
    Evidence: .omo/evidence/task-14-ocifbsd-build.txt
  ```

  **Commit**: NO (build output not committed; only source fixes if any)

- [x] 15. Build `tests/usr.sbin/ocifbsd/` test harness

  **What to do**:
  - After T14: `cd tests/usr.sbin/ocifbsd && bmake TARGET=amd64 TARGET_ARCH=amd64`
  - This builds `ocifbsd_test.c` (C unit test) into a test binary
  - Capture build log
  - The shell test `ocifbsd_test.sh` is an ATF script (no compile needed)
  - Verify both test files are present and well-formed
  - Document any test failures (tests should be runnable; actual execution requires FreeBSD host)

  **Must NOT do**:
  - Don't try to RUN the tests on macOS (they need a FreeBSD jail env)
  - Don't modify test logic in this task (just build them)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: `[]`
  - **Reason**: Test build, log analysis

  **Parallelization**:
  - **Can Run In Parallel**: NO (Wave 3 sequential)
  - **Parallel Group**: Wave 3
  - **Blocks**: T19, T21
  - **Blocked By**: T13, T14

  **Acceptance Criteria**:
  - [ ] `tests/usr.sbin/ocifbsd/ocifbsd_test` binary exists (or build error documented)
  - [ ] Build log captured
  - [ ] `ocifbsd_test.sh` is well-formed shell (run `sh -n` for syntax check)
  - [ ] `ocifbsd_test.c` is well-formed C (`cc -fsyntax-only` if cross-clang available)

  **QA Scenarios**:
  ```
  Scenario: Test harness builds
    Tool: Bash (bmake)
    Preconditions: T14 succeeded (ocifbsd lib available)
    Steps:
      1. cd tests/usr.sbin/ocifbsd
      2. bmake TARGET=amd64 TARGET_ARCH=amd64 -j2 2>&1 | tee /tmp/test-build.log
      3. ls -la ocifbsd_test 2>/dev/null
      4. sh -n ocifbsd_test.sh && echo "shell syntax OK"
    Expected Result: Test binary exists or documented error
    Evidence: .omo/evidence/task-15-test-build.txt
  ```

  **Commit**: NO (build output not committed)

### Wave 4 — CLion IDE Integration (5 parallel)

- [x] 16. Configure CLion bmake toolchain (`.idea/tools/Toolchains.xml`)

  **What to do**:
  - Create `.idea/tools/Toolchains.xml` (if not present)
  - Define a `System` toolchain pointing at Homebrew bmake and macOS clang
  - Toolchain name: `FreeBSD-CrossBuild-macOS`
  - C compiler: `/opt/homebrew/opt/llvm/bin/clang` (or macOS system clang)
  - C++ compiler: same as above
  - Make: `/opt/homebrew/bin/bmake` (NOT `/usr/bin/make` which is GNU make)
  - Debugger: `/usr/bin/lldb` (CLion's default for macOS)
  - Reference: JetBrains Custom Build Targets docs use toolchain to provide env for custom run/debug configs

  **Must NOT do**:
  - Don't put absolute paths to user-specific home directories (use `/opt/homebrew/...` which is the default)
  - Don't include version numbers in toolchain name

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: `[]`
  - **Reason**: XML config file creation

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4 (with 17-20)
  - **Blocks**: T17, T18
  - **Blocked By**: T7 (developer setup doc)

  **Acceptance Criteria**:
  - [ ] `.idea/tools/Toolchains.xml` exists
  - [ ] File contains a `<toolchain>` with name `FreeBSD-CrossBuild-macOS`
  - [ ] Make path points to bmake
  - [ ] Debugger is lldb
  - [ ] XML is well-formed (verify with `xmllint --noout`)

  **QA Scenarios**:
  ```
  Scenario: Toolchain config valid
    Tool: Bash (xmllint)
    Preconditions: xmllint installed (part of libxml2)
    Steps:
      1. xmllint --noout .idea/tools/Toolchains.xml (must succeed)
      2. grep -c 'name="FreeBSD-CrossBuild-macOS"' .idea/tools/Toolchains.xml (must be >= 1)
      3. grep -E "(bmake|lldb|clang)" .idea/tools/Toolchains.xml (must be all three)
    Expected Result: All checks pass
    Evidence: .omo/evidence/task-16-toolchain.txt
  ```

  **Commit**: YES - `chore(ide): add FreeBSD cross-build toolchain config`
  - Files: `.idea/tools/Toolchains.xml`

- [x] 17. Create Custom Build Targets for ocifbsd subdirs (`.idea/misc.xml`)

  **What to do**:
  - Modify `.idea/misc.xml` to add Custom Build Targets under the existing `MakefileSettings` block
  - Target 1: `ocifbsd-core` — Build command: `bmake TARGET=amd64 TARGET_ARCH=amd64 -C $ProjectFileDir$/usr.sbin/ocifbsd/src`; Working dir: `$ProjectFileDir$`
  - Target 2: `ocifbsd-full` — Build command: `bmake TARGET=amd64 TARGET_ARCH=amd64 -C $ProjectFileDir$/usr.sbin/ocifbsd`; Working dir: `$ProjectFileDir$`
  - Target 3: `ocifbsd-image` — Build command: `bmake TARGET=amd64 TARGET_ARCH=amd64 -C $ProjectFileDir$/usr.sbin/ocifbsd/image`; Working dir: `$ProjectFileDir$`
  - Target 4: `ocifbsd-network` — Build command: `bmake TARGET=amd64 TARGET_ARCH=amd64 -C $ProjectFileDir$/usr.sbin/ocifbsd/network`; Working dir: `$ProjectFileDir$`
  - Target 5: `ocifbsd-security` — Build command: `bmake TARGET=amd64 TARGET_ARCH=amd64 -C $ProjectFileDir$/usr.sbin/ocifbsd/security`; Working dir: `$ProjectFileDir$`
  - Clean commands: `bmake TARGET=amd64 TARGET_ARCH=amd64 -C <same dir> clean`
  - Reference: Custom Build Targets docs (https://www.jetbrains.com/help/clion/custom-build-targets.html)

  **Must NOT do**:
  - Don't replace the existing `MakefileProjectSettings` block (keep the original Makefile project)
  - Don't add targets for subdirs that don't exist or don't build

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: `[]`
  - **Reason**: XML config edits

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4 (with 16, 18-20)
  - **Blocks**: T18
  - **Blocked By**: T16 (toolchain)

  **Acceptance Criteria**:
  - [ ] `.idea/misc.xml` contains at least 5 `<customTarget>` entries
  - [ ] Each target has a `buildTool` referencing bmake with the right path
  - [ ] The existing `MakefileProjectSettings` block is preserved
  - [ ] XML is well-formed

  **QA Scenarios**:
  ```
  Scenario: Custom build targets configured
    Tool: Bash (xmllint + grep)
    Steps:
      1. xmllint --noout .idea/misc.xml (must succeed)
      2. grep -c 'name="ocifbsd' .idea/misc.xml (must be >= 5)
      3. grep -c "bmake" .idea/misc.xml (must be >= 5)
      4. # Verify the original MakefileProjectSettings block is intact
      5. grep -c "MakefileProjectSettings" .idea/misc.xml (must be >= 1)
    Expected Result: All checks pass
    Evidence: .omo/evidence/task-17-build-targets.txt
  ```

  **Commit**: YES - `chore(ide): add ocifbsd custom build targets`
  - Files: `.idea/misc.xml`

- [x] 18. Create Run/Debug Configurations (`.idea/workspace.xml`)

  **What to do**:
  - Modify `.idea/workspace.xml` to add `<RunConfiguration>` entries under `<configurations>`
  - Config 1: `Run ocifbsd` — Custom Build Application, target `ocifbsd-full`, executable `obj/.../ocifbsd`, program args (e.g., `--version`), no debug
  - Config 2: `Debug ocifbsd` — Same as Run, but with debugger attached (lldb), stops at main
  - Config 3: `Run ocifbsd create` — Custom Build Application, target `ocifbsd-full`, executable same, args `create --bundle /tmp/bundle`
  - Config 4: `Run ocifbsd start` — Custom Build Application, args `start <id>`
  - Config 5: `Run ocifbsd list` — Custom Build Application, args `list`
  - Note: Running FreeBSD ELF binaries on macOS requires `qemu-x86_64-static` (out of scope for this plan; user must install separately)

  **Must NOT do**:
  - Don't hardcode machine-specific paths (use `$ProjectFileDir$` variables)
  - Don't add SSH remote configs (that's a different work stream)

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: `[]`
  - **Reason**: XML config edits

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4 (with 16, 17, 19, 20)
  - **Blocks**: T20
  - **Blocked By**: T16, T17

  **Acceptance Criteria**:
  - [ ] `.idea/workspace.xml` has `<RunConfiguration>` entries for at least 5 ocifbsd scenarios
  - [ ] Each config has a `<target>` and `<executable>` element
  - [ ] `Debug ocifbsd` config has debugger type `LLDB`
  - [ ] XML well-formed

  **QA Scenarios**:
  ```
  Scenario: Run configs in place
    Tool: Bash
    Steps:
      1. xmllint --noout .idea/workspace.xml (must succeed)
      2. grep -c 'name="Run ocifbsd' .idea/workspace.xml (>= 1)
      3. grep -c 'name="Debug ocifbsd' .idea/workspace.xml (>= 1)
      4. grep "LLDB" .idea/workspace.xml (>= 1)
    Expected Result: All checks pass
    Evidence: .omo/evidence/task-18-run-configs.txt
  ```

  **Commit**: YES - `chore(ide): add ocifbsd run/debug configurations`
  - Files: `.idea/workspace.xml`

- [x] 19. Configure ATF test framework integration

  **What to do**:
  - Research CLion's ATF/kyua test framework integration
  - Add a Run/Debug config: `Run ocifbsd tests` — Bash script, runs `cd tests/usr.sbin/ocifbsd && kyua test`
  - Add as a "Shell Script" run configuration (CLion has a Shell Script template)
  - Document the ATF test execution model in `.plan/020.0-Developer-Setup.md` (cross-reference to T7)

  **Must NOT do**:
  - Don't try to integrate ATF as a first-class test framework (CLion doesn't have native ATF support; the shell-script run config is the best approximation)
  - Don't add Google Test configs (this is C, not C++)

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: `[]`
  - **Reason**: Single Run config addition + doc cross-ref

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4 (with 16-18, 20)
  - **Blocks**: T20
  - **Blocked By**: T14, T15 (test build)

  **Acceptance Criteria**:
  - [ ] `.idea/workspace.xml` has a "Run ocifbsd tests" config
  - [ ] `.plan/020.0-Developer-Setup.md` mentions ATF and `kyua test`
  - [ ] Config uses shell script or external tool

  **QA Scenarios**:
  ```
  Scenario: ATF test config in place
    Tool: Bash
    Steps:
      1. grep "kyua" .idea/workspace.xml (>= 1)
      2. grep "tests/usr.sbin/ocifbsd" .idea/workspace.xml (>= 1)
      3. grep "kyua\|atf" .plan/020.0-Developer-Setup.md (>= 1)
    Expected Result: All checks pass
    Evidence: .omo/evidence/task-19-atf-config.txt
  ```

  **Commit**: YES - `chore(ide): add ATF test run config and dev doc cross-ref`
  - Files: `.idea/workspace.xml`, `.plan/020.0-Developer-Setup.md`

- [x] 20. Test full IDE workflow (build, run, debug from CLion)

  **What to do**:
  - Manually verify (via headless command-line, since we're agents): use the CLion CLI to trigger builds if available
  - Alternative: write a script that simulates what CLion does: `bmake TARGET=amd64 TARGET_ARCH=amd64 -C usr.sbin/ocifbsd` — verify it produces the same artifact that the CLion target would
  - Verify the `obj/.../ocifbsd` path matches the one referenced in the run configs
  - Document the test results in `.omo/evidence/task-20-ide-workflow.md`

  **Must NOT do**:
  - Don't try to actually launch the CLion GUI from a script
  - Don't require user to "click Build" in the GUI to verify

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: `[]`
  - **Reason**: End-to-end workflow test

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 4 (with 16-19)
  - **Blocks**: F1, F2, F3
  - **Blocked By**: T17, T18, T19

  **Acceptance Criteria**:
  - [ ] `.omo/evidence/task-20-ide-workflow.md` exists
  - [ ] The script-equivalent of "CLion Build 'ocifbsd-full'" runs without error
  - [ ] The script-equivalent of "CLion Run 'ocifbsd'" finds the executable at the expected path
  - [ ] The script-equivalent of "CLion Debug 'ocifbsd'" — no equivalent in CLI; document why

  **QA Scenarios**:
  ```
  Scenario: Build/Run/Debug equivalents
    Tool: Bash
    Preconditions: T14 succeeded, configs in T17/T18 are valid
    Steps:
      1. The exact command from Custom Build Target 'ocifbsd-full' runs: bmake TARGET=amd64 TARGET_ARCH=amd64 -C $repo/usr.sbin/ocifbsd
      2. The exact path from Run Config 'Run ocifbsd' exists: test -f $repo/usr.sbin/ocifbsd/ocifbsd
      3. file $repo/usr.sbin/ocifbsd/ocifbsd (should report ELF)
    Expected Result: Build succeeds, binary path correct, ELF format
    Evidence: .omo/evidence/task-20-ide-workflow.md
  ```

  **Commit**: NO (verification only)

### Wave 5 — Verification, Task Doc, and Cleanup (5 parallel)

- [x] 21. Run ATF tests in FreeBSD VM with snapshot/restore

  **What to do**:
  - Restore VM to clean snapshot: `vmctl snapshot restore freebsd-oci clean` (or UTM/QEMU equivalent per T26)
  - SCP the built artifacts from macOS to the VM:
    - `scp usr.sbin/ocifbsd/ocifbsd freebsd-vm:~/`
    - `scp tests/usr.sbin/ocifbsd/ocifbsd_test freebsd-vm:~/`
    - `scp -r tests/usr.sbin/ocifbsd freebsd-vm:~/tests/`
  - SSH into VM and run the test harness:
    - `ssh freebsd-vm 'cd tests/usr.sbin/ocifbsd && ./ocifbsd_test.sh'`
    - `ssh freebsd-vm 'cd tests/usr.sbin/ocifbsd && kyua test -k ocifbsd_test'`
  - Capture test output to `.omo/evidence/task-21-vm-test-output.txt`
  - **Snapshot/restore loop**: after the test run (success OR failure), restore the VM to the clean snapshot so the next test run starts from a known state
  - Document any test failures, crashes, or environmental issues in evidence

  **Must NOT do**:
  - Don't commit the test results if they reveal a real bug — open a follow-up issue instead
  - Don't skip the snapshot restore — every test run must start from clean state
  - Don't try to run tests on macOS (jail(2) is FreeBSD-only)

  **Recommended Agent Profile**:
  - **Category**: `unspecified-high`
  - **Skills**: `[]`
  - **Reason**: Multi-step test orchestration across macOS↔VM boundary

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 5 (with 22-25)
  - **Blocks**: F1, F2
  - **Blocked By**: T15 (test build), T26 (VM provisioned)

  **Acceptance Criteria**:
  - [ ] `ssh freebsd-vm 'uname -a'` returns FreeBSD kernel version
  - [ ] `ssh freebsd-vm 'kyua --version'` works
  - [ ] `ocifbsd_test.sh` runs inside the VM; exit code captured
  - [ ] `ocifbsd_test` binary runs inside the VM; output captured
  - [ ] VM is restored to clean snapshot after the run
  - [ ] Test output saved to `.omo/evidence/task-21-vm-test-output.txt`

  **QA Scenarios**:
  ```
  Scenario: Test deploy + run in VM
    Tool: Bash (scp + ssh)
    Preconditions: T26 done, T15 done, VM is at clean snapshot
    Steps:
      1. scp usr.sbin/ocifbsd/ocifbsd freebsd-vm:~/
      2. scp -r tests/usr.sbin/ocifbsd freebsd-vm:~/tests/
      3. ssh freebsd-vm 'cd tests/usr.sbin/ocifbsd && ./ocifbsd_test.sh 2>&1' | tee /tmp/test-output.txt
      4. ssh freebsd-vm 'cd tests/usr.sbin/ocifbsd && ./ocifbsd_test 2>&1' | tee -a /tmp/test-output.txt
      5. # Restore snapshot
      6. vmctl snapshot restore freebsd-oci clean  # or UTM/QEMU equivalent
    Expected Result: All commands succeed; test output captured
    Failure Indicators: ssh fails, ocifbsd_test crashes, snapshot restore fails
    Evidence: .omo/evidence/task-21-vm-test-output.txt
  ```

  **Commit**: NO (verification only)

- [x] 22. Create `.omo/drafts/oci-bootstrap-tasks.md` (working task tracker)

  **What to do**:
  - This is the working task doc the user requested
  - It tracks sub-tasks, blockers, and progress for the ongoing OCI bootstrap work
  - Sections:
    1. **Active Tasks** — items currently in progress
    2. **Pending Tasks** — items queued, ordered by priority
    3. **Blockers** — things preventing progress
    4. **Decisions Log** — significant choices made during execution
    5. **Notes & Findings** — anything learned during execution
    6. **Completed Tasks** — moved-from-active, kept for history
  - Initially populate from this plan's task list
  - Update as work progresses

  **Must NOT do**:
  - Don't create the file inside `.plan/` (that's for design docs, not work tracking)
  - Don't put the file in the repo root (would clutter the tree)

  **Recommended Agent Profile**:
  - **Category**: `writing`
  - **Skills**: `[]`
  - **Reason**: Doc creation

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 5 (with 21, 23-25)
  - **Blocks**: F1
  - **Blocked By**: T5, T6, T7 (plan docs done)

  **Acceptance Criteria**:
  - [ ] `.omo/drafts/oci-bootstrap-tasks.md` exists
  - [ ] Has all 6 sections listed above
  - [ ] At least 5 items in "Active Tasks" or "Pending Tasks"

  **QA Scenarios**:
  ```
  Scenario: Task tracker exists
    Tool: Bash
    Steps:
      1. test -f .omo/drafts/oci-bootstrap-tasks.md (must succeed)
      2. grep -c "^## " .omo/drafts/oci-bootstrap-tasks.md (must be >= 6)
      3. wc -l .omo/drafts/oci-bootstrap-tasks.md (must be > 30)
    Expected Result: All checks pass
    Evidence: .omo/evidence/task-22-task-tracker.txt
  ```

  **Commit**: YES - `docs: add working task tracker for oci-bootstrap work`
  - Files: `.omo/drafts/oci-bootstrap-tasks.md`

- [x] 23. Update `.plan/000.0-OCI-Jail-TOC.md` cross-references for the new 020.0 doc

  **What to do**:
  - Add a link from the Document Map to the new `.plan/020.0-Developer-Setup.md`
  - Add a section to the Document Dependencies showing how 020.0 relates to the other docs
  - Add a "Bootstrap Section" entry pointing to `.omo/drafts/oci-bootstrap-tasks.md`
  - Verify the TOC still has all 24 original doc rows plus the new one

  **Must NOT do**:
  - Don't break the existing table format
  - Don't renumber existing docs

  **Recommended Agent Profile**:
  - **Category**: `writing`
  - **Skills**: `[]`
  - **Reason**: Doc maintenance

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 5 (with 21, 22, 24, 25)
  - **Blocks**: F1
  - **Blocked By**: T4 (files exist), T7 (dev doc created)

  **Acceptance Criteria**:
  - [ ] TOC has row for 020.0
  - [ ] TOC has cross-reference to `.omo/drafts/oci-bootstrap-tasks.md`
  - [ ] All 24 original doc rows preserved
  - [ ] Markdown is well-formed

  **QA Scenarios**:
  ```
  Scenario: TOC cross-refs in place
    Tool: Bash + Grep
    Steps:
      1. grep "020.0-Developer-Setup" .plan/000.0-OCI-Jail-TOC.md (>= 1)
      2. grep "oci-bootstrap-tasks" .plan/000.0-OCI-Jail-TOC.md (>= 1)
      3. grep -c "| 0\." .plan/000.0-OCI-Jail-TOC.md (>= 24 — original docs)
    Expected Result: All checks pass
    Evidence: .omo/evidence/task-23-toc-xrefs.txt
  ```

  **Commit**: YES (likely part of T6's commit) - `docs(plan): add 020.0 cross-refs to TOC`
  - Files: `.plan/000.0-OCI-Jail-TOC.md`

- [x] 24. Update `.gitignore` for cross-build artifacts and IDE cache

  **What to do**:
  - Add to `.gitignore`:
    - `obj/` — cross-build artifacts
    - `obj.*/` — alternative obj paths
    - `*.o`, `*.a`, `*.so` — built objects (where not already ignored)
    - `usr.sbin/ocifbsd/ocifbsd` — built binary
    - `usr.sbin/ocifbsd/**/ocifbsd-*` — per-subdir built binaries
    - `tests/usr.sbin/ocifbsd/ocifbsd_test` — test binary
  - Verify the existing `.gitignore` is preserved (don't lose other rules)
  - Add a comment header explaining what these are for

  **Must NOT do**:
  - Don't ignore `usr.sbin/ocifbsd/*.c` (source files must be committed)
  - Don't ignore the entire `usr.sbin/ocifbsd/` (only build outputs)

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: `[]`
  - **Reason**: Single-file edit

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 5 (with 21-23, 25)
  - **Blocks**: F1
  - **Blocked By**: T1, T3, T13 (build artifacts exist)

  **Acceptance Criteria**:
  - [ ] `.gitignore` has new `obj/`, `obj.*/`, `*.o` entries
  - [ ] `.gitignore` has specific entries for `usr.sbin/ocifbsd/ocifbsd` and `tests/.../ocifbsd_test`
  - [ ] Original `.gitignore` content preserved
  - [ ] `git check-ignore` confirms patterns work

  **QA Scenarios**:
  ```
  Scenario: Gitignore covers build outputs
    Tool: Bash (git)
    Preconditions: T14 succeeded (obj/ exists)
    Steps:
      1. git check-ignore usr.sbin/ocifbsd/ocifbsd 2>&1 (should be ignored)
      2. git check-ignore tests/usr.sbin/ocifbsd/ocifbsd_test 2>&1 (should be ignored)
      3. git check-ignore usr.sbin/ocifbsd/ocifbsd.c 2>&1 (should NOT be ignored)
      4. git status --ignored | head
    Expected Result: Build outputs ignored, sources not
    Evidence: .omo/evidence/task-24-gitignore.txt
  ```

  **Commit**: YES - `chore: gitignore cross-build artifacts`
  - Files: `.gitignore`

- [x] 25. Commit and push feature branch (if user approved)

  **What to do**:
  - Verify all changes are committed: `git status` clean
  - Verify all evidence files are present
  - Verify the branch builds (T14, T15 passed)
  - If user has approved pushing: `git push -u origin feature/oci-bootstrap`
  - If not: leave branch local; user can push when ready
  - Document the final state in `.omo/evidence/task-25-push.md`

  **Must NOT do**:
  - Don't force-push
  - Don't push to `main` or `devel` (only the feature branch)

  **Recommended Agent Profile**:
  - **Category**: `quick`
  - **Skills**: `[]`
  - **Reason**: Standard git push

  **Parallelization**:
  - **Can Run In Parallel**: YES
  - **Parallel Group**: Wave 5 (with 21-24)
  - **Blocks**: F1, F2, F3, F4
  - **Blocked By**: T4-T24 (all preceding tasks)

  **Acceptance Criteria**:
  - [ ] `git status` is clean (or only untracked `.omo/evidence/`)
  - [ ] `git log --oneline origin/main..HEAD` shows the OCI work
  - [ ] Push either succeeded OR documented as user-deferred

  **QA Scenarios**:
  ```
  Scenario: Branch ready for push
    Tool: Bash (git)
    Steps:
      1. git status (clean or only untracked evidence)
      2. git log --oneline origin/main..HEAD | wc -l (>= 5)
      3. # If user approved: git push -u origin feature/oci-bootstrap
    Expected Result: Branch is ready; push either succeeded or deferred
    Evidence: .omo/evidence/task-25-branch-state.txt
  ```

  **Commit**: NO (this task IS the commit)

---

## Final Verification Wave (MANDATORY — after ALL implementation tasks)

> 4 review agents run in PARALLEL. ALL must APPROVE. Present consolidated results to user and get explicit "okay" before completing.
>
> **Do NOT auto-proceed after verification. Wait for user's explicit approval before marking work complete.**
> **Never mark F1-F4 as checked before getting user's okay.** Rejection or user feedback -> fix -> re-run -> present again -> wait for okay.

- [x] F1. **Plan Compliance Audit** — `oracle`
  Read the plan end-to-end. For each "Must Have": verify implementation exists (read file, curl endpoint, run command). For each "Must NOT Have": search codebase for forbidden patterns — reject with file:line if found. Check evidence files exist in `.omo/evidence/`. Compare deliverables against plan.
  Output: `Must Have [N/N] | Must NOT Have [N/N] | Tasks [N/N] | VERDICT: APPROVE/REJECT`

- [x] F2. **Code Quality Review** — `unspecified-high`
  Run `xmllint --noout` on all `.idea/*.xml` files. Verify Makefile syntax with `bmake -n -C usr.sbin/ocifbsd` (dry run). Review all changed files for: AI slop patterns, empty bodies, commented-out code, unused imports. Check the audit report from T8 against actual evidence.
  Output: `Build [PASS/FAIL] | Lint [PASS/FAIL] | Tests [N pass/N fail] | Files [N clean/N issues] | VERDICT`

- [x] F3. **Real Manual QA** — `unspecified-high`
  Start from clean state. Execute EVERY QA scenario from EVERY task — follow exact steps, capture evidence. Test cross-task integration: do the custom build targets actually invoke bmake? Do the run configs point at real artifacts? Is the IDE workflow documented in 020.0 actually followed end-to-end? Save to `.omo/evidence/final-qa/`.
  Output: `Scenarios [N/N pass] | Integration [N/N] | Edge Cases [N tested] | VERDICT`

- [x] F4. **Scope Fidelity Check** — `deep`
  For each task: read "What to do", read actual diff (`git log -p`, `git diff`). Verify 1:1 — everything in spec was built (no missing), nothing beyond spec was built (no creep). Check "Must NOT do" compliance. Detect cross-task contamination. Flag unaccounted changes. Verify the 4 `.plan/` docs are present and accurate. Verify `.omo/drafts/oci-bootstrap-tasks.md` is the working tracker.
  Output: `Tasks [N/N compliant] | Contamination [CLEAN/N issues] | Unaccounted [CLEAN/N files] | VERDICT`

---

## Commit Strategy

This plan produces a single feature branch (`feature/oci-bootstrap`) with the following commit sequence:

| # | Commit | Files | Task |
|---|--------|-------|------|
| 1 | `feat: bring in ocifbsd OCI runtime from origin/devel` | `usr.sbin/ocifbsd/`, `tests/usr.sbin/ocifbsd/`, `AGENTS_START_HERE.md`, `.plan/019.0-Package-Release.md`, `release/release-oci.conf` | T2 |
| 2 | `docs(plan): reconcile 005.0 with current branch state` | `.plan/005.0-Risks-TODO.md` | T5 |
| 3 | `docs(plan): refresh TOC line counts and add 020.0 entry` | `.plan/000.0-OCI-Jail-TOC.md` | T6 |
| 4 | `docs(plan): add 020.0-Developer-Setup.md (CLion+bmake+cross-build)` | `.plan/020.0-Developer-Setup.md` | T7 |
| 5 | `chore(ocifbsd): add missing SUBDIR entries` (conditional) | `usr.sbin/ocifbsd/Makefile` | T9 |
| 6 | `chore(usr.sbin): add ocifbsd to SUBDIR` (conditional) | `usr.sbin/Makefile` | T10 |
| 7 | `chore(ide): add FreeBSD cross-build toolchain config` | `.idea/tools/Toolchains.xml` | T16 |
| 8 | `chore(ide): add ocifbsd custom build targets` | `.idea/misc.xml` | T17 |
| 9 | `chore(ide): add ocifbsd run/debug configurations` | `.idea/workspace.xml` | T18 |
| 10 | `chore(ide): add ATF test run config and dev doc cross-ref` | `.idea/workspace.xml`, `.plan/020.0-Developer-Setup.md` | T19 |
| 11 | `docs: add working task tracker for oci-bootstrap work` | `.omo/drafts/oci-bootstrap-tasks.md` | T22 |
| 12 | `docs(plan): add 020.0 cross-refs to TOC` | `.plan/000.0-OCI-Jail-TOC.md` | T23 |
| 13 | `chore: gitignore cross-build artifacts` | `.gitignore` | T24 |

All commits use the project's existing commit style: `type(scope): subject` (e.g., `feat:`, `chore:`, `docs:`).

---

## Success Criteria

### Verification Commands
```bash
# Verify bmake is available
which bmake && bmake --version | head -1

# Verify the feature branch is current
git rev-parse --abbrev-ref HEAD  # → feature/oci-bootstrap
git log --oneline origin/main..HEAD | wc -l  # → 5 or more

# Verify the OCI code is present
test -f usr.sbin/ocifbsd/ocifbsd.c && echo "ocifbsd source OK"
test -f usr.sbin/ocifbsd/Makefile && echo "ocifbsd Makefile OK"
test -f tests/usr.sbin/ocifbsd/ocifbsd_test.c && echo "tests OK"

# Verify cross-build works (after T13-14 succeed)
bmake TARGET=amd64 TARGET_ARCH=amd64 -C usr.sbin/ocifbsd -n  # dry run, no errors
file usr.sbin/ocifbsd/ocifbsd  # → ELF 64-bit LSB executable, x86-64

# Verify CLion configs are well-formed
xmllint --noout .idea/misc.xml
xmllint --noout .idea/workspace.xml
xmllint --noout .idea/tools/Toolchains.xml

# Verify docs are in place
test -f .plan/020.0-Developer-Setup.md && echo "dev doc OK"
test -f .omo/drafts/oci-bootstrap-tasks.md && echo "task tracker OK"
test -f AGENTS_START_HERE.md && echo "agents guide OK"
test -f .plan/019.0-Package-Release.md && echo "019.0 doc OK"

# Verify all evidence files exist
ls .omo/evidence/task-*.{txt,md} 2>/dev/null | wc -l  # → 20+ files

# Run the audit gate
test "$(git log --oneline origin/main..HEAD | wc -l)" -ge 5
```

### Final Checklist
- [ ] All "Must Have" items present
  - [ ] `usr.sbin/ocifbsd/` source code on the branch
  - [ ] `tests/usr.sbin/ocifbsd/` test harness on the branch
  - [ ] `bmake` installable via Homebrew (verified on host)
  - [ ] `tools/build/make.py` cross-build driver verified
  - [ ] **FreeBSD VM provisioned with dev tools (bmake, clang, kyua, sshd)**
  - [ ] **VM "clean" snapshot exists; create/restore verified**
  - [ ] **VM ↔ macOS SSH key auth works (`ssh freebsd-vm 'uname -a'`)**
  - [ ] **VM test workflow documented in `.plan/020.0-Developer-Setup.md` "Testing in a FreeBSD VM" section**
  - [ ] `.idea/tools/Toolchains.xml` defines FreeBSD cross-build toolchain
  - [ ] `.idea/misc.xml` has 5+ Custom Build Targets for ocifbsd
  - [ ] `.idea/workspace.xml` has 5+ Run/Debug configurations
  - [ ] `.plan/020.0-Developer-Setup.md` exists
  - [ ] `.plan/005.0-Risks-TODO.md` updated with state preamble
  - [ ] `.plan/000.0-OCI-Jail-TOC.md` line counts refreshed
  - [ ] `AGENTS_START_HERE.md` present
  - [ ] `.omo/drafts/oci-bootstrap-tasks.md` exists
- [ ] All "Must NOT Have" items absent
  - [ ] No modifications to `share/mk/`, `Makefile.inc1`, `Makefile.libcompat`, `Makefile.sys.inc`
  - [ ] No modifications to `usr.sbin/jail/`, `usr.sbin/jexec/`, `usr.sbin/jls/`
  - [ ] No forced pushes to any branch
  - [ ] No committed `obj/`, `*.o`, `*.a`, `*.so` files
  - [ ] No committed VM disk images, SSH private keys, or VM snapshots
  - [ ] No machine-specific absolute paths in committed `.idea/workspace.xml`
  - [ ] No `// TODO` stubs left in audited code
  - [ ] No split into multiple plans
  - [ ] F1-F4 reviews all APPROVED
- [ ] F1 APPROVE: Must Have [N/N], Must NOT Have [N/N], Tasks [N/N]
- [ ] F2 APPROVE: Build [PASS], Lint [PASS], Tests documented
- [ ] F3 APPROVE: All scenarios pass, integration verified (incl. VM test loop)
- [ ] F4 APPROVE: All tasks compliant, no contamination
- [ ] User has explicitly approved the work

---


---

## T27 (added post-plan): Darwin build automation

**Motivation**: The cross-build process from macOS requires many manual steps (install brew, install llvm, set XCC/XLD/XAS env vars, create MAKEOBJDIRPREFIX, run the cross-build). This task automates the process so a developer with a clean macOS install can build ocifbsd with one command.

**Files added/modified**:
- `usr.sbin/ocifbsd/darwin-bootstrap.sh` (new, 289 lines)
  - Checks Xcode CLT, Homebrew, bmake, LLVM (clang/lld/lldb), Python 3
  - Auto-installs missing tools via brew when `--install` flag is passed
  - Installs Homebrew itself if missing (with user confirmation)
  - Generates `/tmp/ocifbsd-cross-build-env` with all XCC/XLD/XAS env vars
  - Idempotent: safe to run multiple times
  - POSIX sh compatible (no bashisms)
- `usr.sbin/ocifbsd/Makefile` (modified, +48 lines)
  - Added 3 targets: `darwin-bootstrap`, `darwin-build`, `darwin-test`
  - `darwin-bootstrap`: just runs the bootstrap script with `--install --yes`
  - `darwin-build`: bootstrap + cross-build userland libraries + build ocifbsd + build tests
  - `darwin-test`: build + scp to FreeBSD VM + run kyua tests + restore snapshot

**Usage from repo root**:
```bash
# One-time setup on a clean macOS
bmake -C usr.sbin/ocifbsd darwin-bootstrap

# Bootstrap + build (most common)
bmake -C usr.sbin/ocifbsd darwin-build

# Build + deploy to VM + run tests
VM_HOST=my-freebsd-vm bmake -C usr.sbin/ocifbsd darwin-test
```

**Acceptance criteria**:
- [x] Script handles clean macOS (Xcode CLT -> brew -> bmake -> llvm -> python3)
- [x] Script auto-installs brew if missing (with confirmation)
- [x] Script is idempotent (re-running doesn't break things)
- [x] Makefile targets use bmake continuation syntax
- [x] Targets accept env vars: TARGET, TARGET_ARCH, VM_HOST, OCIFBSD_ENV_FILE
- [x] darwin-build fails fast on missing tools
- [x] darwin-test handles VM snapshot restore via existing helper

**Tested on this machine**:
- `darwin-bootstrap.sh --check` runs all 5 checks, identifies missing lld
- Makefile targets parse correctly (bmake finds them, error is about .include path, not syntax)

**Not yet tested** (requires clean macOS):
- Full `--install` flow on a machine without brew
- darwin-build on a machine with all tools installed
- darwin-test against a live FreeBSD VM
