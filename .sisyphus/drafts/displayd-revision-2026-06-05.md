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
