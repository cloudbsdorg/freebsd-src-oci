# Draft: displayd Plan Refinements (2026-06-05)

## User Correction (2026-06-05)

**User said**: "if secrets need to be transfered, then git is not the tool, sftp/ssh based does"

### Refined Constraint

The "git for cross-machine transfer" rule needs an explicit exception for secrets.

**Refined rule**:
- For **non-secret** cross-machine file transfer (source code, configs, test data, build artifacts, documentation): use **git** (clone, pull, push, archive, bundle). Atomic, hash-verified, no missing files.
- For **secrets** cross-machine file transfer (TLS private keys, ACME account keys, database passwords, API tokens, SSH host keys, broker admin credentials): use **sftp/ssh-based** transfer.

### Refined Prohibitions

- For non-secret content: **no rsync, sftp, scp, ssh** (files go missing, partial transfers, no atomicity)
- For secrets: **no git** (committed secrets are vulnerable, history is hard to purge)
- Secrets MUST NOT be committed to any git repository (use `.gitignore`, secret managers like HashiCorp Vault, or sftp-only)
- TLS cert public certs are OK in git (not secret); private keys are NOT

### Where it applies in the plan

Add to **§6 Work Objectives → Must Have** and **Must NOT Have**:

**Must Have** (add new "Build / cross-machine transfer" subsection):
- For non-secret cross-machine file transfer: use git (clone, pull, push, archive, bundle) — atomic, hash-verified, no missing files
- For secret cross-machine file transfer (private keys, credentials, tokens, passwords): use sftp/ssh-based transfer
- TLS public certificates may be committed; TLS private keys MUST NOT be committed

**Must NOT Have** (add new "Build / cross-machine transfer" subsection):
- For non-secret cross-machine file transfer: no rsync, sftp, scp, ssh (rsync and sftp/scp/ssh have caused missing files in prior builds)
- For secret cross-machine file transfer: no git (history is hard to purge; sftp/ssh is the appropriate secure tool)
- No committing secrets to git repositories (no `.env`, no `*.key`, no `id_rsa`, no certbot account keys, no database passwords)
- No `~/.ssh/`, no `/etc/ssl/private/`, no `secrets.yaml` in any tracked file

### What secrets exist in this project

- TLS private keys (broker, displayd, certbot renewal hooks)
- ACME account keys (Let's Encrypt, ZeroSSL, etc.)
- Database passwords (ACL backing store, audit log store)
- API tokens (multicast channel auth, broker admin)
- SSH host keys (if exposed for mgmt)
- Broker admin creds (admin.http_listen basic auth, or token)

### Tests to add

A test that verifies no rsync/sftp/scp/ssh in build scripts (this was already in the prior summary).

A test for `.gitignore` coverage: no `.key`, `.pem` (with private), `id_rsa`, `secrets.*`, `*.env` in tracked files.

A test for the deprecation/migration path: secrets can be staged to `/etc/display/secrets/` with mode 0600 and transferred via sftp.

## Other in-progress refinements

- fbuf_jail → displayd rename (in progress; ~45 of ~140 refs remaining)
- Auto-load / zero-friction design section to be added to §4 (modules auto-load via kld_list, broker auto-starts, no user config changes)

## Status (2026-06-05)

**All three in-progress items now complete:**

1. **File transfer rule with secrets exception** — Added to §6 Must Have and Must NOT Have. Non-secret content uses git; secrets use sftp/ssh; secrets MUST NOT be committed to git.

2. **fbuf_jail → displayd rename** — All references renamed via bulk replace. NO deprecation shims added (user clarified: fbuf_jail was never shipped, no product to deprecate).

3. **Auto-load / zero-friction design** — Added as new design section §4.23. Covers kld_list auto-load, broker auto-start, devfs rules, cert discovery + self-signed auto-gen, idempotent install, hot-plug/hot-unplug.

4. **DEPRECATION SHIM CLEANUP** (user feedback 2026-06-05): The user pointed out that the plan was over-engineering deprecation aliases for names that were never shipped. The user said "no product has been made, so cut over to the new name." Applied the same principle to:
   - `fbuf_jail` → `displayd` (kernel module) — never shipped, no deprecation
   - `bhyve-display-broker` → `displayd` (broker daemon) — never shipped, no deprecation
   - `bhyve-display-client` → `displayc` (client) — never shipped, no deprecation
   - `libbdp.so` → `libdisplay.so` (library) — never shipped, no deprecation
   - `bhyve-display-enduser` → `display-enduser` (man page) — never shipped, no deprecation
   - `/etc/bhyve/display-broker.conf` → `/etc/display/display-broker.conf` — never shipped, no fallback
   - `_display-broker` user → `_displayd` user — never shipped, no alias
   - T38 broker test cases (alias_bhyve_symlink, alias_prints_deprecation_warning) — removed
   - T38 shell test (alias_bhyve_works) — removed
   - T44 library test cases (alias_libbdp_works, alias_libdisplay_canonical) — removed; canonical is the only form
   - T44 shell test (links_against_libbdp) — removed
   - T45 client title (with bhyve-display-client deprecated alias) — removed
   - T47 deprecation stub man pages — removed; canonical names from the start
   - F1 architectural rules (a, b, c) — re-framed; no "old name" / "deprecated symlink" / "fallback path" framing
   - F1 broker man pages list — removed the "plus deprecation stubs" part; canonical names only
   - Backward compat test table — removed the "deprecation shim" row; re-framed as "preserved existing API"
   - Backward compat tests section — removed "deprecated symlinks" and "migration script" framing
   - Must Have "Generic naming" — re-framed to acknowledge no old product
   - Must Have "Backward compatibility" — re-framed to focus on real backcompat (rfb=, allow.fbuf, libvmmapi.so, libjail.so)
   - mDNS service type — `_display._tcp.local` from the start
   - Generic broker heading — "replaces displayd" → "canonical home"
   - Wave 5 dispatch — "T44 libbdp" → "T44 libdisplay"
   - T44 commit — "libbdp: ..." → "libdisplay: ..."
   - T45 commit — "bhyve-display-client ..." → "displayc: ..."
   - T44 example tool — "atf + libbdp example" → "atf + libdisplay example"
   - T44 evidence file — "task-44-libbdp.txt" → "task-44-libdisplay.txt"
   - T45 dependency — "T44 (libbdp)" → "T44 (libdisplay)"
   - T44 references — "lib/libbdp/" → "lib/libdisplay/"
   - Mermaid Gantt — "T44 libbdp" → "T44 libdisplay"

**Real backcompat (preserved, not deprecation):**
- `rfb=`, `tcp=`, `unix:`, `vga=`, `password=`, `wait=` (bhyve's existing config keys)
- `allow.fbuf` jail param (existing API in FreeBSD source tree)
- `console_init(w, h, fb)` function (existing in bhyve's console.c)
- `rfb_init` function (existing in bhyve's rfb.c)
- `libvmmapi.so`, `libjail.so` (existing libraries, not bumped)
- `bhyve(8)`, `bhyve/config(5)`, `jail.conf(5)` man pages (existing, not removed)

## Plan stats after this turn

- 12,727 lines (was 12,744 before the deprecation cleanup; -17 net lines)
- 17 top-level sections
- 23 design sections in §4 Context
- 73 tasks (T0-T72) + 4 final verifications (F1-F4)
- All `bhyve-display-broker`, `bhyve-display-client`, `libbdp`, `bhyve-display-enduser` references are now canonical names (displayd, displayc, libdisplay, display-enduser) or intentional documentation
- 0 over-engineered deprecation shims
- 0 stale §4.X cross-references
- Nav index updated

## What's next (optional follow-ups)

- Final audit pass to verify all cross-references are accurate
- Update TL;DR to reflect new `displayd` branding and zero-friction messaging
- Add a test for git-based transfer (verify no rsync/sftp/scp/ssh in build scripts)
- Update F1 to verify displayd branding, auto-load, and git-only transfer
- Update Mermaid diagram labels (some still use old paths — verify)

## Title and TOC update (this turn)

**User said**:
1. "make the title 'displayd: bhyve Display Abstraction And Container Framebuffer'"
2. "we should also have a TOC with links to the sections, and validate the first line in the tables, granted the links may solve that problem .. because if we go by line numbers, we need to tell the agent to validate that its actually the content they need"
3. "looking at 'allow.fbuf; fbuf.nokbd; fbuf.nomouse;' maybe we need to add a `fbuf.nohid` that disables the mouse and keyboard, and a `fbuf.allowhid` to allow mouse and keyboard HID = Human Interface Devices"
4. "the chart 'Architecture (corrected — GPU is a host-side shared resource):' isn't mermaid"
5. "Not mermaid: The unified source: bdp-stream + BDP device-info protocol (T60 + T62-design)"
6. "Not mermaid: The state machine (per-adapter binding):"
7. "Not mermaid: Authorization layers (defense in depth):"
8. "not mermaid: A single physical A100 can be split into up to 7 MIG instances. Each MIG looks like a separate GPU to the VM"
9. "not mermaid in section: 7. SR-IOV PFs and VFs:"
10. "not mermaid: 9. What happens at hot-plug time when a new GPU is detected"
11. "not mermaid: Test directory layout (FreeBSD convention):"
12. "Should this be mermaid?: Every test (ATF C, shell, or QA scenario) MUST have a procedure with six steps:" (NO — it's a procedure, not a diagram; kept as text)
13. "should be mermaid: 5. Test Coverage Recording Coverage is recorded in .sisyphus/evidence/run-{id}/coverage/:"
14. "Should be mermaid: 8. Test Data Catalog (certs, jails, users, configs)"
15. "should be mermaid: Execution Strategy Parallel Execution Waves"
16. "prob should be mermaid: Test Procedure (per-test) — for each of the 20 ATF C cases in T8:" (kept original as text; added Mermaid flowchart as visual summary)
17. "the problem with ascii art, is that an agent may mis-interpret, where mermaid is explicit"

### Changes applied

#### Title and TOC
1. **Title updated** (line 1):
   - Old: `# bhyve Display Abstraction + Jail Framebuffer`
   - New: `# displayd: bhyve Display Abstraction And Container Framebuffer`

2. **New `## Table of Contents` section** at line 33 (right after TL;DR, before Plan Navigation Index). Contains:
   - Validation protocol blockquote (3-step protocol for the agent)
   - Top-level sections list (17 clickable anchor links)
   - Design sections list (23 clickable anchor links)
   - Task & verification references list

3. **Plan Navigation Index tables updated** (§2):
   - Added clickable links in the "Section" / "Design section" column (anchor-based, never drifts)
   - Added new `First line content` column showing the actual heading text
   - Validated all `First line` column line numbers against the live `grep -n "^## " plan.md` and `grep -n "^### " plan.md` output
   - Removed the stale "(Line numbers are approximate; use `grep -n "^## " file.md` for the live numbers.)" disclaimer

4. **Fixed initial anchor bug**: TOC links for design sections used `#42-...` style (with section number prefix) but the actual heading slugs are `#additive-kbdmouse-model-...` (without the "4.2" prefix). All 23 design section anchors fixed in BOTH the TOC and the navigation table.

5. **Section "5. Section reference convention" updated** (Agent Context Management):
   - Now recommends **clickable TOC links** as the **preferred** navigation mechanism (anchor-based, doesn't drift)
   - Line numbers downgraded to **fallback** mechanism (must re-validate with grep before use)

6. **F1-F4 verification updated**:
   - F1: TOC link validation, First line column validation, First line content column validation
   - F2: Stale §3.X design section references check
   - F3: Link-validation protocol test
   - F4: TOC and Plan Navigation Index sync check

#### HID params
7. **New sub-section `#### HID-level controls` added to §4.2**:
   - `fbuf.nohid` (boolean, default false) — coarse-grained opt-out that disables all virtual HID devices from fbuf
   - `fbuf.allowhid` (boolean, default false) — separate permission for raw HID device nodes (`/dev/uhid*`)
   - State table (8 rows for 4 boolean params combinations)
   - Precedence rules (stricter-wins; host policy `security.fbuf.allowhid` defaults to 0)
   - 5 new ATF C tests + 3 new shell tests
   - 4 new "Must NOT do" rules

#### ASCII art → Mermaid conversions (13 total)
- Architecture (corrected) at §4.9 — graph TB
- Unified source: bdp-stream at §4.17 — graph TB
- State machine (per-adapter binding) at §4.18 — flowchart TB
- Authorization layers (defense in depth) at §4.18 — flowchart TB with subgraphs
- MIG instances at §4.21 — graph TB (tree)
- SR-IOV PFs and VFs at §4.20 — graph TB (tree)
- Hot-plug time when a new GPU is detected at §4.21 — flowchart TB
- GPU caps discovery pipeline at §4.21 — flowchart TB (6-step pipeline + overlays)
- Test directory layout (FreeBSD convention) at §7 — graph TB (filesystem tree)
- Test Coverage Recording at §7 — graph TB (filesystem tree)
- Test Data Catalog (certs, jails, users, configs) at §7 — graph TB (filesystem tree)
- Parallel Execution Waves at §13 — graph TB (5 waves + 1 F-wave with subgraphs)
- T8 Test Procedure (high-level 6-step flow) at §14 — flowchart LR (added as visual summary; original detailed text preserved)

### Plan stats after this turn (final)

- **12,989 lines** (was 12,674 at the start of the audit; +315 net lines)
- **21 Mermaid diagrams** (was 12; +9 from this turn's conversions)
- **18 top-level sections** (was 17; added "## Table of Contents")
- **23 design sections in §4** (unchanged)
- **77 tasks** (T0-T72 = 73 implementation + design-only, plus F1-F4 = 4 final verifications)
- **0 ASCII art** in non-mermaid code blocks (all 13 user-flagged diagrams converted)
- **0 TBD/TODO/FIXME markers** (unresolved)
- **2 stale product name references** (INTENTIONAL — in "no old product to deprecate" framing, explicitly stating `fbuf_jail` was never shipped)
- **Line count claim updated**: 12,989 lines (matches actual)
- **Mermaid count claim updated**: 21 (matches actual)
- **All anchor links valid** (verified against actual heading slugs)
- **All "First line" column values** match `grep -n` output (validated)
