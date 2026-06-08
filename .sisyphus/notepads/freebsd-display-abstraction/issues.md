# Issues — freebsd-display-abstraction

## T1 (2026-06-06) — bhyvegc API audit

### Plan vs reality drift

- **Plan stated "430+ lines" for bhyvegc.c.** Actual is 100. Plan was written against an older or hypothetical version of the code. The audit report documents the actual line counts; downstream tasks should not assume the 430-line size.

- **Plan expected `bhyvegc_text_*` and `bhyvegc_draw_*` functions.** None exist. Text/draw lives in `amd64/vga.c` (x86-specific). This is important for T8 (console refactor) and T12 (displayd.ko) — the `bhyvegc` API will not grow text/draw functions, and a portable "console" abstraction must be built *on top* of bhyvegc, not by extending it.

- **Plan task body said "Commit: NO (read-only, no code change)"** but the user-instructions-for-this-T1 explicitly said to commit. Followed the user instructions. Worth flagging to the orchestrator: the per-task "Commit" field may be misleading for T-tasks that produce a draft document — the rule should be "commit if a deliverable is produced, even read-only ones."

- **Plan output path is `usr.sbin/bhyve/bhyvegc-coupling.md`.** User instructions for T1 specified `.sisyphus/drafts/t1-bhyvegc-audit.md`. Followed the user instructions. Downstream tasks T2–T7 will likely have similar path conflicts — they should follow the user-delegation instructions, not the plan's nominal "Output" path.

### Constraints found

- **`console` is a true singleton** — no per-instance state, no `console_destroy`, no `console_fb_unregister`. Multi-instance work (jail fb) cannot be done without T8 refactoring console.{c,h}.

- **No input fan-out.** `console_kbd_register` / `console_ptr_register` are single-callback with `>` priority. This is what T2 will document and what T8 (or a separate T2-follow-up) will need to address.

- **No graceful transport shutdown.** RFB is `rfb_init` once and the thread lives until process exit. T7 (transport registry) and T11 (rfb wrap) must add a real `transport_shutdown`.

- **Pixel format is hardcoded RGBA32.** No YUV, no RGB565. RFB adapts via `pixfmt` mutation, not buffer layout. Future BDP / multicast / RDP transports may want a different pixel format; the audit recommends adding a pixel-format enum to `bhyvegc_image` as a backward-compatible extension.

## T19 (2026-06-06) — GPU kernel audit issues

### Plan-path discrepancies (must be fixed before T20/T21/T22 are worked)

- **T19/T20/T21 references `sys/dev/drm/`, but the actual path is `sys/dev/drm2/`.** The `drm2` name was introduced when DRM2 superseded the old `drm` in the FreeBSD-13 → 14 transition. Four locations in the plan body need to be updated.

- **T19 references `sys/modules/drm/` for build wiring.** This directory does not exist. DRM2 is statically compiled via `optional drm2` in `sys/conf/files:1577+`. T20's gpu_resource module can be loadable (the design should assume it is), but the underlying DRM2 core is static.

- **T19 references `compat/nvidia/` for nvidia.ko.** The `compat/` directory does not exist in this src tree. nvidia.ko lives in `compat/nvidia/` only when the user unpacks the NVIDIA tarball. T20's framework must handle the absent-nvidia-ko case (default to `gpu_backend_stub`).

- **T19 references `sys/kern/kern_cgroup.c` for the cgroup controller pattern.** This file does not exist. No native cgroup framework is in this tree. T20 should follow the `racct` pattern (`sys/kern/subr_racct.c`) for per-jail resource accounting, not invent a cgroup controller.

- **T19 names `devfs_rule_add()` as a kernel-callable function.** It is not. The devfs rule API is ioctl-based (`DEVFSIO_RADD` on the devfs mount fd, dispatched by `devfs_rules_ioctl()` in `sys/fs/devfs/devfs_rule.c:160`). The userspace consumer is `sbin/devfs/rule.c:119`.

### Testability gap

- **No real GPU in this base to exercise the framework against.** `ls sys/modules/ | grep -iE "gpu|drm|kms|nvidia|amdgpu|radeon|video"` returns only `linuxkpi_video` and `videomode`. T18 and T46 can only validate the *path* through `prison_check_gpu`, not its correctness on a real GPU. The T20 acceptance criteria should explicitly require a `gpu_backend_stub`.

### Documentation drift

- The plan's T19 task body says "read `sys/dev/drm/` directory structure" — but the real path is `sys/dev/drm2/`. Future audit tasks should grep the tree for the actual location before quoting a path. The "linux-distribution" model of `sys/dev/drm/` is the upstream-Linux layout; the FreeBSD porting renames the directory but the content is the same.
