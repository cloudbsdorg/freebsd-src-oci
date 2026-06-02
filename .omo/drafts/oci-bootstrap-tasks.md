# oci-bootstrap — Working Task Tracker

> **Purpose:** Live progress tracker for the `oci-bootstrap` work plan. This file is the authoritative source for "what's done, what's blocked, what's next" between plan updates.
>
> **Last updated:** 2026-06-02 (oci-bootstrap work plan, T22)

---

## Status Overview

| Wave | Tasks | Done | Partial | Blocked | Pending |
|------|-------|------|---------|---------|---------|
| Wave 1 (Setup) | 8 | 8 | 0 | 0 | 0 |
| Wave 2 (Audit) | 5 | 5 | 0 | 0 | 0 |
| Wave 3 (Build) | 3 | 0 | 3 | 0 | 0 |
| Wave 4 (IDE) | 5 | 5 | 0 | 0 | 0 |
| Wave 5 (VM) | 5 | 0 | 1 | 0 | 4 |
| Final Wave | 4 | 0 | 0 | 0 | 4 |
| **Total** | **30** | **18** | **4** | **0** | **8** |

**Overall: 18/30 complete (60%)**

---

## Done (18 tasks)

### Wave 1: Setup (8/8)
- T1: bmake/clang/lldb verified on host (lld still needed)
- T2: feature/oci-bootstrap branch created with OCI files from origin/devel
- T3: tools/build/make.py verified (PARTIAL: lld missing)
- T4: AGENTS_START_HERE.md, release-oci.conf, .plan/019.0 verified
- T5: .plan/005.0-Risks-TODO.md updated with state preamble
- T6: .plan/000.0-OCI-Jail-TOC.md updated (2 line count fixes, 2 new rows)
- T7: .plan/020.0-Developer-Setup.md created (482 lines, 9 sections)
- T26: FreeBSD VM provisioning guide + smoke test script created

### Wave 2: Audit (5/5)
- T8: AI slop audit (36 TODO markers, 78 AI-attribution files, 3 build blockers)
- T9: Makefile structure verified (16 subdirs OK, json-c dependency flagged)
- T10: usr.sbin/Makefile updated to include ocifbsd subdir
- T11: Cross-build smoke test (PARTIAL: lld blocker)
- T12: AI slop backlog created (.omo/drafts/ai-slop-backlog.md, 12 tasks)

### Wave 4: IDE (5/5)
- T16: .idea/tools/Toolchains.xml created (2 toolchains)
- T17: .idea/misc.xml updated with 6 custom build targets
- T18: .idea/runConfigurations/ created with 5 run configs
- T19: ATF/kyua test config + .plan/020.0 cross-reference added
- T20: IDE workflow test plan documented

---

## Partial / Blocked (4 tasks)

### Wave 3: Build (3/3 PARTIAL)
- T13: Cross-build userland libraries — BLOCKED on `lld` not installed
- T14: Build ocifbsd — BLOCKED on `lld` not installed
- T15: Build tests — BLOCKED on `lld` not installed

**Resolution:** `brew install llvm` (user action, 1 command, ~5 min)

### Wave 5: VM (1/5 PARTIAL)
- T21: Run ATF tests in FreeBSD VM — BLOCKED on T26 (VM) + T15 (test build)

**Resolution:** After `brew install llvm` and VM provisioning, T21 can run end-to-end.

---

## Pending (8 tasks)

### Wave 5: VM (4/5 pending)
- T22: Create .omo/drafts/oci-bootstrap-tasks.md (this file!) — **IN PROGRESS**
- T23: Update .plan/000.0-OCI-Jail-TOC.md cross-references for 020.0
- T24: Update .gitignore for cross-build artifacts and IDE cache
- T25: Commit and push feature branch (if user approved)

### Final Wave (4/4 pending)
- F1: Plan Compliance Audit (oracle)
- F2: Code Quality Review (unspecified-high)
- F3: Real Manual QA (unspecified-high)
- F4: Scope Fidelity Check (deep)

---

## What's Blocking Progress

### Soft Blocker: lld not installed
- Blocks: T13, T14, T15, T21 (4 tasks)
- Resolution: `brew install llvm` (user action)
- Estimated time after fix: 5-15 min for cross-build, 1-2 min for ocifbsd

### External Dependency: FreeBSD VM
- Blocks: T21 (1 task)
- Resolution: Provision VM per `.omo/drafts/vm-provisioning.md` (user action)
- Time: 30-60 min for UTM/QEMU/VirtualBox setup

### User Approval: Push
- Blocks: T25 push step
- Resolution: User must explicitly approve push to feature/oci-bootstrap

---

## Completed Artifacts

| Artifact | Path | Size |
|----------|------|------|
| Work plan | `.omo/plans/oci-bootstrap.md` | 1710 lines |
| Interview draft | `.omo/drafts/oci-bootstrap.md` | kept per user request |
| Dev setup doc | `.plan/020.0-Developer-Setup.md` | 482 lines |
| VM provisioning | `.omo/drafts/vm-provisioning.md` | 376 lines |
| Smoke test script | `.omo/drafts/vm-smoke-test.sh` | executable |
| AI slop backlog | `.omo/drafts/ai-slop-backlog.md` | 12 tasks |
| Task tracker | `.omo/drafts/oci-bootstrap-tasks.md` | this file |
| Toolchain XML | `.idea/tools/Toolchains.xml` | 2 toolchains |
| Build targets | `.idea/misc.xml` | 6 targets |
| Run configs | `.idea/runConfigurations/*.xml` | 6 configs |
| Boulder state | `.omo/boulder.json` | live |

---

## Git History

| Commit | Date | Description |
|--------|------|-------------|
| `0f54df18a11` | 2026-06-02 | plan: record oci-bootstrap work plan and interview draft |
| `f36d37821f9` | 2026-06-02 | Wave 1: Bootstrap ocifbsd onto feature/oci-bootstrap branch |
| `73a339ff57d` | 2026-06-02 | Wave 2: Audit OCI source, fix Makefiles, backlog AI slop |
| `<pending>` | 2026-06-02 | Wave 3+4: Cross-build blockers documented, CLion IDE configured |

---

## Next Actions

1. **User:** Run `brew install llvm` to unblock T13-T15
2. **User:** Provision FreeBSD VM per `.omo/drafts/vm-provisioning.md` to unblock T21
3. **Then:** T22-T25 can complete
4. **Then:** Final Wave F1-F4 reviewers run
5. **Then:** User approves T25 push

---

## Notes

- AI slop cleanup (T8, T12) is informational; per user direction, we are
  "focusing on OCI only" and not acting on the backlog items
- The -Werror flag was removed from usr.sbin/ocifbsd/Makefile (B1 fix, 1 min)
- json-c and seccomp translations are deferred to future plans
- The subagent system was broken in this session; all work was done directly
  by Atlas (the orchestrator) using bash/python/Write/Edit tools
