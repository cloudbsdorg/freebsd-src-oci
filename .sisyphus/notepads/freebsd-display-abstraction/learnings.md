# Learnings — freebsd-display-abstraction

## T1 (2026-06-06) — bhyvegc API audit

### Patterns / conventions discovered

- **`bhyvegc` is a pure pixel-buffer abstraction, not a graphics console.** Despite the name, there is no text/draw logic inside it; that lives in `amd64/vga.c` (x86-specific). The plan's expectation of "430+ lines" and `bhyvegc_text_*` / `bhyvegc_draw_*` was wrong. Actual: 100 LOC, 4 public functions.

- **`console.c` is a vtable-style singleton.** Single file-static `struct console`, single kbd/ptr/fb-render consumer with priority-based replacement. Multi-instance is impossible without a refactor (T8 owns that).

- **The `raw` flag in `struct bhyvegc` is the key decoupling point.** When a consumer passes a `void *fbaddr` to `bhyvegc_init`, bhyvegc sets `raw=1` and uses the caller's buffer directly. When fbaddr is NULL, bhyvegc self-allocates. The future `displayd.ko` jail mmap can plug into this path with zero changes to bhyvegc.

- **`bhyvegc_image` is the proto-`fb_info_t`.** The plan introduces a new `fb_info_t` struct, but the existing `bhyvegc_image { vgamode, width, height, data }` is sufficient. Adding pixel-format + stride later is additive and backward-compatible.

- **All RFB transport is hard-wired via `rfb_init` (rfb.c:1341) called from `pci_fbuf_init` (pci_fbuf.c:455).** No vtable, no second transport. T4/T11 will replace this with a `display_transport` registry; the rest of `rfb.c` (encoder, client thread, msg parsers) can be reused as-is.

- **VMM coupling is at the pci_fbuf layer, not bhyvegc.** `vm_create_devmem(pi->pi_vmctx, VM_FRAMEBUFFER, "framebuffer", FB_SIZE)` is the single VMM call. bhyvegc only sees a `void *`.

### Recurring file references for downstream tasks

- `bhyvegc.{c,h}` — pixel buffer primitive (reusable as-is for displayd)
- `console.{c,h}` — singleton wrapper that must become multi-instance (T8)
- `pci_fbuf.c:444-449` — current call sites of `console_init` + `console_fb_register` + `console_get_image`; line 455 is `rfb_init` (target of T13 wire-up)
- `rfb.c:909, 932, 949` — `console_key_event` / `console_ptr_event` dispatchers (T2 input fan-out)
- `amd64/vga.c:319` — `vga_render` (vga_full + vgamode path)
- `amd64/ps2kbd.c:502` / `amd64/ps2mouse.c:415` — actual kbd/ptr consumers at priority 1
- `usb_mouse.c:317` — alternate ptr consumer at priority 10 (overrides ps2mouse)

### Cross-task flow

- bhyvegc verdict feeds T4 (transport vtable), T5 (backend vtable), T7 (transport registry)
- bhyvegc verdict unblocks T8 (console refactor)
- Combined with T2 (input fan-out), T8 has everything it needs

## T19 (2026-06-06) — GPU kernel subsystem audit

### Patterns / conventions discovered

- **DRM lives in `sys/dev/drm2/`, not `sys/dev/drm/`.** The plan used the latter; the actual path is `sys/dev/drm2/` (the `drm2` kernel option). This is the post-FreeBSD-13 name for the in-tree DRM/KMS core.

- **DRM2 is statically compiled, not a loadable module.** Every `drm2/*.c` is `optional drm2` in `sys/conf/files:1577+`. There is no `sys/modules/drm2/` and there shouldn't be one. The `agp` module (`sys/modules/agp/`) is loadable; DRM2 is not.

- **There is no `compat/` directory in src.** nvidia.ko (the proprietary FreeBSD driver) lives in `compat/nvidia/` only after the user unpacks the NVIDIA tarball. The only nvidia content in src is `sys/contrib/dev/nvidia/{LICENCE.nvidia, tegra124_xusb.bin.uu, tegra210_xusb.bin.uu}` — and those are ARM Tegra SoC XUSB firmware blobs, NOT x86 dGPU drivers.

- **There is no native cgroup framework in this tree.** `sys/kern/kern_cgroup.c` does not exist. The only `cgroup.h` is `sys/compat/linuxkpi/common/include/linux/cgroup.h` (a header-only shim for Linux guest drivers). The FreeBSD analog for per-jail resource accounting is `sys/kern/subr_racct.c` (used by `kern_jail.c` for `allow.set_hostname`, `prison_racct_add()`, etc.). T20's "VRAM cgroup" should follow the `racct` pattern, not invent a cgroup controller.

- **devfs rule creation is ioctl-based, not function-call-based.** The plan referenced `devfs_rule_add()` as a kernel-callable function; it does not exist. Rules are added via the `DEVFSIO_RADD` ioctl on the devfs mount fd (`sys/fs/devfs/devfs_rule.c:160`), populated with a `struct devfs_rule` (`sys/fs/devfs/devfs.h:66-100`). The userspace consumer is `sbin/devfs/rule.c:119`.

- **The DRM cdevsw is a singleton — every `/dev/dri/*` minor uses the same `drm_cdevsw`.** The cdevsw lives at `sys/dev/drm2/drm_stub.c:73-82` and has 5 entries: `drm_open`, `drm_read`, `drm_ioctl`, `drm_poll`, `drm_mmap_single`. Minor types are `DRM_MINOR_LEGACY` (path `dri/card%d`), `DRM_MINOR_RENDER` (`dri/renderD%d`), `DRM_MINOR_CONTROL` (`dri/controlD64`). All created by the same `make_dev_p` call site at `drm_stub.c:379`.

- **DRM device defaults are `root:video 0660`**, hardcoded in `drmP.h:1594-1596` as `DRM_DEV_UID=UID_ROOT`, `DRM_DEV_GID=GID_VIDEO`, `DRM_DEV_MODE=0660`. To restrict per-jail, the jail's devfs ruleset can override these via a `dri/*` glob + `mode/uid/gid` actions.

- **The DRM hook-point map for per-jail governance is 8 sites:** `drm_open` (allow/deny open), `drm_open_helper` (per-open `drm_file` reservation), `drm_read` (legacy DMA read), `drm_ioctl` (per-ioctl policy), `drm_poll` (per-poll), `drm_mmap_single` (per-mmap dispatch), `drm_gem_mmap_single` (GEM mmap), and — the critical one for VRAM — `ttm_mem_global_alloc` in `sys/dev/drm2/ttm/ttm_bo.c:1229` (host-wide VRAM accounting). The minimum viable set is `drm_open` + `drm_ioctl` + `ttm_mem_global_alloc`.

- **The existing `prison_check_*` family follows a clear convention** (`sys/kern/kern_jail.c:3953,4058,4076` + `sys/sys/jail.h:468-522`): `int prison_check_X(struct ucred *cred, ...) → 0 / errno`. T20's `prison_check_gpu` should follow the same convention and register its jail params via `prison_add_allow("gpu", "open", ...)` (`kern_jail.c:5253`).

- **No KMS driver is in this base.** `ls sys/modules/ | grep -iE "gpu|drm|kms|nvidia|amdgpu|radeon|video"` returns only `linuxkpi_video` and `videomode`. No `i915kms`, no `amdgpu`, no `radeonkms`, no `nouveau`. **T20's gpu_resource framework will be design-only and untestable on real hardware in this base.** T18/T46 can only validate the *path* through the framework, not its correctness on a real GPU. The framework must have a `gpu_backend_stub` so ATF cases can exercise `prison_check_gpu` without a GPU.

### Recurring file references for downstream tasks

- `sys/dev/drm2/drm_stub.c:73-82` — the singleton cdevsw; the chardev registration is the only `/dev/dri/*` producer
- `sys/dev/drm2/drm_fops.c:120,491,550,581` — `drm_open`, `drm_read`, `drm_poll`, `drm_mmap_single` (5 hook points)
- `sys/dev/drm2/drm_drv.c:355` — `drm_ioctl` dispatcher
- `sys/dev/drm2/drm_gem.c:456` — `drm_gem_mmap_single` (cdev_pager_allocate ties the BO's vm_object to a jail)
- `sys/dev/drm2/ttm/ttm_bo.c:1229-1248,1435` — `ttm_mem_global_alloc` (per-BO VRAM choke point) and `ttm_bo_init_mm` (per-domain host cap at attach)
- `sys/dev/drm2/ttm/ttm_memory.c:38-124` — `struct ttm_mem_global` + helpers (host-wide VRAM accounting struct)
- `sys/dev/drm2/drmP.h:1594-1596` — `DRM_DEV_UID/GID/MODE` constants (root:video 0660)
- `sys/fs/devfs/devfs.h:66-100,105-113,190-191` — `struct devfs_rule`, `DEVFSIO_*` ioctls, `devfs_ruleset_*` (per-jail visibility)
- `sys/fs/devfs/devfs_rule.c:160,343-357` — `devfs_rules_ioctl`, `devfs_rule_applyde_recursive`, `devfs_rule_run`
- `sbin/devfs/rule.c:116-122` — userspace rule consumer (parses `devfs.rules`)
- `sys/kern/kern_jail.c:3953,4058,4076,5253` — `prison_check_af`, `prison_check`, `prison_check_nfsd`, `prison_add_allow` (the model for T20's `prison_check_gpu` and `prison_add_allow("gpu", ...)`)
- `sys/sys/jail.h:468-522` — all `prison_check_*` declarations
- `sys/kern/subr_racct.c` — `racct` (FreeBSD's per-jail resource accounting — model for VRAM cap)

### Cross-task flow

- T19 verdict feeds T20 (gpu_resource design) and T21 (gpu jail params + module).
- T19's path-correction list (§0 of the audit, §10 of this notepad) must be applied to T19/T20/T21/T22 acceptance criteria before they are worked.
- T20's `prison_check_gpu` should be a 4-bit `PR_ALLOW_GPU_{OPEN,AUTH,MMAP,IOCTL}` family + 2 numeric params (`gpu.vram_cap`, `gpu.time_slice_us`).
- T21's `gpu_resource` module must compile and be testable even when no GPU is present — via a `gpu_backend_stub` that returns synthesised numbers.

## T2 (2026-06-08) — Input fan-out audit learnings

### Patterns / conventions discovered

- **The input path has a clean three-layer separation** that T8 / T12 can exploit: (1) **transport** (rfb.c reads bytes from the wire), (2) **dispatcher** (console.c:106,113 single-cb invocation), (3) **consumer** (ps2kbd_event, ps2mouse_event, umouse_event — the bhyve-side bridge to the guest). Layers 1 and 2 are arch-agnostic and reusable. Layer 3 is what changes for jails.
- **The bhyve input path is "transport bytes → kernel pushes to guest via IRQ / endpoint"** (rfb → console → consumer → vm_isa_pulse_irq or hci_intr). The jail path is the opposite: "transport bytes → kernel writes to a per-jail buffer → jail's `/dev/kbd0` / `/dev/ums0` reads." T2 is the first task in the plan that explicitly identifies this direction reversal.
- **All three input consumers register a single callback with a priority.** Strict `>` comparison means equal priorities keep the first registrant. The umouse at priority 10 wins over ps2mouse at priority 1 when both are registered. This is well-defined behaviour and not a bug, but it constrains the future design: a multi-instance refactor (T8) must keep the same priority semantics, just made per-instance.
- **`input_detected` is a per-client RFB protocol flag** (rfb.c:910, 933, 950). It signals "the desktop is non-empty" to the VNC client, which prevents the client from suspending its frame requests. T11's rfb wrap must preserve this.
- **The keysym → scancode translation happens inside `ps2kbd_event`** (ps2kbd.c:405 → ps2kbd_keysym_queue at ps2kbd.c:390). It is **bhyve-VM-specific** (PS/2 scancodes). A jail consumer that wants raw RFB keysyms (for text-input-aware jails) would not need this translation.
- **`umouse_event` is the only consumer that reads `gc_image`** (usb_mouse.c:262, 287-288) to scale VNC pixel coords to the USB mouse's report range. The VNC coordinate scaling is specific to a single screen with a single client. A multi-display / per-jail consumer would need its own scaling.

### Recurring file references for downstream tasks

- `usr.sbin/bhyve/rfb.c:899,916,940` — `rfb_recv_key_msg`, `rfb_recv_client_msg`, `rfb_recv_ptr_msg` (the three transport-side dispatch sites; T11 wraps these)
- `usr.sbin/bhyve/rfb.c:909,932,949` — `console_key_event` / `console_ptr_event` call sites; T11 must preserve these
- `usr.sbin/bhyve/console.c:85-117` — the dispatcher + register functions; T8 owns the per-instance refactor here
- `usr.sbin/bhyve/console.h:35-36,38-51` — the public API surface (kbd/ptr typedefs + register/event funcs); T8 extends this
- `usr.sbin/bhyve/amd64/ps2kbd.c:394-410` — `ps2kbd_event` (the PS/2 kbd consumer; amd64-only)
- `usr.sbin/bhyve/amd64/ps2mouse.c:372-399` — `ps2mouse_event` (the PS/2 mouse consumer; amd64-only)
- `usr.sbin/bhyve/usb_mouse.c:256-295` — `umouse_event` (the USB mouse consumer; source is arch-agnostic but only wired on amd64 in practice)
- `usr.sbin/bhyve/amd64/atkbdc.c:142-158,242,251,499-509,545-549` — `atkbdc_assert_kbd_intr` / `atkbdc_assert_aux_intr` / `atkbdc_event` (the bhyve-side bridge to guest IRQs 1 + 12)
- `usr.sbin/bhyve/amd64/bhyverun_machdep.c:358` — the only call site of `atkbdc_init(ctx)` (amd64-only)

### Cross-task flow

- T2 verdict feeds T8 (console refactor → per-instance), T11 (rfb wrap → preserves dispatch sites), T12 (displayd module → needs per-jail consumer), T13 (pci_fbuf wire → does not need to touch input fan-out).
- T8 cannot be worked until T3 (jail API audit) is reconciled: T3 is marked `[x]` in the plan but has no draft/evidence/checkpoint, so its actual completion state is unknown.
- The T2 verdict **does not block T4 (transport vtable) or T5 (backend vtable)** — those can proceed because they wrap the existing dispatcher layer, which is reusable.
- The T2 verdict **informs T12's design**: the new `displayd_kbd_event` / `displayd_ptr_event` consumers will register against the (post-T8) per-instance `console` API. The `arg` parameter will be a `struct displayd_jail_fb *` (or similar per-jail handle).
- The T2 verdict **informs T38 (broker daemon)**: the broker will need a transport that delivers input to a per-jail consumer, not to a per-VM consumer. The transport layer (rfb.c / future bdp.c) is unchanged; the consumer layer is what changes.
