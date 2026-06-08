# T6 — `fbuf` Jail Parameter Set + `PRISON_FLAG_PRISON_FBUF` Spec

**Task**: T6 from `.sisyphus/plans/freebsd-display-abstraction.md` (Wave 1, design).
**Branch**: `framebuffer` (host worktree: `pppoe1.cloudbsd.org:~/git/freebsd-src-oci-fb-build`, HEAD `5b1b097b8e1`).
**Date**: 2026-06-08.
**Honcho session**: `displayd-reconcile-wave1-t0-t6`.
**Read-only**: no source code modified; no new params registered. Spec only.
**Predecessor**: T3 (jail API audit, see `.sisyphus/drafts/t3-jail-api-recipe.md`).
**Verdict**: **The plan's "PRISON_FLAG_PRISON_FBUF" name is misleading — FreeBSD's `PR_ALLOW_*` flags are packed into a single `pr_allow` bitmask on `struct prison`, not separate `PRISON_FLAG_*` defines. The right primitive is `prison_add_allow()` (the dynamic `allow.X` registration that `allow.vmm` / `allow.vmm_ppt` use), which returns a flag bit. The full param set: `allow.fbuf` + 3 typed `fbuf.*` params. The sub-flags `fbuf.nokbd` / `fbuf.nomouse` need to be renamed to `allow.fbuf.nokbd` / `allow.fbuf.nomouse` to fit the standard pattern, OR registered via a new OSD with a sub-namespace. See §3 for the deviation and §4 for the spec.**

---

## 1. Goal

Add a complete jail param set for the `fbuf` (framebuffer) capability:

- `allow.fbuf` (boolean) — enables the jail to access displayd framebuffer + kbd + mouse.
- `fbuf.width` (int) — framebuffer width in pixels.
- `fbuf.height` (int) — framebuffer height in pixels.
- `fbuf.transport` (string) — transport name (`"rfb"`, `"rdp"`, etc.).

Plus, per the plan's "Additive KBD/Mouse model" (§4.2 of the main plan), opt-out sub-flags:
- `fbuf.nokbd` (boolean) — opt out of the virtual keyboard.
- `fbuf.nomouse` (boolean) — opt out of the virtual mouse.

This spec is the **authoritative interface contract** that T9 (jail param registration) and T12 (displayd kernel module) must implement. The audit-style recipe is in T3; this document is the **interface spec** (what users see, what semantics each param has, what the precedence rules are, what the test cases are).

## 2. The "PRISON_FLAG_PRISON_FBUF" naming question

The plan's T6 title says "PRISON_FLAG_PRISON_FBUF spec" but the actual FreeBSD jail API does not use `PRISON_FLAG_*` for boolean capabilities. FreeBSD uses two mechanisms:

1. **Static `PR_ALLOW_*` defines** (`sys/sys/jail.h:256-274`) — the existing 18 static allow flags. These are bit values in `struct prison::pr_allow` (a 32-bit field). Each is a power of 2, defined as a `#define PR_ALLOW_FOO (1u << N)`.
2. **Dynamic `prison_add_allow()` registration** — `sys/kern/kern_jail.c:5253` allocates a free bit at runtime and returns it. Used by `allow.vmm` and `allow.vmm_ppt` (`sys/dev/vmm/vmm_dev.c:1215-1217`).

There is **no `PRISON_FLAG_PRISON_FBUF` define** in any current FreeBSD source. The plan's name appears to be a misnomer — the right primitive is either a new static `PR_ALLOW_FBUF` (added to `pr_flag_allow[]` at compile time) or a dynamic `prison_add_allow()` call from the `displayd.ko` module.

**Recommendation: dynamic `prison_add_allow()`** (Option A from T3 §4.3). Rationale: matches the `allow.vmm` precedent exactly; lets the new params be added without modifying the static `pr_flag_allow[]` table; lets `displayd.ko` be a loadable module that registers the flag on `MOD_LOAD` and (per the existing API limitation) cannot unregister on `MOD_UNLOAD`.

The user's `jail.conf` syntax for the dynamic flavor is identical to the static flavor — `allow.fbuf = true;` works whether `allow.fbuf` is static (in `pr_flag_allow[]`) or dynamic (registered via `prison_add_allow()`). The kernel does the dispatch.

## 3. The `fbuf.nokbd` / `fbuf.nomouse` sub-flag question

Per the plan §4.2 ("Additive KBD/Mouse model"), the keyboard and mouse are **included by default** when `allow.fbuf` is set, and the opt-out flags are `fbuf.nokbd` / `fbuf.nomouse`. The plan's spelling is `fbuf.nokbd`, not `allow.fbuf.nokbd`.

**Standard `prison_add_allow()` does not support this name shape.** The function signature is:

```c
unsigned prison_add_allow(const char *prefix, const char *name, ...);
```

When `prefix == NULL`, the registered name is `allow.<name>` / `allow.no<name>`. When `prefix != NULL`, the registered name is `allow.<prefix>.<name>` / `allow.<prefix>.no<name>`. **There is no way to register a bare `fbuf.nokbd` (no `allow.` prefix) via this API.**

The two ways to support the plan's spelling:

**Option A — rename the public spelling to `allow.fbuf.nokbd` / `allow.fbuf.nomouse`.**
- Cost: deviation from the plan's exact wording.
- Benefit: zero new kernel infrastructure. Reuses `prison_add_allow()` with `prefix = "fbuf"`, `name = "nokbd"`, and the registered name becomes `allow.fbuf.nokbd` / `allow.fbuf.nofbuf.nokbd`. Two more bits in `pr_allow`.
- Backward compat: none needed (this is a new param).

**Option B — add a new OSD with a sub-namespace.**
- The displayd OSD owns a per-jail struct with sub-fields `nokbd`, `nomouse`, `width`, `height`, `transport`, `nohid`, `allowhid`. The OSD provides `PR_METHOD_SET` / `PR_METHOD_GET` handlers that dispatch on the param name.
- `fbuf.nokbd` is a boolean in the OSD struct; `osd_jail_get(cred->cr_prison, displayd_osd, "fbuf.nokbd", &val)` returns it.
- Cost: a new OSD slot (uses one of the per-jail OSD allocations), and a non-standard userspace spelling (the kernel must be taught to dispatch `fbuf.X` style names, since the existing `pr_flag_allow[]` only knows `allow.X` / `allow.noX`).
- Benefit: matches the plan's spelling exactly.

**Recommendation: Option A.** The plan's spelling is a UX detail; the `allow.fbuf.nokbd` spelling is just as readable and reuses the entire existing jail-set machinery. The plan's spelling is **descriptive intent**, not a hard contract — the user-facing semantics ("kbd opt-out") are what matter.

## 4. The interface spec

### 4.1 `allow.fbuf` (boolean, default off)

- **What it does**: enables a jail to access the displayd-provided `/dev/fb0`, `/dev/kbd0`, `/dev/ums0` cdevs.
- **Default**: false. Without this, the jail sees no displayd devices.
- **When set**: the displayd kmod registers `allow.fbuf` on `MOD_LOAD` via `prison_add_allow(NULL, "fbuf", NULL, "desc")` and returns the flag bit. The displayd cdev's `open` checks `cred->cr_prison->pr_allow & pr_allow_fbuf_flag` and returns `EACCES` if not set.
- **Inheritance**: a child jail inherits from its parent; not in `PR_ALLOW_DIFFERENCES` so the child cannot relax it.
- **Default for prison0 (the host)**: false. The host does not need to use the displayd jail-fb path.

### 4.2 `allow.fbuf.nokbd` (boolean, default off)

- **What it does**: opt-out for the virtual keyboard. When set, the jail's devfs ruleset hides `/dev/kbd0` (and the displayd consumer does not provide a `notify_key` callback). Per the plan's "Additive" model, the **default** is kbd-**on**; this flag is the only way to get kbd-**off**.
- **Default**: false (kbd included).
- **When set**: the displayd kmod registers `allow.fbuf.nokbd` on `MOD_LOAD` via `prison_add_allow("fbuf", "nokbd", NULL, "desc")` and returns the flag bit. The devfs rule generator (T6's consumer, see §5) reads this flag at jail-creation time and adjusts the per-jail devfs ruleset to omit the kbd cdev.
- **Inheritance**: child inherits. A child can opt in / out independently of the parent? **No** — out of `PR_ALLOW_DIFFERENCES`, so a child cannot RELAX (cannot give a grandchild more access than itself). But a child CAN opt in / opt out at the same level? Hmm, this needs design discussion. **Recommendation**: keep the child identical to the parent for these sub-flags, by leaving them out of `PR_ALLOW_DIFFERENCES`. The opt-in / opt-out decision is made at the leaf jail only.

### 4.3 `allow.fbuf.nomouse` (boolean, default off)

Same as `nokbd`, for the mouse. Default off (mouse included).

### 4.4 `fbuf.width` (int, default 1920)

- **What it does**: framebuffer width in pixels. Used by displayd to size the per-jail mmap.
- **Default**: 1920 (a sensible HD width).
- **Range**: 1..16384. Reject values outside this range with `EINVAL`.
- **Persistence**: per-jail OSD struct (T3 Recipe 2).

### 4.5 `fbuf.height` (int, default 1080)

Same as `width`, for height. Default 1080.

### 4.6 `fbuf.transport` (string, default "rfb")

- **What it does**: the display_transport name to use when a remote viewer attaches.
- **Default**: `"rfb"`.
- **Allowed values**: `"rfb"`, `"rdp"`, `"bdp"`, `"none"`. Reject other values with `EINVAL`. (`"none"` means "no remote transport; local-only display"; the future broker may extend the list.)
- **Persistence**: per-jail OSD struct.

### 4.7 (Future, not in this spec) `allow.fbuf.nohid` / `allow.fbuf.allowhid`

The plan §4.2.4 introduces a HID-level pair (`fbuf.nohid` / `fbuf.allowhid`) for raw HID device passthrough. This is **out of scope for T6** because:
- It requires a `fbuf.hid` sub-tree with its own boolean pair (4-arg variant of `prison_add_allow` doesn't exist).
- The host-policy sysctl `security.fbuf.allowhid` (default 0) and the per-jail stricter-wins precedence are described in the plan but the kernel implementation is T9 / T12 / T20 work.

T6 spec **only covers `allow.fbuf` + `allow.fbuf.{nokbd,nomouse}` + the 3 typed `fbuf.*` params**. The HID pair is filed as a follow-up spec task (T6.A).

## 5. Precedence and interaction rules

The 5 params interact via these rules:

1. **`allow.fbuf` is the master switch.** If unset, the jail sees no displayd devices regardless of the other 4.
2. **`allow.fbuf.nokbd` and `allow.fbuf.nomouse` are opt-outs.** They have effect ONLY when `allow.fbuf` is set. Unsetting them (the default) is a no-op equivalent to having them unset.
3. **`fbuf.width` / `fbuf.height` are read at displayd module load time, per jail.** Changing them at runtime requires a displayd reload (not currently supported; T9 owns the reload path).
4. **`fbuf.transport` is read at every transport attach.** Multiple transports can be active simultaneously (e.g., a remote VNC viewer + a BDP multicast); the per-jail default sets which is tried first, but transports can also be specified on the broker's attach request.
5. **Resolution order at jail start**: `allow.fbuf` → if set, instantiate `/dev/fb0`. Then check `allow.fbuf.nokbd` → if set, skip `/dev/kbd0`. Then `allow.fbuf.nomouse` → if set, skip `/dev/ums0`. Then read `fbuf.width` / `fbuf.height` to size the fb mmap. Then read `fbuf.transport` to bind the default transport.

## 6. devfs ruleset mapping

The displayd module must install per-jail devfs rules for its cdevs. At jail-creation time, the displayd consumer (a userland helper or the kernel's `prison_attach` path) reads the per-jail OSD values and adds rules:

| Config | `/dev/fb0` | `/dev/kbd0` | `/dev/ums0` |
|---|---|---|---|
| `allow.fbuf` only | ✓ | ✓ | ✓ |
| `allow.fbuf; allow.fbuf.nokbd;` | ✓ | ✗ | ✓ |
| `allow.fbuf; allow.fbuf.nomouse;` | ✓ | ✓ | ✗ |
| `allow.fbuf; allow.fbuf.nokbd; allow.fbuf.nomouse;` | ✓ | ✗ | ✗ |
| (no `allow.fbuf`) | ✗ | ✗ | ✗ |

The devfs rule mechanism is the existing `DEVFSIO_RADD` ioctl on the per-jail devfs mount fd (`sys/fs/devfs/devfs_rule.c:160` — see T19 audit §3.1). Each rule is a `struct devfs_rule` (`sys/fs/devfs/devfs.h:66-100`) with conditions (path glob) and actions (mode/uid/gid/hide).

## 7. Test cases (T6 acceptance)

The T6 acceptance is the user-facing contract. ATF C tests live in T9 / T12's test plan, but T6 documents them here:

```
T6.1: jail with `allow.fbuf;` -> jls -v shows allow.fbuf=true; jail can open /dev/fb0
T6.2: jail with `allow.fbuf; allow.fbuf.nokbd;` -> /dev/kbd0 absent in jail's devfs
T6.3: jail with `allow.fbuf; allow.fbuf.nomouse;` -> /dev/ums0 absent in jail's devfs
T6.4: jail with `allow.fbuf; allow.fbuf.nokbd; allow.fbuf.nomouse;` -> only /dev/fb0
T6.5: jail with no `allow.fbuf` -> no displayd devices
T6.6: jail with `allow.fbuf; fbuf.width=1280; fbuf.height=720;` -> mmap size is 1280*720*4
T6.7: jail with `allow.fbuf; fbuf.transport=rdp;` -> default transport is "rdp"
T6.8: child jail with `allow.fbuf;` (parent has it too) -> child sees same devices
T6.9: child jail without `allow.fbuf;` (parent has it) -> child has no displayd
T6.10: jail with `allow.fbuf; fbuf.transport=bogus;` -> EINVAL at jail start
```

## 8. Files that will be touched (T9 / T12 implement, T6 spec only)

- `sys/modules/displayd/Makefile` (new)
- `sys/modules/displayd/displayd.c` (new — registers the allow flags + creates the cdevs)
- `sys/modules/displayd/displayd_params.c` (new — registers the typed params via OSD)
- `sys/conf/files` (1 line)
- `usr.sbin/jail/jail.conf.5` (~30 lines added)
- `usr.sbin/bhyve/displayd_consumer.c` (new — T12, the bhyve-side consumer for jail-attached displayd; not a T6 file)
- `sys/modules/displayd/devfs_rules.c` (new — generates the per-jail devfs ruleset at jail start; not a T6 file)

## 9. Out of scope / follow-up

- **T6.A** — HID-level params (`allow.fbuf.nohid` / `allow.fbuf.allowhid` + host-policy `security.fbuf.allowhid` sysctl). Requires either a 4-string variant of `prison_add_allow` or a new OSD with a sub-namespace. Defer until the HID sub-tree is needed.
- **T6.B** — Multi-display params (`fbuf.0.*` / `fbuf.1.*` per the plan's T53 multi-display design). Defer until multi-display is in scope.
- **T6.C** — Runtime reload of `fbuf.width` / `fbuf.height` (changing the fb size without restarting the jail). The T9 design does not support this; T9.A follow-up can add it.

## 10. Verdict

> **The "fbuf" jail param set is `allow.fbuf` + `allow.fbuf.{nokbd,nomouse}` (3 boolean flags) + `fbuf.{width,height,transport}` (3 typed params), implemented via `prison_add_allow()` for the booleans and an OSD for the typed params. The plan's `PRISON_FLAG_PRISON_FBUF` name is a misnomer — the right primitive is a dynamic `prison_add_allow()` bit, not a new `PR_ALLOW_*` define. The sub-flag spelling `fbuf.nokbd` deviates from the standard `allow.X` shape; either rename to `allow.fbuf.nokbd` (simplest, recommended) or add an OSD sub-namespace. T9 / T12 implement, T6 spec only.**
