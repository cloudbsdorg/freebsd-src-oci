# T3 — FreeBSD Jail Parameter API Audit (recipe for `allow.fbuf` + `gpu.*`)

**Task**: T3 from `.sisyphus/plans/freebsd-display-abstraction.md` (Wave 1, recon).
**Branch**: `framebuffer` (host worktree: `pppoe1.cloudbsd.org:~/git/freebsd-src-oci-fb-build`, HEAD `bc76be73b4a`).
**Date**: 2026-06-08.
**Honcho session**: `displayd-reconcile-wave1-t0-t6`.
**Read-only**: no source code modified; no new params added. This is recipe documentation only.
**Predecessor**: T0 (PARTIAL PASS — see `.sisyphus/drafts/t0-env-validation.md`).
**Verdict**: **The FreeBSD jail API supports the displayd params in three ways: (1) dynamic `allow.fbuf` flag via `prison_add_allow()` — same precedent as `allow.vmm` / `allow.vmm_ppt`. (2) Typed string params (`fbuf.width`, `fbuf.height`, `fbuf.transport`) via `SYSCTL_JAIL_PARAM_STRING` + an OSD slot. (3) Numeric typed params (`gpu.cores`, `gpu.memory`) via `SYSCTL_JAIL_PARAM` + an OSD slot. The `fbuf.nokbd` / `fbuf.nomouse` sub-flags are NOT a standard `allow.X` shape — they need either a custom `prison_add_allow`-style API extension or OSD-defined booleans. See Recipe 1.5 for the deviation.**

---

## 1. Scope

The plan's T3 has three recipes:
1. `allow.fbuf` boolean + `fbuf.nokbd` / `fbuf.nomouse` boolean sub-flags
2. Typed params `fbuf.width`, `fbuf.height`, `fbuf.transport`
3. `allow.gpu` boolean + `gpu.cores` / `gpu.memory` / `gpu.scheduler`

This document gives step-by-step recipes, the files each touches (with line ranges), and a verification command for each.

## 2. Files to read before working

| File | Lines | Why |
|---|---:|---|
| `sys/sys/jail.h` | 535 | Public `struct prison`, `PR_ALLOW_*` defines, `prison_add_allow()` declaration, `SYSCTL_JAIL_PARAM_*` macros |
| `sys/kern/kern_jail.c` | 5713 | `pr_flag_allow[]` table, `prison_set_allow()` / `prison_set_allow_locked()`, `prison_add_allow()` impl, kparams dispatch |
| `sys/kern/kern_jailmeta.c` | 617 | OSD lifecycle (`osd_jail_register`, `PR_METHOD_*` dispatch, `jm_osd_method_set_meta` etc.) |
| `sys/dev/vmm/vmm_dev.c:1209-1219` | — | **PRECEDENT for dynamic `allow.X` flag** — `vmmdev_init()` calls `prison_add_allow(NULL, "vmm", ...)` and `prison_add_allow(NULL, "vmm_ppt", ...)` |
| `sys/dev/vmm/vmm_dev.c:96, 120, 466` | — | **PRECEDENT for flag check** — `(ucred->cr_prison->pr_allow & pr_allow_vmm_flag) == 0` |
| `lib/libjail/jail.c:1199-1247` | — | `kldload_param()` and `noname()` — how `allow.X` / `allow.noX` auto-flip works |
| `lib/libjail/jail.c:42-50` (`lib/libjail/jail.h`) | — | `struct jailparam { jp_name, jp_value, jp_valuelen, jp_elemlen, jp_ctltype, jp_structtype, jp_flags }` |
| `lib/libjail/jail.c:57-65` | — | `jailparam_init / import / import_raw / set / get / export / free / all` |
| `usr.sbin/jail/jail.conf.5` | 273 | Doc style (BSD-2-Clause mdoc; `.It Li` for params; sample `allow.mount` / `allow.nomount` lines) |

## 3. Key API surface

### 3.1 `prison_add_allow()` — dynamic `allow.X` flag

**Signature** (`sys/sys/jail.h:524`):
```c
unsigned prison_add_allow(const char *prefix, const char *name,
    const char *prefix_descr, const char *descr);
```

**Implementation** (`sys/kern/kern_jail.c:5253`):
- Allocates a free bit in `pr_allow_all` (the mask of dynamically-added allow bits).
- Allocates the next free slot in `pr_flag_allow[NBBY * NBPW]` (line 226 — the static allow table).
- Returns the flag bit; if no free bit, returns 0.
- Idempotent: re-adding the same `(prefix, name)` returns the existing flag (line 5283 `goto no_add`).

**Precedent** (`sys/dev/vmm/vmm_dev.c:1213-1217`):
```c
static unsigned int pr_allow_vmm_flag, pr_allow_vmm_ppt_flag;

static int
vmmdev_init(void)
{
    int error;
    sx_xlock(&vmmdev_mtx);
    error = make_dev_p(MAKEDEV_CHECKNAME, &vmmctl_cdev, &vmmctlsw, NULL,
        UID_ROOT, GID_VMM, 0660, "vmmctl");
    if (error == 0) {
        pr_allow_vmm_flag = prison_add_allow(NULL, "vmm", NULL,
            "Allow use of vmm in a jail");
        pr_allow_vmm_ppt_flag = prison_add_allow(NULL, "vmm_ppt", NULL,
            "Allow use of vmm with ppt devices in a jail");
    }
    sx_xunlock(&vmmdev_mtx);
    return (error);
}
```

**Check site** (`sys/dev/vmm/vmm_dev.c:120`):
```c
if (pr_allow_vmm_flag != 0 &&
    (ucred->cr_prison->pr_allow & pr_allow_vmm_flag) == 0)
    return (EACCES);
```

The `pr_allow_vmm_flag != 0` guard is the **key gotcha**: if `prison_add_allow()` returned 0 (no free bits, or already-registered-with-different-name), don't trust the bit.

### 3.2 `pr_flag_allow[]` — static `allow.X` table

(`sys/kern/kern_jail.c:226-251`) — the static table of all `allow.X` / `allow.noX` pairs known at compile time. Each entry is `{positive_name, negative_name, flag_bit}`. The static flags use bits from the `PR_ALLOW_*` defines in `sys/sys/jail.h:256-274`. **This table is full-size so dynamic params can be added** (line 223 comment) — meaning the dynamic allocation finds a zero-`flag` slot in the same array.

**`PR_ALLOW_ALL_STATIC` (sys/sys/jail.h:283)**: the bitmask of all statically-allocated allow flags. New static flags added here must be appended to the bitmask, and the corresponding entry added to `pr_flag_allow[]`.

**`PR_ALLOW_DIFFERENCES` (sys/sys/jail.h:299)**: the set of flags that children can differ from their parent. By default all flags are inherited identical; only `UNPRIV_DEBUG` and `UNPRIV_PARENT_TAMPER` can be relaxed. For `allow.fbuf`, we should decide: should a child jail be able to relax it? Probably NOT — a child with `allow.fbuf;` should not be able to give a grandchild less-restrictive access. Leave it out of `PR_ALLOW_DIFFERENCES`.

### 3.3 `struct jailparam` — the iovec-based userspace API

(`lib/libjail/jail.h:42-50`):
```c
struct jailparam {
    char    *jp_name;
    void    *jp_value;
    size_t   jp_valuelen;
    size_t   jp_elemlen;
    int      jp_ctltype;
    int      jp_structtype;
    unsigned jp_flags;
};
```

The `jail(8)` command and the libjail `jail_set()` / `jail_get()` calls pass an array of `jailparam` (via iovec) to the kernel. The kernel matches `jp_name` against the registered params (static via the `kparams` table; dynamic via OSD `PR_METHOD_SET` / `PR_METHOD_GET`).

**`jailparam_init` (lib/libjail/jail.c)**: looks up the param name in the kernel's registered list via a sysctl roundtrip, returns 0 on success, ENOENT if the param doesn't exist. **This is why `kldload_param()` exists** — it kldloads the module that registers the param if the param is in a known-missing list (`linux`, `sysvmsg`, `allow.mount.<fs>`).

### 3.4 `SYSCTL_JAIL_PARAM_*` — the typed-param registration macros

(`sys/sys/jail.h:417-442`):
- `SYSCTL_JAIL_PARAM_DECL(name)` — declares a sysctl sub-tree `_security_jail_param_<name>`.
- `SYSCTL_JAIL_PARAM(module, param, type, fmt, descr)` — registers a typed param.
- `SYSCTL_JAIL_PARAM_STRING(module, param, access, len, descr)` — registers a string param (`CTLTYPE_STRING`).
- `SYSCTL_JAIL_PARAM_STRUCT(module, param, access, len, fmt, descr)` — registers a binary struct param (`CTLTYPE_STRUCT`).
- `SYSCTL_JAIL_PARAM_NODE(module, descr)` — registers a sub-node (for grouping).
- `SYSCTL_JAIL_PARAM_SUBNODE(parent, module, descr)` — registers a sub-node under a parent.

The `sysctl_jail_param` handler (in `sys/kern/kern_jail.c`) dispatches the sysctl to the OSD jail method (`PR_METHOD_SET` for writes, `PR_METHOD_GET` for reads), which in turn dispatches to the per-OSD-slot handler registered by the module.

### 3.5 OSD jail — the dispatch machinery for typed params

(`sys/kern/kern_jailmeta.c`):
- `osd_jail_register(destructor, methods[PR_MAXMETHOD])` — allocates a slot in the per-jail OSD array.
- `osd_jail_call(pr, method, arg)` — iterates all registered OSDs, calling the method that the OSD provided for that method index.
- Methods: `PR_METHOD_CREATE`, `PR_METHOD_SET`, `PR_METHOD_GET`, `PR_METHOD_CHECK`, `PR_METHOD_ATTACH`, `PR_METHOD_REMOVE`, `PR_METHOD_PRISON_REMOVED`, `PR_METHOD_MAX`.

The `meta` OSD (`kern_jailmeta.c:605`) is the canonical typed-param OSD — every typed jail param (e.g., `ip4.addr`, `host.hostname`, `name`) goes through it. To add a new typed param, you can either (a) add to the meta OSD's hardcoded table, or (b) create a new OSD for the new subsystem.

**For displayd, the cleaner path is (b)** — a new OSD for `fbuf.*` and `gpu.*` params, registered in the `displayd.ko` module's load handler.

## 4. Recipe 1 — `allow.fbuf` boolean + sub-flags

### 4.1 Goal

Add a new allow-flag:
- `allow.fbuf` (boolean, default off) — enables the jail to access the `displayd` framebuffer + kbd + mouse devices.
- `fbuf.nokbd` (boolean, default off) — opt-out for the keyboard (`displayd` adds kbd by default when fbuf is on; this disables it).
- `fbuf.nomouse` (boolean, default off) — same for mouse.

### 4.2 Standard-pattern limitation

The standard `prison_add_allow()` only registers a single `(prefix, name)` pair, generating `allow.X` / `allow.noX`. The plan's sub-flag names `fbuf.nokbd` / `fbuf.nomouse` are **not** the standard `allow.fbuf.nokbd` / `allow.fbuf.nomouse` — they're sibling names at the fbuf prefix level. The standard `noname()` helper (lib/libjail/jail.c:1233) does convert `fbuf.kbd` ↔ `fbuf.nokbd` correctly (it just inserts `no` after the last `.`), but the **kernel side** would need to register the `fbuf.kbd` / `fbuf.nokbd` boolean pair as a `bool_flags` entry somewhere — and `pr_flag_allow[]` is reserved for `allow.X` / `allow.noX`.

### 4.3 Two implementation options

**Option A — three separate `allow.X` flags** (simplest, deviates from the plan's spelling):
- `allow.fbuf` — main capability.
- `allow.fbuf.nokbd` — opt-out.
- `allow.fbuf.nomouse` — opt-out.

This is the cleanest FreeBSD-native approach. The user writes `allow.fbuf; fbuf.nokbd;` and `jail(8)` flips `fbuf.nokbd` to `allow.fbuf.nokbd` for the kernel side. **Cons**: the public name is `allow.fbuf.nokbd`, not `fbuf.nokbd`. **Pros**: zero new kernel infrastructure; reuses `prison_add_allow()` and `pr_flag_allow[]` directly.

**Option B — new OSD with a "sub-param" table** (matches the plan's spelling exactly):
- Register a new OSD slot for `displayd` via `osd_jail_register()`.
- The OSD provides `PR_METHOD_SET` / `PR_METHOD_GET` handlers that dispatch on the param name (`fbuf.nokbd`, `fbuf.nomouse`, `fbuf.width`, etc.) — all share one sub-namespace.
- Add `SYSCTL_JAIL_PARAM_NODE(displayd, "displayd parameters")` to group them under `security.jail.param.displayd.<param>`.
- The `fbuf.nokbd` / `fbuf.nomouse` are stored as OSD-attached data on the `struct prison`, not as `pr_allow` bits.

**Cons of B**: requires a new OSD slot (uses one of the OSD's per-jail allocations), and the `fbuf.nokbd` is a boolean stored outside `pr_allow` so the per-jail `pr_allow` check pattern (`cred->cr_prison->pr_allow & flag`) doesn't apply. The check site would be `osd_jail_get(cred->cr_prison, displayd_osd, "fbuf.nokbd", ...)`.

**Recommendation: Option A.** Simpler, reuses existing infrastructure, and the `allow.fbuf.nokbd` name is fine for users (jail.conf syntax). If the spelling is a hard requirement, the plan needs to be updated.

### 4.4 Option A — step-by-step

**File: `sys/modules/displayd/Makefile` (new)**
- Add `KMOD=displayd`, `SRCS=displayd.c`, `.include <bsd.kmod.mk>`.

**File: `sys/modules/displayd/displayd.c` (new)**
```c
#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/jail.h>
#include <sys/proc.h>
#include <sys/ucred.h>

static unsigned int pr_allow_fbuf_flag;
static unsigned int pr_allow_fbuf_nokbd_flag;
static unsigned int pr_allow_fbuf_nomouse_flag;

static int
displayd_modevent(module_t mod, int type, void *data)
{
    int error = 0;
    switch (type) {
    case MOD_LOAD:
        pr_allow_fbuf_flag = prison_add_allow(NULL, "fbuf", NULL,
            "Allow access to displayd framebuffer + kbd + mouse");
        pr_allow_fbuf_nokbd_flag = prison_add_allow("fbuf", "nokbd", NULL,
            "Disable the virtual keyboard that displayd adds with fbuf");
        pr_allow_fbuf_nomouse_flag = prison_add_allow("fbuf", "nomouse", NULL,
            "Disable the virtual mouse that displayd adds with fbuf");
        break;
    case MOD_UNLOAD:
        /* prison_add_allow() does not support removal in this rev. */
        break;
    }
    return (error);
}

static moduledata_t displayd_mod = {
    "displayd",
    displayd_modevent,
    NULL
};

DECLARE_MODULE(displayd, displayd_mod, SI_SUB_DRIVERS, SI_ORDER_MID);
```

**File: `sys/conf/files`**
- Add `modules/displayd/displayd.c optional displayd` (or via `Makefile.inc1` if preferred).

**File: `share/man/man5/jail.conf.5`** (or wherever the plan's main param doc lives — see §2.7)
- Add an `allow.fbuf` entry under the "ALLOW FLAGS" section, following the `allow.vmm` style:
  ```
  .It Li Va allow.fbuf Ta "boolean" Ta "default: false"
  Allow the jail to access the displayd framebuffer + kbd + mouse devices.
  When enabled, the jail receives /dev/fb0, /dev/kbd0, and /dev/ums0 (additive).
  Use allow.fbuf.nokbd and allow.fbuf.nomouse to opt out of specific input devices.
  ```

**Verification (build, doesn't need kldload)**:
```sh
cd /usr/src/sys/modules/displayd && make
# Expected: clean build with no warnings (Werror is on)
# If you don't have a /usr/src at the same rev, build from the worktree:
# cd ~/git/freebsd-src-oci-fb-build/sys/modules/displayd && make
```

**Verification (kldload, needs root — see T0.D follow-up)**:
```sh
sudo kldload ./displayd.ko
jail -c name=t3fbuf allow.fbuf path=/ persist
jls -v t3fbuf | grep fbuf   # should show allow.fbuf
jail -r t3fbuf
sudo kldunload displayd.ko
```

**Files touched** (Option A):
- `sys/modules/displayd/Makefile` (new, ~5 lines)
- `sys/modules/displayd/displayd.c` (new, ~40 lines)
- `sys/conf/files` (1 line)
- `usr.sbin/jail/jail.conf.5` (~10 lines added)

### 4.5 Check site (consumer side)

`sys/dev/displayd/displayd.c` (or wherever the cdev open is):
```c
static int
displayd_open(struct cdev *dev, int flags, int fmt, struct thread *td)
{
    struct ucred *cred = td->td_ucred;
    unsigned int want = 0;

    /* Map the cdev's minor to which device is being opened. */
    switch (minor(dev)) {
    case FB_MINOR:    /* /dev/fb0 */
    case KBD_MINOR:   /* /dev/kbd0 */
        want = pr_allow_fbuf_flag;
        break;
    case MOUSE_MINOR: /* /dev/ums0 */
        want = pr_allow_fbuf_flag;
        break;
    }

    if (want != 0 &&
        (cred->cr_prison->pr_allow & want) == 0)
        return (EACCES);

    return (0);
}
```

The `fbuf.nokbd` / `fbuf.nomouse` opt-outs are checked at **devfs-rule generation time** (T6 owns this), not at open time. See §6.2 for the devfs rule mapping.

## 5. Recipe 2 — `fbuf.width`, `fbuf.height`, `fbuf.transport` (string params)

### 5.1 Goal

Add three typed string/integer params under the `fbuf` prefix:
- `fbuf.width` (int, default 1920) — framebuffer width in pixels.
- `fbuf.height` (int, default 1080) — framebuffer height in pixels.
- `fbuf.transport` (string, default "rfb") — transport name (`"rfb"`, `"rdp"`, etc.).

### 5.2 Step-by-step

**File: `sys/modules/displayd/displayd_params.c` (new, ~80 lines)**
```c
#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/jail.h>
#include <sys/osd.h>
#include <sys/sx.h>

SYSCTL_JAIL_PARAM_DECL(displayd);
SYSCTL_JAIL_PARAM_STRING(displayd, fbuf_transport,
    CTLFLAG_RW, 16, "displayd transport (rfb, rdp, ...)");
SYSCTL_JAIL_PARAM(displayd, fbuf_width,
    CTLTYPE_INT | CTLFLAG_RW, "I", "displayd framebuffer width");
SYSCTL_JAIL_PARAM(displayd, fbuf_height,
    CTLTYPE_INT | CTLFLAG_RW, "I", "displayd framebuffer height");

static struct {
    int width;
    int height;
    char transport[16];
} displayd_defaults = { 1920, 1080, "rfb" };

static int
displayd_jm_set(struct ucred *cred, struct prison *pr, struct jailparam *jp)
{
    struct displayd_osd *o;
    /* ... per-param dispatch: fbuf.width / fbuf.height / fbuf.transport ... */
    return (0);
}

static int
displayd_jm_get(struct ucred *cred, struct prison *pr, struct jailparam *jp)
{
    /* ... read back from the OSD-allocated per-prison struct ... */
    return (0);
}

static osd_method_t displayd_methods[PR_MAXMETHOD] = {
    [PR_METHOD_SET] = (osd_method_t)displayd_jm_set,
    [PR_METHOD_GET] = (osd_method_t)displayd_jm_get,
};

static void
displayd_jm_destructor(void *data)
{
    free(data, M_DISPLAYD);
}

static int displayd_osd;

static int
displayd_modevent(module_t mod, int type, void *data)
{
    int error = 0;
    switch (type) {
    case MOD_LOAD:
        displayd_osd = osd_jail_register(displayd_jm_destructor,
            displayd_methods);
        if (displayd_osd < 0) {
            error = displayd_osd;
            break;
        }
        /* Register the jail params under security.jail.param.displayd. */
        SYSCTL_JAIL_PARAM_NODE(displayd, "displayd parameters");
        /* (The macros above register the params on MOD_LOAD.) */
        break;
    case MOD_UNLOAD:
        /* osd_jail_unregister is not in the public API; safe to leak on
           MOD_UNLOAD in a development build. */
        break;
    }
    return (error);
}

static moduledata_t displayd_mod = { "displayd", displayd_modevent, NULL };
DECLARE_MODULE(displayd, displayd_mod, SI_SUB_DRIVERS, SI_ORDER_MID);
```

**File: `sys/modules/displayd/Makefile`**
- Add `SRCS+= displayd_params.c` (in addition to the main `displayd.c`).

**Verification**:
```sh
make                    # clean build
sudo kldload ./displayd.ko
sysctl -a | grep jail.param.displayd   # should show 3 params
jail -c name=t3fb2 allow.fbuf fbuf.width=1280 fbuf.height=720 fbuf.transport=rdp persist
jls -v t3fb2 | grep fbuf                # should show the 3 params
jail -r t3fb2
sudo kldunload displayd.ko
```

**Files touched**:
- `sys/modules/displayd/Makefile` (1 line)
- `sys/modules/displayd/displayd_params.c` (new, ~80 lines)
- `usr.sbin/jail/jail.conf.5` (~30 lines added)

## 6. Recipe 3 — `allow.gpu` + `gpu.cores` / `gpu.memory` / `gpu.scheduler`

### 6.1 Goal

- `allow.gpu` (boolean, default off) — enables jail access to `/dev/dri/*` (the drm render nodes).
- `gpu.cores` (int, default 50) — percentage of GPU compute cores.
- `gpu.memory` (int, default 256) — MB of VRAM.
- `gpu.scheduler` (string, default "auto") — scheduler policy (`"auto"`, `"wfq"`, `"fifo"`).

### 6.2 Step-by-step

**File: `sys/modules/gpu_resource/gpu_resource.c` (new — per T19, design-only in this base)**

For `allow.gpu`:
```c
static unsigned int pr_allow_gpu_flag;
pr_allow_gpu_flag = prison_add_allow(NULL, "gpu", NULL,
    "Allow access to GPU devices (DRM render nodes)");
```

For the typed params, follow Recipe 2's pattern with a separate OSD slot for gpu.* params. Store the values in the per-jail OSD struct.

**Check site (the per-jail GPU governance)**:
```c
/* In drm_open() at sys/dev/drm2/drm_fops.c:120 */
int error = prison_check_gpu(cred, dev, GPU_OP_OPEN);
if (error != 0)
    return (error);
```

Where `prison_check_gpu()` is a new function (T19 recommended it) that:
1. Checks `cred->cr_prison->pr_allow & pr_allow_gpu_flag` — refuse if not set.
2. Looks up the gpu_resource OSD for the prison; if absent, refuse.
3. If present, check `gpu.cores` / `gpu.memory` against the requested operation.
4. If the operation is `GPU_OP_MMAP` (a GEM/TTM mmap), call `ttm_mem_global_alloc()` with the prison's `gpu.memory` cap subtracted from the current usage.

### 6.3 `gpu.scheduler` accepting a percentage string

`gpu.scheduler` is a string (`"auto"`, `"wfq"`, `"fifo"`). The same `SYSCTL_JAIL_PARAM_STRING` macro handles it. Inside `displayd_jm_set`, parse the string and reject unknown values with `EINVAL`. No need for a custom type.

(If the plan ever wants `gpu.cores` to be a percentage like `"50%"` instead of just `50`, that's a string param with custom parsing — same `SYSCTL_JAIL_PARAM_STRING` + a small parser.)

**Files touched**:
- `sys/modules/gpu_resource/Makefile` (new)
- `sys/modules/gpu_resource/gpu_resource.c` (new, ~200 lines)
- `sys/kern/kern_jail.c` (~30 lines added — the `prison_check_gpu` function; OR move it to `gpu_resource.c` if the OSD is a loadable module)
- `sys/dev/drm2/drm_fops.c` (~6 lines — add the `prison_check_gpu` call in `drm_open`)
- `usr.sbin/jail/jail.conf.5` (~40 lines added)

## 7. Verification — build the three modules

```sh
# In the worktree (or /usr/src if the worktree is missing the kernel dirs).
cd ~/git/freebsd-src-oci-fb-build
# (a) Displayd module
cd sys/modules/displayd && make
# Expected: cc -O2 -pipe -Werror -D_KERNEL -DKLD_MODULE ... ; ld ... ; objcopy ...
# Exit 0, produces displayd.ko
# (b) Gpu_resource module
cd ../gpu_resource && make
# Expected: same build chain, produces gpu_resource.ko
# (c) Verify the existing kernel still builds
cd /usr/src && make -j$(sysctl -n hw.ncpu) buildkernel KERNCONF=GENERIC
# Expected: existing kernel builds clean (the drm_fops.c change is a no-op
# for kernels that don't include gpu_resource.ko).
```

## 8. Backward-compatibility check

- **No existing `allow.X` flag is changed.** All three recipes add NEW flags.
- **No existing param is renamed.** The new `fbuf.*` and `gpu.*` params live under new `security.jail.param.displayd.*` and `security.jail.param.gpu_resource.*` sub-trees.
- **Existing `pr_allow` and `pr_flag_allow[]` semantics are preserved.** `prison_add_allow()` only ever appends, never modifies.
- **`PR_ALLOW_DIFFERENCES` is unchanged.** `allow.fbuf` / `allow.gpu` are NOT in the differences set; children inherit the parent's value.

## 9. Caveats and follow-up tasks

1. **`prison_add_allow()` has no removal path.** Once `MOD_LOAD` registers the flag, `MOD_UNLOAD` cannot free the bit. A module that may be unloaded should keep its flag in a static bit; this is a known limitation.
2. **`osd_jail_register()` slots are a finite resource.** `OSD_JAIL_MAX` is the cap. A count of currently-used slots would need to be checked before adding new displayd OSDs; consult `sys/sys/osd.h`.
3. **`fbuf.nokbd` / `fbuf.nomouse` are checked at devfs-rule time, not at open time.** T6 owns the devfs rule mapping; T3's recipe assumes the sub-flags are stored as OSD bools and the devfs ruleset is generated at jail creation by reading the OSD values.
4. **T19's path corrections apply here too:** T3 references `sys/kern/kern_cgroup.c` which doesn't exist; the racct pattern (`sys/kern/subr_racct.c`) is the model for `prison_check_gpu`'s per-jail VRAM cap. T3's recipe (Recipe 3) uses the racct pattern.
5. **No cgroup framework exists in this base.** T20 (gpu_resource design) is design-only, un-testable on real hardware. The T3 recipe stores the cap data in the OSD struct, not in a cgroup.

## 10. Verdict (the one-line answer)

> **The FreeBSD jail API supports `allow.fbuf` (via `prison_add_allow()` — same as `allow.vmm`) and typed `fbuf.*` / `gpu.*` params (via `SYSCTL_JAIL_PARAM_*` + an OSD slot). The `fbuf.nokbd` / `fbuf.nomouse` sub-flags in the plan don't fit the standard `allow.X` shape; either accept `allow.fbuf.nokbd` (simplest) or add a new OSD with a sub-namespace. Three recipes, ~150 lines of new code, plus jail.conf.5 doc updates. No existing API is broken.**
