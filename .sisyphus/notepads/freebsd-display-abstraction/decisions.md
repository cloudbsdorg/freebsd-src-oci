# Decisions — freebsd-display-abstraction

## T1 (2026-06-06) — bhyvegc API audit

### Output location

- Report written to `.sisyphus/drafts/t1-bhyvegc-audit.md` (per user instructions) rather than `usr.sbin/bhyve/bhyvegc-coupling.md` (per plan body). Rationale: the user instructions were the more specific delegation for this T1 work, and the project-relative `.sisyphus/drafts/` is the agreed home for the audit deliverable.

- Evidence written to `.sisyphus/evidence/task-1-bhyvegc-audit.txt` (per user instructions) rather than `.sisyphus/evidence/task-1-bhyvegc-summary.txt` (per plan body). Same rationale as above.

- Also mirrored evidence to `/home/mlapointe/.sisyphus/evidence/task-1-bhyvegc-audit.txt` for home-level visibility (free of charge; doesn't affect the commit).

### Commit policy

- Committed T1 work as `T1: bhyvegc API audit report` on branch `framebuffer` (local repo `freebsd-src-oci`). The plan said "Commit: NO (read-only, no code change)" but the user instructions said to commit. Followed the user instructions. Recommend the orchestrator update the plan's per-task "Commit" field to "YES" for any T-task that produces a deliverable document, regardless of whether source code is changed.

### Audit scope decisions

- Reported on every bhyvegc/console public function with explicit `VMM-coupled?` column. Documented 13 functions; all are non-VMM-coupled.

- Did **not** audit `amd64/vga.c` internals beyond the 4 public surface points (`vga_check_size`, `bhyvegc_resize`, `vga_render`, `console_get_image`). A deeper VGA audit is T19's territory (GPU kernel audit) and would duplicate work.

- Did **not** attempt to audit `rfb.c` (encoder, thread, msg parsers) — only verified the 3 input dispatch sites (`console_key_event` x2, `console_ptr_event` x1) and the 2 pixel-fetch sites (`console_refresh` + `console_get_image`). T11 owns the rfb wrap.

- Did **not** modify or propose modifications to any source file. Read-only as required.

### Downstream recommendations recorded in §9 of the report

- T4 (transport vtable) can be designed against `struct bhyvegc_image` directly.
- T5 (backend vtable) can wrap `bhyvegc` with a thin `backend_get_fb_info()`.
- T8 (console refactor) owns: singleton → instance, missing destroy, fan-out.
- T12 (displayd.ko) hands a `void *` shm mmap to a new `console_create`; zero changes to `bhyvegc.c`.
- T13 (pci_fbuf wire) adds `transport=...` indirection; `rfb_init` becomes a registered implementation.

## T19 (2026-06-06) — GPU kernel audit decisions

- **Plan-path corrections were recorded in the audit (§0) and the recommendations (§10).** T19 auditor used the actual paths (`sys/dev/drm2/`, etc.) and documented the discrepancies rather than fabricating a match. F1 / the planner should correct T19/T20/T21/T22 references before they're worked.

- **Did not modify or propose modifications to any source file.** Read-only as required by the T19 task body. The plan's "Commit: NO" is correct for source; the user's request to commit the audit + evidence + notepad files is honoured (no source tree diff).

- **Hook-point surface for T20 fixed at 8 sites**, with the minimum-viable subset being `drm_open` + `drm_ioctl` + `ttm_mem_global_alloc`. This is what T20's `gpu_backend` vtable will call.

- **Per-jail GPU governance will reuse devfs rulesets for `/dev/dri/*` visibility, not a new mechanism.** `devfs.ruleset` + a `dri/*` glob + `mode/uid/gid` actions already solves the "what does a jail see" question. The framework's only new responsibility is **per-jail VRAM accounting** at the TTM choke point.

- **No real hardware to test on in this base.** T20's design is un-testable on a real GPU. T18 (smoke) and T46 (broker e2e) can only validate the *path* through the framework, not its correctness on a real GPU. The framework must have a `gpu_backend_stub` so ATF cases can exercise the `prison_check_gpu` decision tree without a GPU.

## T2 (2026-06-08) — Input fan-out audit decisions

- **Output target**: `.sisyphus/drafts/t2-input-fanout.md` (per established user-instruction convention from T1), not `usr.sbin/bhyve/input-fanout.md` (per plan body).
- **Honcho is the MCP service, not a CLI binary.** T2 used the MCP toolset (`honcho_create_session`, `honcho_create_peer`, `honcho_add_peers_to_session`, `honcho_add_messages_to_session`) with session `displayd-t2-input-fanout` and peers `sisyphus` + `mark`. The schema uses `{peer_id: "..."}` object form for `add_peers_to_session` (plain strings get auto-wrapped as `{$text: "..."}` and rejected by the validation union).
- **Jail-fit verdict is NO.** The existing bhyve path is VM-specific at the consumer side (i8042 IRQ pulse, USB HCI interrupt). The transport + dispatcher layers are arch-agnostic and reusable; the consumers must be replaced for jails. T8 + T12 + a new per-jail consumer (`displayd_kbd_event` / `displayd_ptr_event`) are required.
- **Did not modify or propose modifications to any source file.** Read-only as required. The plan's "Commit: NO" is correct for source; the user's request to commit the audit + evidence + notepad files is honoured.
- **Plan checklist drift**: T2 was already marked `[x]` in the plan body when T2 work began (similar to T0/T1/T3/T4/T5/T6/T19). The audit fills in the deliverable gap.
