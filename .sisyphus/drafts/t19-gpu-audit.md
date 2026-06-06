# T19 — GPU kernel subsystem audit (DRM, devfs, nvidia presence)

**Task**: T19 from `.sisyphus/plans/freebsd-display-abstraction.md` (Wave 1, recon).
**Branch**: `framebuffer` on `freebsd-src-oci` (host `pppoe1.cloudbsd.org`). Plan said `displayd`/`freebsd-src-pppoe`; the actual workdir/commit history is the same `freebsd-display-abstraction` plan and the same T19 body — the repo is `freebsd-src-oci` per the on-disk state (see T1 checkpoint `.sisyphus/state/task-1.checkpoint.json`).
**Read-only**: no source code modified. All findings are from `git show HEAD:path` semantics — files inspected in-place, not edited.
**Verdict**: **"DRI is the right abstraction; DRM2 hooks available at open/mmap/ioctl, but the actual KMS drivers (i915kms, amdgpu, radeonkms, nvidia.ko) are NOT in this base tree."** The DRM2 framework is present and ready to wrap; the GPU hardware drivers are not, so the audit must flag that T20/T21 will design a framework over an empty driver surface.

---

## 0. Plan-path discrepancies (must be flagged to the planner)

The T19 task body in the plan references four source paths. **Three of them do not exist in this tree.** I worked from the actual paths and flagged the mismatch here so the planner can correct T20/T21 references:

| Plan says | Actual in tree | What it is |
|---|---|---|
| `sys/dev/drm/` (the path the plan uses 4×) | **`sys/dev/drm2/`** | DRM2 is the FreeBSD-rewritten DRM/KMS core, declared as the `drm2` kernel option in `sys/conf/files:1577+` |
| `sys/modules/drm/` (the plan says to check "build wiring") | **does not exist** | DRM2 is statically compiled in via `optional drm2` in `sys/conf/files`; no `sys/modules/drm{,2}/` directory exists in the tree |
| `compat/nvidia/` (the plan says to check "nvidia.ko presence") | **`compat/` does not exist**; only `sys/contrib/dev/nvidia/{LICENCE.nvidia, tegra124_xusb.bin.uu, tegra210_xusb.bin.uu}` | nvidia.ko is the proprietary FreeBSD driver and lives in `compat/nvidia/` (downloaded tarball), not in src. The only nvidia content in src is the Tegra XUSB firmware blobs and licence text |
| `sys/kern/kern_cgroup.c` (the plan says to read for cgroup controller pattern) | **does not exist** | No native cgroup v1/v2 controller framework. The only `cgroup.h` is the linuxkpi shim at `sys/compat/linuxkpi/common/include/linux/cgroup.h` (header-only, for Linux guest drivers) |
| `devfs_rule_add()` (the plan names it as a kernel-callable function) | **does not exist as a function** | devfs rules are created via the `DEVFSIO_RADD` ioctl on the devfs mount fd, dispatched by `devfs_rules_ioctl()` in `sys/fs/devfs/devfs_rule.c:160`. The userspace consumer is `sbin/devfs/rule.c:119` |

These discrepancies are not "T19 auditor mistakes" — they are the plan's mapping of upstream-Linux terminology (`/dev/dri`, `kern_cgroup.c`, `compat/nvidia`) onto a FreeBSD tree that organises those concepts differently. The audit's findings below are all from the actual paths; the follow-up §10 has the recommended plan corrections.

---

## 1. DRM2 directory structure (the real DRM core)

**Path**: `sys/dev/drm2/`
**Files**: 77 (`.c` + `.h`)
**Total LOC**: 30,797
**Build wiring**: `sys/conf/files:1577+` lists every `.c` as `optional drm2` — i.e. compiled in (not loadable) when the `drm2` kernel option is on. No `sys/modules/drm2/`.

### 1.1 Top-level layout (relevant subdirs)

```
sys/dev/drm2/
├── drm_drv.c          (1,242 lines — driver core, attach/detach, IOCTL dispatch glue)
├── drm_fops.c         (632 lines — drm_open/drm_read/drm_ioctl/drm_poll/drm_mmap_single)
├── drm_stub.c         (463 lines — minor-device registration, cdevsw, make_dev_p)
├── drm_ioctl.c        (1,148 lines — DRM_*_IOCTL handler table + dispatch)
├── drm_gem.c          (1,099 lines — GEM object lifecycle + drm_gem_mmap_single)
├── drm_vm.c           (264 lines — legacy DMA mmap, mostly FREEBSD_NOTYET stubs)
├── drm_pci.c          (PCI bus glue)
├── drm_agpsupport.c   (AGP kernel API)
├── drm_auth.c         (DRM master/auth token)
├── drm_bufs.c         (legacy DMA buffer pool)
├── drm_context.c      (legacy DRM context)
├── drm_crtc.c         (KMS CRTC)
├── drm_crtc_helper.c  (KMS helper)
├── drm_dp_helper.c    (DisplayPort helper)
├── drm_edid.c         (EDID parser)
├── drm_fb_helper.c    (fbdev emulation)
├── drm_irq.c          (IRQ handler)
├── drm_lock.c         (legacy DRM lock)
├── drm_memory.c       (DRM_MEM_* pools)
├── drm_mm.c           (range allocator)
├── drm_modes.c        (display mode helpers)
├── drm_platform.c     (platform-device glue)
├── drm_scatter.c      (scatter-gather)
├── drm_sysctl.c       (per-device sysctls)
├── drmP.h             (private master header, 1,700+ lines)
├── drm.h              (legacy user ABI)
├── drm_core.h         (core flag macros)
├── drm_crtc.h, drm_crtc_helper.h, drm_mode.h, drm_fourcc.h
└── ttm/               (Translation Table Maps — AMD VRAM manager)
    ├── ttm_bo.c           (BO alloc/free, ~1,700 lines)
    ├── ttm_bo_api.h, ttm_bo_driver.h
    ├── ttm_bo_manager.c   (per-domain managers: SYSTEM/TT/VRAM)
    ├── ttm_bo_util.c
    ├── ttm_bo_vm.c        (fault/vm_ ops)
    ├── ttm_execbuf_util.c (command submission helper)
    ├── ttm_lock.c         (TTM validation locks)
    ├── ttm_memory.c       (ttm_mem_global — host-wide VRAM accounting)
    ├── ttm_agp_backend.c
    └── ttm_object.c
```

### 1.2 What is NOT here (the real surprise)

| Driver | Linux path | FreeBSD path | Present? |
|---|---|---|---|
| i915 / i915kms | `drivers/gpu/drm/i915/` | `sys/dev/drm2/i915/` or `sys/dev/drm/` | **NO** — neither in this tree |
| amdgpu | `drivers/gpu/drm/amd/amdgpu/` | `sys/dev/drm2/amdgpu/` or `sys/dev/drm/` | **NO** |
| radeon / radeonkms | `drivers/gpu/drm/radeon/` | `sys/dev/drm2/radeon/` or `sys/dev/drm/` | **NO** |
| nouveau (NVIDIA KMS) | `drivers/gpu/drm/nouveau/` | `sys/dev/drm2/nouveau/` or `sys/dev/drm/` | **NO** |
| nvidia (proprietary UMS) | n/a (binary blob) | `compat/nvidia/` (external tarball) | **NO** — `compat/` directory does not exist in src; only `sys/contrib/dev/nvidia/` (Tegra firmware + LICENCE) is in src |
| vmwgfx | `drivers/gpu/drm/vmwgfx/` | n/a | **NO** |

**Implication**: T20/T21 design a `gpu_resource` framework over an empty driver surface. The framework will be design-only and untestable on real hardware in this base. The DRM2 plumbing is correct; the KMS drivers would have to be ported (the `drm2` directory is the "glue" — the i915/amdgpu/nouveau backends were dropped during the FreeBSD-13 → 14 transition, see `tools/tools/drm/` for the legacy stub). **This is a critical caveat for T20.**

---

## 2. DRM2 hook points — the cdevsw surface

The single `drm_cdevsw` in `sys/dev/drm2/drm_stub.c:73-82` is the chardev entry table:

```c
static struct cdevsw drm_cdevsw = {
    .d_version    = D_VERSION,
    .d_open       = drm_open,
    .d_read       = drm_read,
    .d_ioctl      = drm_ioctl,
    .d_poll       = drm_poll,
    .d_mmap_single = drm_mmap_single,
    .d_name       = "drm",
    .d_flags      = D_TRACKCLOSE
};
```

Minor numbers are created by `drm_get_minor()` (`drm_stub.c:357-393`) and `make_dev_p` (line 379):

```c
ret = make_dev_p(MAKEDEV_WAITOK | MAKEDEV_CHECKNAME, &new_minor->device,
    &drm_cdevsw, 0, DRM_DEV_UID, DRM_DEV_GID,
    DRM_DEV_MODE, minor_devname, minor_id);
```

with `minor_devname` from `drm_stub.c:368-376`:

| `type` | `minor_devname` | Resulting path |
|---|---|---|
| `DRM_MINOR_LEGACY` | `"dri/card%d"` | `/dev/dri/card0`, `/dev/dri/card1`, … |
| `DRM_MINOR_RENDER` | `"dri/renderD%d"` | `/dev/dri/renderD128`, `/dev/dri/renderD129`, … |
| `DRM_MINOR_CONTROL` | (not enumerated above) | `/dev/dri/controlD64`, … |
| default | `"dri/card%d"` | (fallback) |

`DRM_DEV_*` defaults (`drmP.h:1594-1596`):
- `DRM_DEV_UID  = UID_ROOT`
- `DRM_DEV_GID  = GID_VIDEO`
- `DRM_DEV_MODE = 0660` (rw for `root:video`)

**Important**: `make_dev_p` with a path containing `/` creates the `dri/` parent directory automatically (devfs handles nested parents). So the only place `/dev/dri/*` can be created is `drm_stub.c`. There is no per-driver cdev — the same `drm_cdevsw` serves every minor.

### 2.1 Hook point map (file:line + what to wrap)

| File:line | Function | Signature | Wrap for what |
|---|---|---|---|
| `sys/dev/drm2/drm_fops.c:120` | `drm_open()` | `int drm_open(struct cdev *kdev, int flags, int fmt, DRM_STRUCTPROC *p)` | **Per-jail check on /dev/dri/* open.** This is the only entry that has `struct thread *p` in hand. Inject `prison_check_gpu(curthread->td_ucred, dev)` here. |
| `sys/dev/drm2/drm_fops.c:176` | `drm_open_helper()` | `static int drm_open_helper(struct cdev *kdev, int flags, int fmt, DRM_STRUCTPROC *p, struct drm_device *dev)` | **Per-jail reservation of a `drm_file` slot.** Same place, but also pass `cred` into `dev->driver->open()` so the driver's `open` callback can enforce further policy. |
| `sys/dev/drm2/drm_fops.c:491` | `drm_read()` | `int drm_read(struct cdev *kdev, struct uio *uio, int ioflag)` | Read-side enforcement (legacy DMA buffer reads, mostly FREEBSD_NOTYET). Low priority for T20. |
| `sys/dev/drm2/drm_drv.c:355` | `drm_ioctl()` | `int drm_ioctl(struct cdev *kdev, u_long cmd, caddr_t data, int flags, DRM_STRUCTPROC *p)` | **Per-jail ioctl policy.** After `devfs_get_cdevpriv` returns `file_priv`, before the ioctl dispatch at line 384. This is where a future `prison_check_ioctl(cred, dev, cmd)` can short-circuit blacklisted ioctls (e.g. `DRM_IOCTL_SET_MASTER`, `DRM_IOCTL_ADD_CTX`). |
| `sys/dev/drm2/drm_fops.c:550` | `drm_poll()` | `int drm_poll(struct cdev *kdev, int events, struct thread *td)` | Per-jail poll. Already has `struct thread *td`. |
| `sys/dev/drm2/drm_fops.c:581` | `drm_mmap_single()` | `int drm_mmap_single(struct cdev *kdev, vm_ooffset_t *offset, vm_size_t size, struct vm_object **obj_res, int nprot)` | **Per-jail GEM/TTM mmap policy.** Dispatches to TTM (`ttm_bo_mmap_single`) if `dev->drm_ttm_bdev != NULL`, else to `drm_gem_mmap_single()`. Inject jail-scoped VRAM accounting here. |
| `sys/dev/drm2/drm_gem.c:456` | `drm_gem_mmap_single()` | `int drm_gem_mmap_single(struct drm_device *dev, vm_ooffset_t *offset, vm_size_t size, struct vm_object **obj_res, int nprot)` | Per-jail GEM mmap. The mmap's `cdev_pager_allocate` (line 477) ties the BO's `vm_object` to the jail. |
| `sys/dev/drm2/ttm/ttm_bo.c:1229` | `ttm_mem_global_alloc()` | `int ttm_mem_global_alloc(struct ttm_mem_global *glob, uint64_t size, bool no_wait, bool interruptible)` | **Host-wide VRAM accounting.** This is the *single* allocation choke point — every BO allocation goes through it. The right place to enforce a per-jail VRAM cap (decrement `glob->cur_alloc_pages` by the jail's quota on success; refuse if it would exceed). |
| `sys/dev/drm2/ttm/ttm_bo.c:1435` | `ttm_bo_init_mm()` | `int ttm_bo_init_mm(struct ttm_bo_device *bdev, unsigned type, unsigned long p_size)` | **Per-domain cap.** This is called once at driver attach with the size of the VRAM domain. Per-jail cap is enforced at the next hook; this sets the host ceiling. |

**Hook-point summary for T20 design**: there are 8 clear injection sites. The minimum viable surface for per-jail GPU governance is `drm_open` (allow/deny at attach) + `drm_ioctl` (master-only ioctl gating) + `ttm_mem_global_alloc` (VRAM cap).

### 2.2 What is not in the cdevsw

- `drm_close` / `drm_release` — not in cdevsw. Cleanup happens via `devfs_set_cdevpriv(priv, drm_release)` in `drm_fops.c:277`, which runs when the cdev is closed (`D_TRACKCLOSE`).
- `drm_mmap` (the legacy non-`single` variant) — `sys/dev/drm2/drm_vm.c:46`, behind `#ifdef FREEBSD_NOTYET`. Not active.

---

## 3. devfs rule API — how /dev/dri/* is gated per-jail

### 3.1 The API surface (the plan got this wrong)

The plan said "`devfs_rule_add` (e.g. `devfs_rule_add`)". There is no `devfs_rule_add()` kernel function. The devfs rule API in this tree is **ioctl-based**, not function-call-based:

**Header**: `sys/fs/devfs/devfs.h`
- `struct devfs_rule` (lines 66-100) — the kernel↔userland-shared rule descriptor
- Conditions: `DRC_DSWFLAGS` (match cdevsw flags), `DRC_PATHPTRN` (match a glob path)
- Actions: `DRA_BACTS` (`DRB_HIDE`/`DRB_UNHIDE`), `DRA_UID`, `DRA_GID`, `DRA_MODE`, `DRA_INCSET` (include another ruleset)
- Ioctls (lines 105-113): `DEVFSIO_RADD`, `DEVFSIO_RDEL`, `DEVFSIO_RAPPLY`, `DEVFSIO_RAPPLYID`, `DEVFSIO_RGETNEXT`, `DEVFSIO_SUSE`, `DEVFSIO_SAPPLY`, `DEVFSIO_SGETNEXT`
- No `_KERNEL`-side `devfs_rule_add()` exported function (lines 117-212).

**Implementation**: `sys/fs/devfs/devfs_rule.c`
- `devfs_rules_ioctl()` (line 160) is the dispatch entry that takes a `DEVFSIO_RADD`-populated `struct devfs_rule` and stores it into the ruleset.
- `devfs_rule_applyde_recursive()` (line 343) walks a `devfs_dirent` tree and applies the rule to each entry.
- `devfs_rule_run()` (line ~344, after the recursive function) runs the match-conditions + applies actions (hide/unhide/uid/gid/mode).

**Userspace consumer**: `sbin/devfs/rule.c:116-122` opens the devfs mount fd and writes rules via `ioctl(mpfd, DEVFSIO_RADD, &dr)`. This is the binary that `devfs.conf` / `devfs.rules` parse and feed in at boot.

### 3.2 The actual devfs API for per-jail use (T20/T21 design input)

For a per-jail view of `/dev/dri/*`, the natural FreeBSD mechanism is a **per-jail devfs mount with its own ruleset**:

1. Each jail's devfs mount is tagged with a `devfs_rsnum` (`struct devfs_mount::dm_ruleset` at `devfs.h:167`).
2. Rules are scoped per-ruleset: a jail with `ruleset=N` only sees rules tagged with `N` (`devfs_rule.c:265` — `error = devfs_ruleset_use(rsnum, dm)`).
3. The `sbin/devfs/rules` syntax (parsed by `sbin/devfs/rule.c`) supports `add path dri/.* mode 0660` style lines which translate into `DEVFSIO_RADD` ioctls at boot.

So **for T20/T21**: per-jail GPU governance does NOT need a new "gpu jail param" mechanism. It reuses `devfs.ruleset` + a `dri/*` path glob + mode/uid/gid actions. The framework's only new responsibility is **per-jail VRAM accounting** (the TTM choke point) — `/dev/dri/*` visibility is already solved by devfs.

### 3.3 Caveats for the devfs path-glob

- `devfs_rule.dr_pathptrn[DEVFS_MAXPTRNLEN]` (`devfs.h:80`) is a 200-byte char buffer. The current syntax is `fnmatch(3)`-style, not regex.
- `dri/card0`, `dri/renderD128` etc. are all created by the same `make_dev_p` call site in `drm_stub.c:379` — there is no per-driver customisation, so a single glob `dri/*` covers everything.
- A jail whose devfs ruleset is *empty* sees an empty `/dev/dri/`. This is the natural default-deny.

---

## 4. compat/nvidia + nvidia.ko — not in base, by design

The plan's expectation: "Check `compat/nvidia/` for nvidia.ko presence (likely absent in base; expected)."

| Path | Status | Notes |
|---|---|---|
| `compat/` (top-level) | **does not exist** | nvidia.ko is shipped as an out-of-tree tarball (NVIDIA-FreeBSD-x86_64-xxx.tar.xz) and unpacked into `compat/nvidia/` at the user's discretion |
| `compat/nvidia/nvidia.ko` | **absent** (the entire dir is absent) | Per the plan's prediction |
| `compat/nvidia/nvidia-modeset.ko` | **absent** | nvidia-modeset is the KMS shim — absent with nvidia.ko |
| `sys/contrib/dev/nvidia/` | present, but only 3 files | `LICENCE.nvidia` (licence text), `tegra124_xusb.bin.uu`, `tegra210_xusb.bin.uu` (Tegra SoC XUSB firmware for ARM — unrelated to x86 dGPU) |

**Verdict**: nvidia.ko and friends are NOT in this base. This is correct per design — the proprietary driver is shipped outside src.

**Implication for T20**: the gpu_resource framework cannot assume nvidia.ko is loaded. It must be loadable-only-if-loaded (i.e. `gr_nvidia_backend` is a stub returning ENOTSUP unless `nvidia_handle` is set in `linker_set_nvidia_modules` or similar). On a typical FreeBSD server with no nvidia.ko, the framework still has to allow the design to compile.

---

## 5. sys/modules/drm — does not exist, and that is OK

The plan said: "Check `sys/modules/drm/` for build wiring."

| Path | Status | Notes |
|---|---|---|
| `sys/modules/drm/` | **does not exist** | DRM2 is statically compiled into the kernel via `optional drm2` in `sys/conf/files:1577-…`. No module build directory needed. |
| `sys/modules/drm2/` | **does not exist** | Same reason. |
| `sys/modules/agp/` | **exists** | `KMOD=agp`, sources `agp.c agp_if.c` + per-arch glue (`agp_i810.c`, `agp_amd64.c`, `agp_nvidia.c`, `agp_ati.c` …). AGP is loadable; DRM2 is not. |

`sys/conf/options:574-575` shows the DRM options section:

```
# DRM options
DRM_DEBUG		opt_drm.h
```

(only one option — DRM_DEBUG; no DRM_ENABLE or DRM_KMS, indicating DRM2 is built when its `optional drm2` flag is on, which is the default for any kernel that includes `sys/conf/files` items 1577+).

**Verdict**: DRM2 is a non-loadable kernel option in this tree. A `sys/modules/drm{,2}/` would only exist if DRM2 were converted to a `KMOD` — which is NOT planned (T20/T21 design over a static option).

---

## 6. sys/kern/kern_cgroup.c — does not exist, no native cgroup v1/v2

The plan said: "Read `sys/kern/kern_cgroup.c` briefly to see the cgroup controller pattern (for future VRAM cgroup)."

| Path | Status | Notes |
|---|---|---|
| `sys/kern/kern_cgroup.c` | **does not exist** | No native cgroup controller framework in this base. The cgroup work in FreeBSD is still in the design/review stage (see freebsd-arch@ discussion, "cgroup v2" thread, mid-2024 → ongoing). |
| `sys/compat/linuxkpi/common/include/linux/cgroup.h` | exists (header only) | Linux-kAPI shim — translates Linux guest `cgroup_*` calls into `sysctl`/`mac_none` stubs. Not a usable host-side framework. |
| `sys/kern/subr_racct.c` | exists | `racct` is the **closest analog** to cgroup resource accounting on FreeBSD — per-jail CPU/IO/memory quotas (used by `kern_jail.c` for `allow.set_hostname`, `prison_racct_add()` etc.). T20/T21 should follow the `racct` pattern, not invent a cgroup controller. |

**Verdict**: there is no cgroup framework to mimic. T20's "VRAM cgroup" should be implemented as a per-prison accounting table hung off `struct prison` (mirroring `struct racct` for memory), or as a separate `struct prison_gpu_quota` field on `struct prison` itself.

---

## 7. Existing resource mediation patterns (the `prison_check` family)

To inform T20's `prison_check_gpu` design, the existing patterns in `sys/kern/kern_jail.c` / `sys/sys/jail.h`:

| Function | Signature | Returns | Purpose |
|---|---|---|---|
| `prison_check()` (kern_jail.c:4058) | `int prison_check(struct ucred *cred1, struct ucred *cred2)` | 0 / `ESRCH` | Are cred1 and cred2 in the same jail (or is cred1 an ancestor jail of cred2)? |
| `prison_check_nfsd()` (kern_jail.c:4076) | `bool prison_check_nfsd(struct ucred *cred)` | bool | Does this cred have `PR_ALLOW_NFSD` + vnet? |
| `prison_check_ip4()` (jail.h:502) | `int prison_check_ip4(const struct ucred *, const struct in_addr *)` | 0 / errno | IP-allowlist check (vnet/firewall style) |
| `prison_check_ip4_locked()` | (locked variant) | | |
| `prison_check_ip6()` (jail.h:513) | `int prison_check_ip6(const struct ucred *, const struct in6_addr *)` | 0 / errno | IPv6 variant |
| `prison_check_ip6_locked()` | (locked variant) | | |
| `prison_check_af()` (kern_jail.c:3953) | `int prison_check_af(struct ucred *cred, int af)` | 0 / errno | Address-family allowlist |
| `prison_priv_check()` (jail.h:522) | `int prison_priv_check(struct ucred *cred, int priv)` | 0 / EPERM | Privilege check (replaces `suser`) |

**Pattern for T20's `prison_check_gpu`**:
```c
int prison_check_gpu(struct ucred *cred, struct drm_device *dev,
    enum gpu_op op);   /* GPU_OP_OPEN / GPU_OP_AUTH / GPU_OP_MMAP / GPU_OP_IOCTL */
```
- `cred` is `curthread->td_ucred` (always available at the hook site).
- `dev` is the `struct drm_device *` (always available from `cdev->si_drv1`).
- `op` selects which jail param to consult: `allow.gpu.open`, `allow.gpu.auth`, `allow.gpu.mmap`, `allow.gpu.ioctl`, plus a per-jail `gpu.vram_cap` and `gpu.time_slice`.
- Returns 0 on success; `ENXIO` / `EPERM` / `ENOSPC` (VRAM cap) / `EBUSY` (auth lock-out) on failure.

Jail-param registration: `prison_add_allow("gpu", "open", ...)` (kern_jail.c:5253) registers the `allow.gpu.open` boolean sysctl + jail param + `PR_ALLOW_GPU_OPEN` bit.

The T20 framework should:
1. Add a new `PR_ALLOW_GPU` family (4 bits: `OPEN`, `AUTH`, `MMAP`, `IOCTL`).
2. Add 2 numeric jail params: `gpu.vram_cap` (bytes), `gpu.time_slice_us` (microseconds).
3. Reuse the existing `prison_check_*` return-code convention.

---

## 8. Existing /dev/dri/* consumers (sanity check)

```sh
$ grep -rln "/dev/dri\|dri/" sys/ 2>/dev/null
sys/dev/drm2/drm_stub.c
sys/compat/linsysfs/linsysfs.c
sys/compat/linux/linux_util.c
```

- `sys/compat/linsysfs/linsysfs.c` — Linux sysfs compatibility, exposes `/sys/devices/.../drm/...` for Linux guests.
- `sys/compat/linux/linux_util.c` — Linux util shim, may translate Linux `/dev/dri/renderD128` to the FreeBSD node.
- The DRM2 framework itself (`drm_stub.c`) — the only producer.

No bhyve, jail, or VMM code path currently touches `/dev/dri/*`. This is good — the surface is clean for T20.

---

## 9. Verdict (the requested one-liner)

**"DRI is the right abstraction; DRM2 hooks available at open/mmap/ioctl, but the actual KMS drivers (i915kms, amdgpu, radeonkms, nvidia.ko) are NOT in this base tree."**

Sub-findings that must be passed downstream:
- **Path corrections**: T20 must be edited to say `sys/dev/drm2/` (not `sys/dev/drm/`), `compat/nvidia/` does not exist in src (nvidia.ko is an out-of-tree tarball), `sys/kern/kern_cgroup.c` does not exist (use `racct` as the model).
- **Hook-point map**: §2.1 table — 8 injection sites, of which the minimum viable set is `drm_open` + `drm_ioctl` + `ttm_mem_global_alloc`.
- **devfs already covers the path gating**: T20/T21 do NOT need a new "gpu jail param" for visibility — `devfs.ruleset` + a `dri/*` glob is enough.
- **VRAM cap is the new thing**: the actual missing piece is per-jail VRAM accounting at the TTM choke point. This is what `gpu_resource` adds.
- **No real hardware to test on**: T20's design is therefore un-testable in this base. T18 (smoke test) and T46 (broker e2e) can only validate the *path* through the framework, not its correctness on a real GPU. The framework must have a `gpu_backend_stub` that returns synthesised numbers so ATF cases can exercise the `prison_check_gpu` decision tree without a GPU.
- **Plan-commit signal**: T19 is a design/audit task, not a code change. The plan's "Commit: NO" is correct — but the user requested a commit for the audit+evidence files. We commit **only** the audit and evidence files, not the source tree (which is unchanged).

---

## 10. Recommended plan corrections (for F1 / planner)

These should be applied to T19, T20, T21, T22 acceptance criteria before they are worked:

1. **T19 references**: change every `sys/dev/drm/` → `sys/dev/drm2/`. Drop the `sys/kern/kern_cgroup.c` read (replace with `sys/kern/subr_racct.c` + `kern_jail.c prison_check_*`).
2. **T20 references**: change `sys/dev/drm/` → `sys/dev/drm2/`; change `sys/fs/devfs/` to add the rule-creation constraint (ioctl API, not function call); add the `ttm_mem_global_alloc` hook point to the vtable.
3. **T21 references**: must include the requirement that `gpu_backend_stub` exists (so the framework is testable without a real GPU).
4. **T19 acceptance criterion**: drop "Verdict line" being the only one — add "Path corrections list present" (§0 of this audit) so the planner sees the discrepancies.
5. **T22 preflight framework**: no change needed (it doesn't touch DRM). But T23's 11 shipped checks should not assume any GPU backend is present; they should all be no-ops when `gpu_backend_stub` is in use.

---

## 11. Source code locations — quick reference (file:line)

```
DRM2 build:
  sys/conf/files:1577+                 (every drm2/*.c listed as "optional drm2")
  sys/conf/options:574-575             (DRM_DEBUG option only)

DRM cdevsw + dev nodes:
  sys/dev/drm2/drm_stub.c:73-82        (static struct cdevsw drm_cdevsw)
  sys/dev/drm2/drm_stub.c:368-376      (minor_devname "dri/card%d" / "dri/renderD%d")
  sys/dev/drm2/drm_stub.c:379-381      (make_dev_p with DRM_DEV_UID/GID/MODE)
  sys/dev/drm2/drmP.h:1594-1596        (DRM_DEV_UID=root, GID=video, MODE=0660)

Hook points:
  sys/dev/drm2/drm_fops.c:120          (int drm_open(struct cdev *, int, int, DRM_STRUCTPROC *))
  sys/dev/drm2/drm_fops.c:163          (EXPORT_SYMBOL(drm_open))
  sys/dev/drm2/drm_fops.c:176          (static int drm_open_helper(...))
  sys/dev/drm2/drm_fops.c:277          (devfs_set_cdevpriv(priv, drm_release))
  sys/dev/drm2/drm_fops.c:491          (int drm_read(struct cdev *, struct uio *, int))
  sys/dev/drm2/drm_fops.c:550          (int drm_poll(struct cdev *, int, struct thread *))
  sys/dev/drm2/drm_fops.c:581-599      (drm_mmap_single dispatch: TTM or GEM)
  sys/dev/drm2/drm_drv.c:355           (int drm_ioctl(struct cdev *, u_long, caddr_t, int, DRM_STRUCTPROC *))
  sys/dev/drm2/drm_drv.c:495           (EXPORT_SYMBOL(drm_ioctl))
  sys/dev/drm2/drm_gem.c:456-487       (drm_gem_mmap_single + cdev_pager_allocate)
  sys/dev/drm2/ttm/ttm_bo.c:1229-1248  (ttm_mem_global_alloc / free — host-wide VRAM accounting)
  sys/dev/drm2/ttm/ttm_bo.c:1435       (ttm_bo_init_mm — per-domain cap at attach)
  sys/dev/drm2/ttm/ttm_memory.c:38-124 (struct ttm_mem_global + helpers)
  sys/dev/drm2/drm_agpsupport.c:97,112,140,148 (AGP acquire/release ioctls)

devfs:
  sys/fs/devfs/devfs.h:50-100          (devfs_rnum, devfs_rsnum, struct devfs_rule)
  sys/fs/devfs/devfs.h:105-113         (DEVFSIO_RADD/DEL/APPLY/..., DEVFSIO_SUSE/...)
  sys/fs/devfs/devfs.h:174, 190-191    (extern devfs_rule_depth, devfs_ruleset_*)
  sys/fs/devfs/devfs_rule.c:160        (devfs_rules_ioctl dispatch)
  sys/fs/devfs/devfs_rule.c:343-350    (devfs_rule_applyde_recursive + devfs_rule_run)
  sbin/devfs/rule.c:116-122            (userspace rule consumer via ioctl)

Resource mediation patterns:
  sys/kern/kern_jail.c:3953            (prison_check_af)
  sys/kern/kern_jail.c:4058-4062       (prison_check — same-jail / ancestor)
  sys/kern/kern_jail.c:4076-4085       (prison_check_nfsd)
  sys/kern/kern_jail.c:5253-5310       (prison_add_allow — register allow.X.Y params)
  sys/sys/jail.h:468, 502-522          (prison_check* declarations)
  sys/kern/subr_racct.c                (per-jail resource accounting — model for VRAM cap)

nvidia:
  sys/contrib/dev/nvidia/LICENCE.nvidia
  sys/contrib/dev/nvidia/tegra124_xusb.bin.uu   (ARM Tegra firmware)
  sys/contrib/dev/nvidia/tegra210_xusb.bin.uu   (ARM Tegra firmware)
  compat/                                (does not exist — nvidia.ko lives in external tarball)
```

---

## 12. Output paths (per task)

| Artefact | Path | Purpose |
|---|---|---|
| This audit | `.sisyphus/drafts/t19-gpu-audit.md` | The human-readable report (this file) |
| Evidence (commands + outputs) | `.sisyphus/evidence/task-19-gpu-audit.txt` | Machine-checkable evidence the audit was run |
| Notepad (learnings/issues/decisions) | `.sisyphus/notepads/freebsd-display-abstraction/{learnings,issues,decisions}.md` | Appended, not overwritten |
| Checkpoint | `.sisyphus/state/task-19.checkpoint.json` | Updated for T20 to pick up |
| Commit | (per request) | `audit(t19): GPU kernel subsystem recon` — drafts + evidence + notepad only, no source |

— Task T19 audit complete. Unblocks T20 (gpu_resource design) and T21 (gpu jail params + module).
