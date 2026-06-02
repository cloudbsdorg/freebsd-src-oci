# Draft: freebsd-src-oci — Audit, Plan, and CLion Tooling

## Repository Identity

- **Repo**: `cloudbsdorg/freebsd-src-oci` (a FreeBSD fork)
- **Purpose**: Native OCI container runtime (`ocifbsd`) using FreeBSD jails
- **Author**: Mark LaPointe <mark@cloudbsd.org>
- **Platform host (developer)**: macOS (Darwin)
- **Build tool on macOS**: bmake (FreeBSD make) — required by FreeBSD Makefiles
- **IDE**: CLion (already opened on this tree; `.idea/` present and untracked)

## Branch State (the key finding)

| Branch | Tip | What it has | What it's missing |
|---|---|---|---|
| `main` (local & origin) | `fd041384adc` (2026-06-02) | Clean FreeBSD main; full `.plan/` doc tree (24 docs); pre-existing `release/Makefile.oci`, `release/scripts/make-oci-image.sh`, `release/tools/oci-image-*.conf` (upstream FreeBSD OCI image build) | **All `usr.sbin/ocifbsd/` source code**; `AGENTS_START_HERE.md`; `release/release-oci.conf`; `.plan/019.0-Package-Release.md` |
| `origin/devel` | `c466494a009` (2026-04-30) | Everything on main PLUS 16 OCI phase commits (96 ocifbsd source files + 3 test files); `AGENTS_START_HERE.md` (claimed all phases complete); `.plan/019.0-Package-Release.md`; `release/release-oci.conf`; `.plan/005.0-Risks-TODO.md` updated to mark 352 tasks complete | 762 commits behind `freebsd:main` (i.e., not current with upstream) |

**`git rev-list --left-right --count origin/main...origin/devel`** → `762  21`
- 21 commits in `devel` that aren't in `main` (the OCI work)
- 762 commits in `main` that aren't in `devel` (newer upstream FreeBSD changes that `devel` hasn't pulled in)

**The orphaned work is in fact NOT orphaned** — it's all on `origin/devel`. The local `main` was likely rebased/merged from `freebsd:main` without the OCI feature branch.

## What's Actually on `origin/devel` (the OCI work to recover)

### Source code (`usr.sbin/ocifbsd/`, 96 files)
- Top-level: `Makefile`, `ocifbsd.c`, `ocifbsd.8`, `README.md`
- `include/ocifbsd.h`
- `src/` (Phase 1 core): `container.c`, `oci2jail.c`, `state.c`, `hooks.c`, `utils.c`
- `image/` (Phase 2): `zfs_store.{c,h}`, `pull.{c,h}`, `push.{c,h}`, `unpack.{c,h}`
- `network/` (Phase 3): `network.{c,h}`, `bridge.c`, `vnet.c`, `cni.c`
- `security/` (Phase 4): `rctl.{c,h}`, `mac.{c,h}`
- `orchestration/` (Phase 6): `orchestration.h`, `pod.c`, `stack.c`, `scheduler.c`, `health.c`, `rolling_update.c`, `orch_cli.c`, `orch_init.c`
- `convert/` (Phase 7): config conversion
- `namespace/` (Phase 8): namespace + resource mgmt
- `clustering/` (Phase 9): clustering
- `security-daemon/`, `pam/`, `tpm/`, `cert/`, `api/`, `logd/`, `metrics/`, `gc/`, `export/`

### Tests
- `tests/usr.sbin/ocifbsd/`: `Makefile`, `ocifbsd_test.c` (C unit tests), `ocifbsd_test.sh` (ATF shell test)

### Root files
- `AGENTS_START_HERE.md` (agent guide; claims all phases 0-18 complete)

### Release artifacts
- `release/release-oci.conf` (release build config focused on OCI images)
- `.plan/019.0-Package-Release.md` (release & packaging doc)

### Docs
- `.plan/005.0-Risks-TODO.md` was rewritten to mark 352 tasks complete in commit `cee839ce6ef`
- `.plan/000.0-OCI-Jail-TOC.md` updated for completion
- All 24 plan docs present and on main already

## Documentation State (on local main, where the user is)

- `.plan/000.0-OCI-Jail-TOC.md` — TOC (line counts are stale; refers to orphaned completion state)
- `.plan/000.1-Agent-Workflow.md` — workflow for multi-agent work (claims work
- `.plan/001.0-Overview.md` — exec summary, current/proposed architecture
- `.plan/001.1-Implementation-Phases.md` — phase plan
- `.plan/001.2-Alternative-Approaches.md` — alternatives considered
- `.plan/002.0-CLI-Spec.md` — CLI reference
- `.plan/003.0-Implementation.md` — file structure, data structures
- `.plan/004.0-Testing.md` — testing strategy
- `.plan/005.0-Risks-TODO.md` — task tracker; **on main still says all tasks NOT STARTED** (the orphan commit updated this but isn't on main)
- `.plan/006.0` through `.plan/018.0` — phase-specific deep dives
- `.plan/A.0-Glossary-and-Appendix.md` — glossary

**Inconsistency**: `.plan/000.0-OCI-Jail-TOC.md` and `.plan/000.1-Agent-Workflow.md` on main were modified by orphan commits (line counts, "completed" status). The user sees an inconsistent doc state — some files updated, others not.

## CLion + bmake on macOS (the IDE problem)

**Current CLion config (in `.idea/`):**
- `workspace.xml` — basic CLion 2026.1.2 (Classic) workspace, default Makefile sync (`RE_IMPORT`)
- `misc.xml` — `MakefileProjectSettings` points to `$PROJECT_DIR$` (top-level Makefile)
- `vcs.xml` — Git VCS mapping
- `codeStyles/` — code style config
- `editor.xml` — editor settings
- `.gitignore` — standard CLion gitignore

**Problems with current setup**:
1. `MakefileProjectSettings` is set to top-level `Makefile`, which is the FreeBSD world build — won't work on macOS (needs cross-compile or VM).
2. No `Makefile` runner configured to use `bmake` (FreeBSD make) — CLion's default Make runner uses GNU make.
3. No custom build targets for subprojects (`usr.sbin/ocifbsd/`, `usr.sbin/jail/`, `tests/usr.sbin/ocifbsd/`).
4. No Run/Debug configurations for `ocifbsd`, the test harness, or integration test VMs.
5. No toolchain config — CLion needs to know about FreeBSD's `clang`, includes, sysroot, etc.
6. FreeBSD build uses BSD-style conventions (`.PATH`, `SRCS`, `PROG`, `MAN`, etc.) — CLion's Makefile support is GNU-make-centric.

**What bmake needs to work on macOS**:
- `bmake` is in Homebrew (`bmake` formula) or pkgsrc
- FreeBSD source tree expects `bmake` to be in `$PATH`
- Some FreeBSD-specific make variables (e.g., `.MAKE` job server)

## User's Stated Goals (parsed)

1. **Set up CLion to compile/run/debug** this FreeBSD source on macOS using `bmake` and CLion's tooling
2. **Audit the OCI work** — does the existing implementation make sense?
3. **Document the situation** — update the `.plan/` docs to reflect actual state
4. **Create a working task doc** if one doesn't exist for getting the OCI system going

## Open Questions for User (must clarify before plan generation)

- [ ] **Q1: Use `origin/devel`'s ocifbsd code, or restart from scratch?**
  - `origin/devel` has 16 phase commits, 96 source files, 3 test files, all merged
  - Re-using is much faster; starting fresh would take weeks
- [ ] **Q2: How to merge `origin/devel` OCI work onto a current branch?**
  - Options: (a) branch from `origin/devel` directly, (b) cherry-pick/merge OCI commits onto `main`, (c) rebase `devel` onto current `main`
- [ ] **Q3: What's the build/test target?**
  - macOS host building FreeBSD userland? (cross-compile)
  - FreeBSD VM (bhyve/QEMU/VirtualBox/UTM) on macOS?
  - Just need editor/IDE indexing, not full builds?
- [ ] **Q4: Scope of CLion integration?**
  - Just syntax highlighting + jump-to-definition?
  - Full build/test/debug from CLion?
  - Specific targets (e.g., only `ocifbsd`) or whole tree?
- [ ] **Q5: How aggressive on doc updates?**
  - Fix line counts in TOC, restore "NOT STARTED" status to be honest about state, add a new "How to bootstrap" doc?
  - Or full rewrite of `.plan/` to reflect the 21-commit delta from `origin/devel`?

## Initial Analysis — Do the OCI changes make sense?

**From a quick read of commit messages and file shapes:**

**Sensible:**
- Naming: `ocifbsd` is a sensible name (FreeBSD's `ocifbsd` — FreeBSD-flavored OCI runtime)
- File layout: `usr.sbin/ocifbsd/<module>/` matches FreeBSD conventions
- Subdirs: `image/`, `network/`, `security/`, `orchestration/`, `pam/`, `tpm/`, `cert/`, `api/`, `logd/`, `gc/`, `export/`, `convert/`, `clustering/`, `namespace/`, `metrics/`, `security-daemon/` — matches a real container runtime feature set
- Test framework: ATF shell test (`ocifbsd_test.sh`) + C unit test (`ocifbsd_test.c`) is conventional
- Release config: `release/release-oci.conf` with `WITH_OCIIMAGES=1` is consistent with FreeBSD's existing OCI image build
- License header on `ocifbsd.c` is the standard FreeBSD Foundation header (suspicious: header says Klara, Inc. under FreeBSD Foundation sponsorship — could be AI-generated, but is structurally valid)

**Concerns (flag for verification, not blockers):**
- The 352-tasks-complete claim came from AI (Junie) — needs human verification of what actually compiles/runs
- No CI config to run tests (`.cirrus.yml` exists, need to check if ocifbsd tests are run)
- No FreeBSD VM CI for integration tests
- The "complete" claim is based on file presence, not on passing tests

**What I need to actually verify when planning:**
- Does the code compile on FreeBSD (when run in a VM)?
- Do the unit tests pass?
- Does the OCI spec compliance check pass?
- Does the FreeBSD main build still work with these additions?

## Next Steps (after user clarifies)

1. Decide branch strategy (devel? merge into main? new branch?)
2. Generate work plan at `.omo/plans/oci-bootstrap.md`
3. Generate task doc at `.omo/drafts/oci-bootstrap-tasks.md`
4. Update `.plan/005.0-Risks-TODO.md` to reflect actual state
5. Update `.plan/000.0-OCI-Jail-TOC.md` line counts
6. Create `.plan/020.0-Developer-Setup.md` for CLion + bmake + bootstrap
7. Generate commit-ready config files in `.idea/`

## Plan Path Conventions

- **Plan file**: `.omo/plans/oci-bootstrap.md`
- **Draft file**: `.omo/drafts/oci-bootstrap.md` (this file)
- **Doc updates**: edit existing `.plan/*.md` files (do not create new `.plan/` files unless necessary)
- **CLion config**: edit existing `.idea/*.xml` files (do not create new top-level config)
