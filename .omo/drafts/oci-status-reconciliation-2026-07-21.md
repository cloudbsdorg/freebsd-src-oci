# OCI Status Reconciliation — 2026-07-21

## Why this note exists

The `.plan/` design set, `origin/devel` history, `feature/oci-bootstrap`, and `main` had drifted apart:

| Source | Claimed | Reality |
|--------|---------|---------|
| `.plan/005.0` on `main` | All Phase 0–5 tasks `NOT STARTED` | Wrong — large scaffold lives on feature branch |
| `origin/devel` | “352 tasks complete” | **False / unverified** AI bulk mark; do not trust |
| `feature/oci-bootstrap` OCI-STATUS | “BOOTSTRAP 100% COMPLETE” | **True for compile-all-SUBDIRs** on FreeBSD 16; **not** product-complete |
| `feature/oci-bootstrap` `.plan/005.0` | Only Phase 0 `IN PROGRESS` | Stale relative to June bootstrap finish |
| `main` | Clean FreeBSD + plans | Correct: **no** `usr.sbin/ocifbsd/` on main |
| Feature vs main | — | Feature ~177 commits ahead, **~7 commits behind** `main` |

## What was done in this reconciliation

Updated on the working tree (docs only):

1. **`.plan/005.0-Risks-TODO.md`** — new §0 Current State; legend (`SCAFFOLDED` / `BUILDS` / `TESTED` / `COMPLETED` / `BLOCKED`); Phase 0–5 + extended module table.
2. **`.plan/003.0-Implementation.md`** — actual directory tree (`src/`, SUBDIR LIBs) vs early flat layout.
3. **`.plan/000.0-OCI-Jail-TOC.md`** — status snapshot, 019/020 rows, Task Index statuses for 0–5 and scaffold note for 6–18.
4. **`.plan/001.0-Overview.md`** — branch/status banner.
5. **`.plan/019.0-Package-Release.md`**, **`.plan/020.0-Developer-Setup.md`** — brought from feature onto this tree.
6. **This draft** — audit trail.

## Branch / merge facts

```
main @ dfe6a886ea7
  - Merge freebsd:main (twice in the lag window)
  - nuageinit / limits / etcupdate / libc / libarchive / nfs fixes

feature/oci-bootstrap @ 25d02cb5df2
  - usr.sbin/ocifbsd/ (~57 .c files, 15 SUBDIRs)
  - tests/usr.sbin/ocifbsd/ (42/42 convert tests)
  - AGENTS_START_HERE.md, OCI-STATUS.md, cross-build tools
  - Lacks the 7 commits currently only on main
```

### Required next step: upstream merge

```sh
git checkout feature/oci-bootstrap
git fetch origin
git merge origin/main   # or rebase — prefer merge for shared feature branch
# resolve conflicts if any
# re-run on FreeBSD 16:
#   make -C usr.sbin/ocifbsd
#   make -C tests/usr.sbin/ocifbsd && kyua test -k Kyuafile
```

Do **not** force-push. Do **not** treat scaffold modules as production-ready after the merge.

## Maturity summary (post-bootstrap)

| Layer | Maturity |
|-------|----------|
| Build system + all SUBDIRs compile | **Done** |
| Core CLI surface (create/start/kill/delete/state/list/inspect/run) | Scaffold + builds |
| Core lifecycle ATF | **Missing** (`ocifbsd_test.c.disabled`) |
| convert (K8s/Compose) | Partial tests; 2 known bugs pinned |
| image / network / security / orchestration / … | Libraries build; stubs & unproven behavior |
| rc.d, etc config, OCI conformance | Not started |
| On `main` | Plans only (after this doc pass) |

## Rejected interpretations

1. “Everything is done because devel said 352 tasks complete.”
2. “Nothing is started because main’s tracker said NOT STARTED.”
3. “Bootstrap 100% means ready to ship.”

Correct interpretation: **design is extensive; build bootstrap is done; verification and productization are largely still ahead.** Critical path starts with **merging upstream into the feature branch**.

## Related files

- `.plan/005.0-Risks-TODO.md` — authoritative task statuses
- `.plan/000.0-OCI-Jail-TOC.md` — document map
- `.omo/plans/oci-bootstrap.md` — original bootstrap work plan (historical)
- `usr.sbin/ocifbsd/OCI-STATUS.md` on feature — build log / changelog
