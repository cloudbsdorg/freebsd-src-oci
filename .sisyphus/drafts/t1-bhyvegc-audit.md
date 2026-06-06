# T1 — `bhyvegc` API audit

**Task**: T1 from `.sisyphus/plans/freebsd-display-abstraction.md` (Wave 1, recon).
**Branch**: `displayd` on `freebsd-src-pppoe` (host `pppoe1.cloudbsd.org`).
**Read-only**: no source code modified.
**Verdict**: **`bhyvegc` is pure pixel manipulation, safe for non-VM use.** 0 hidden couplings.

---

## 1. File locations

All files live under `usr.sbin/bhyve/` on the `displayd` branch.

| File | Lines | Role |
|---|---:|---|
| `usr.sbin/bhyve/bhyvegc.h` | 46 | Public API header — opaque `struct bhyvegc`, public `struct bhyvegc_image`, 4 functions |
| `usr.sbin/bhyve/bhyvegc.c` | 100 | Implementation — only 4 public functions, all pure pixel-buffer manipulation |
| `usr.sbin/bhyve/console.h` | 53 | Vtable-style typedefs + 8 public functions (singleton wrapper) |
| `usr.sbin/bhyve/console.c` | 117 | Singleton state — one global `console` struct, callbacks registered via priority |
| `usr.sbin/bhyve/pci_fbuf.c` | 487 | Sole current consumer of `bhyvegc`; calls `console_init` + `console_fb_register` |

### `grep -rln "bhyvegc" usr.sbin/bhyve/` (full reference set)

```
usr.sbin/bhyve/Makefile
usr.sbin/bhyve/amd64/vga.c
usr.sbin/bhyve/amd64/vga.h
usr.sbin/bhyve/bhyvegc.c
usr.sbin/bhyve/bhyvegc.h
usr.sbin/bhyve/console.c
usr.sbin/bhyve/console.h
usr.sbin/bhyve/pci_fbuf.c
usr.sbin/bhyve/rfb.c
usr.sbin/bhyve/usb_mouse.c
```

### `grep -rln "console_kbd_register\|console_ptr_register\|console_fb_register" usr.sbin/bhyve/`

```
usr.sbin/bhyve/pci_fbuf.c          (console_fb_register at line 445)
usr.sbin/bhyve/console.c           (the dispatchers)
usr.sbin/bhyve/usb_mouse.c         (console_ptr_register at line 317)
usr.sbin/bhyve/amd64/ps2mouse.c    (console_ptr_register at line 415)
usr.sbin/bhyve/amd64/ps2kbd.c      (console_kbd_register at line 502)
usr.sbin/bhyve/console.h           (declarations)
```

---

## 2. Public API — `bhyvegc.h` (the whole thing)

```c
struct bhyvegc;                           /* opaque handle */

struct bhyvegc_image {                    /* public pixel-buffer descriptor */
    int       vgamode;                    /* 0 = raw RGBA32, non-zero = VGA-emulated */
    int       width;
    int       height;
    uint32_t *data;                       /* pixel storage — caller- or self-allocated */
};

struct bhyvegc *bhyvegc_init(int width, int height, void *fbaddr);
void            bhyvegc_set_fbaddr(struct bhyvegc *gc, void *fbaddr);
void            bhyvegc_resize(struct bhyvegc *gc, int width, int height);
struct bhyvegc_image *bhyvegc_get_image(struct bhyvegc *gc);
```

That is the entire public surface. **4 functions. No `bhyvegc_text_*`, no `bhyvegc_draw_*`, no `bhyvegc_refresh`, no `bhyvegc_destroy`.** The plan's expectation of "430+ lines" and a text/draw API was based on an out-of-date or inflated assumption. On the `displayd` branch `bhyvegc` is 100 lines of impl and is purely a framebuffer *buffer* abstraction, not a graphics console. (The console logic — text mode, font rendering — lives in `amd64/vga.c` and is x86/amd64-specific, NOT inside `bhyvegc`.)

### Function-by-function audit

| Function | Inputs | Touches VMM? | Touches `struct vm*`? | Notes |
|---|---|:---:|:---:|---|
| `bhyvegc_init(w, h, fbaddr)` | dimensions + opaque `void *fbaddr` | **No** | **No** | If `fbaddr != NULL`, sets `gc->raw = 1` and uses `fbaddr` directly. If `fbaddr == NULL`, allocates `calloc(w*h*4)` and `raw = 0`. Pure memory ownership decision. |
| `bhyvegc_set_fbaddr(gc, fbaddr)` | opaque `void *` | **No** | **No** | Flips `raw = 1`; frees the previously-owned buffer if it was self-allocated and differs from the new one. Idiomatic swap — designed for "the framebuffer moved" use cases. |
| `bhyvegc_resize(gc, w, h)` | new dimensions | **No** | **No** | In `raw` mode: just updates `width`/`height` (the caller owns storage, so we can't resize it). In non-raw mode: `reallocarray` + `memset` zero-fill. No VMM dependency. |
| `bhyvegc_get_image(gc)` | opaque handle | **No** | **No** | Returns the `bhyvegc_image*` pointer. NULL-guard for an uninitialized handle. |

### Internal struct (`bhyvegc.c:37-40`)

```c
struct bhyvegc {
    struct bhyvegc_image *gc_image;
    int raw;                              /* 0 = self-allocated, 1 = caller-provided */
};
```

No hidden fields. The `void *fbaddr` is *not* stored on the `struct bhyvegc` — it is stored *inside* `gc_image->data`. Swapping it is a one-line write.

### Why the "0 hidden couplings" verdict is safe

- **No header includes anything from `vmmapi.h`, `vm.h`, or any VMM-specific header** (`bhyvegc.c` only pulls `<sys/types.h>`, `<stdlib.h>`, `<stdio.h>`, `<string.h>` and the local `bhyvegc.h`).
- **No functions take a `struct vm *` or `struct vmspace *`.** The only thing passed in is a `void *` that the *caller* must have allocated.
- **The `raw` flag exists precisely to decouple bhyvegc from allocation policy.** When the consumer is `pci_fbuf` it passes a `vm_create_devmem(...)` pointer; when the consumer is a future jail/fbuf it will pass a `mmap` of `/dev/fb0` from the `displayd.ko` module. bhyvegc is indifferent to the source.
- **No globals, no static state, no hidden singletons inside `bhyvegc.c`.** The singleton lives one level up in `console.c` — see §3.

---

## 3. `console` module — the vtable-style wrapper

`console.c` wraps `bhyvegc` in a single-instance facade and adds the input fan-out (kbd + ptr) and the framebuffer render callback. It is a **vtable-style singleton** with one file-static state struct.

### State (`console.c:34-47`)

```c
static struct {
    struct bhyvegc     *gc;            /* the wrapped bhyvegc */

    fb_render_func_t   fb_render_cb;
    void              *fb_arg;

    kbd_event_func_t   kbd_event_cb;
    void              *kbd_arg;
    int                kbd_priority;

    ptr_event_func_t   ptr_event_cb;
    void              *ptr_arg;
    int                ptr_priority;
} console;
```

One framebuffer render callback, one keyboard callback, one pointer callback. **No array, no list, no chaining.**

### Typedefs (`console.h:34-36`)

```c
typedef void (*fb_render_func_t)(struct bhyvegc *gc, void *arg);
typedef void (*kbd_event_func_t)(int down, uint32_t keysym, uint32_t keycode, void *arg);
typedef void (*ptr_event_func_t)(uint8_t mask, int x, int y, void *arg);
```

### Public API (`console.h:38-51`)

| Function | Purpose | Notes |
|---|---|---|
| `console_init(int w, int h, void *fbaddr)` | **One-shot init.** Wraps `bhyvegc_init`. | No corresponding `console_destroy()`. Calling it twice is undefined — would leak the old `gc` and overwrite the static. |
| `console_set_fbaddr(void *fbaddr)` | Swaps the framebuffer pointer. | Wraps `bhyvegc_set_fbaddr`. |
| `console_get_image(void)` | Returns `bhyvegc_get_image(console.gc)`. | |
| `console_fb_register(render_cb, arg)` | Registers the *one* render callback. | **No priority, no chaining, no deregister.** Last writer wins. |
| `console_refresh(void)` | Calls `console.fb_render_cb(console.gc, console.fb_arg)` if non-NULL. | The "render" path — it pulls VGA or other emulation logic via the registered cb. |
| `console_kbd_register(event_cb, arg, pri)` | Higher-priority cb replaces lower. | Strictly `>` comparison, not `>=`. Multiple registrants at the same priority → first one wins. |
| `console_key_event(down, keysym, keycode)` | Dispatches to the current kbd cb. | **No fan-out — single consumer.** |
| `console_ptr_register(event_cb, arg, pri)` | Same priority rule as kbd. | |
| `console_ptr_event(button, x, y)` | Dispatches to the current ptr cb. | **No fan-out — single consumer.** |

### Init / shutdown gap

`console_init` exists; **there is no `console_destroy` and no `console_fb_unregister`**. This is acceptable for bhyve (process-scoped singleton) but will need both for the multi-instance displayd work (T8 / T12).

---

## 4. How `bhyvegc` integrates with `console` + `pci_fbuf`

The current end-to-end flow is short and linear:

```
pci_fbuf_init (pci_fbuf.c:402)
  │
  ├── sc->fb_base = vm_create_devmem(pi->pi_vmctx, VM_FRAMEBUFFER,
  │                                 "framebuffer", FB_SIZE);   ← VMM-coupled HERE
  │                                                          (NOT inside bhyvegc)
  │
  ├── console_init(sc->memregs.width, sc->memregs.height, sc->fb_base);  ← L444
  │       │
  │       └── bhyvegc_init(w, h, fbaddr)
  │              └── gc_image->data = fbaddr;  raw = 1;
  │
  ├── console_fb_register(pci_fbuf_render, sc);                 ← L445
  │       (registers the render callback; last-writer-wins)
  │
  ├── sc->gc_image = console_get_image();                       ← L449
  │       (cached in softc — used by rfb.c and usb_mouse.c)
  │
  └── error = rfb_init(...);                                    ← L455
          (legacy direct path, NOT going through any transport vtable)
```

### Render path (when does `console_refresh` fire?)

The render callback is invoked from inside `rfb_send_screen` (rfb.c:721), called from the per-client RFB thread. It is **not** invoked periodically on its own; the VNC client must request an update. There is no internal "tick" in bhyve that triggers `console_refresh`.

```
VNC client → RFB UpdateRequest → rfb_send_screen (rfb.c:689)
  ├── console_refresh()                                  (rfb.c:721)
  │     └── pci_fbuf_render(gc, sc)                      (console.c → pci_fbuf.c:363)
  │           ├── if (vga_full && vgamode): vga_render(gc, sc->vgasc)
  │           └── if size changed: bhyvegc_resize(...)
  └── gc_image = console_get_image()                     (rfb.c:722)
        └── iterate pixels → RFB frame update
```

### VGA fallback (`amd64/vga.c:319-334`)

When `vga_full` is set and `vgamode != 0`, the render cb delegates to `vga_render(gc, sc->vgasc)`. `vga_render` is x86-specific and lives in `amd64/` because it implements VGA text-mode font and CRTC register semantics. It calls `vga_check_size` → `bhyvegc_resize` if the CRTC mode change altered the visible resolution.

This means **`bhyvegc` carries a `vgamode` flag and the softc-driven `vga_full` flag, but neither bhyvegc itself nor its API has any VGA-specific code paths**. The flag is purely a hint for the consumer; it's the consumer's choice whether to honor it.

---

## 5. `fb_info` flow to consumers

`bhyvegc_image` (re-excerpted for clarity) is the only framebuffer-info struct in the system:

```c
struct bhyvegc_image {
    int       vgamode;      /* 0 = raw, non-zero = VGA mode */
    int       width;
    int       height;
    uint32_t *data;         /* always RGBA32, row-major, no stride */
};
```

There is **no `fb_info_t` struct in the current code.** The plan (§1) and the Mermaid diagram (§3.7) use `fb_info_t` as a future abstraction, but today the only public type carrying framebuffer metadata is `struct bhyvegc_image`. When a transport needs dimensions and a pixel pointer it asks `console_get_image()` and dereferences `.width`, `.height`, `.data` directly.

### Who reads `bhyvegc_image`?

| Site | What it reads | Why |
|---|---|---|
| `rfb.c:274, 517, 601, 693, 722` | `width`, `height`, `data` | Pixel format + frame encoding to RFB clients |
| `usb_mouse.c:262` | `gc` (for liveness check, not pixel access) | Guard against `console_get_image` returning NULL when guest is not yet painting |
| `amd64/vga.c:182, 208, 319, 1287` | `width`, `height`, `data` | VGA text/graphics rendering into the buffer |
| `pci_fbuf.c:112, 363, 378, 449` | all fields | Caches `gc_image` on the softc, drives resize, dispatches to vga_render |

There is **no async notification** for framebuffer size or address changes. Each consumer polls by calling `console_get_image()` and comparing against its own cached `gc_width` / `gc_height` (see `pci_fbuf.c:374-379` and `amd64/vga.c:182-209`).

---

## 6. Transport hooks — what exists today and what's missing

### What exists (a *direct*, non-pluggable path)

- `bhyvegc_init` / `bhyvegc_set_fbaddr` / `bhyvegc_get_image` — the only transport-relevant hooks, and they are about **framebuffer ownership**, not network transport.
- `rfb_init(family, host, port, wait, password)` — called once, directly from `pci_fbuf_init` (pci_fbuf.c:455). RFB is **hard-wired**. There is no `transport=...` option, no registry, no pluggable backend, no second transport.
- The thread that drives RFB is created inside `rfb_init` (rfb.c:1341). The render callback is the one registered via `console_fb_register` in `pci_fbuf_init`. Shutdown is process exit — no graceful disconnect path on the bhyve side.

### What is missing for pluggable transports

| Hook | Today | Needed for `display_transport` vtable |
|---|---|---|
| `transport_init(args, security)` | `rfb_init(family, host, port, wait, password)` — RFB-specific | Generic signature taking a key/value list + security policy. |
| `transport_shutdown()` | **none** | Must close client sockets, join threads, free per-client state. |
| `transport_name()` | hardcoded `rfb` in bhyve config | Returns the registered name (used by config parser, audit log, sysctl). |
| `transport_attach(fb_info)` | implicit — happens before `rfb_init` returns | Explicit handshake: "here's the fb, you may start sending pixels." |
| `transport_detach()` | **none** | Cleanup before `transport_shutdown`. |
| `transport_send_input(event)` | partial — RFB thread reads from socket directly and calls `console_key_event` / `console_ptr_event` | Generic: any transport can produce input. |

### What is missing for multi-instance (jail + displayd.ko)

| Hook | Today | Needed |
|---|---|---|
| `console_create(w, h, fbaddr) → ctx*` | `console_init` (singleton) | Instance handle. |
| `console_destroy(ctx)` | **none** | Frees `gc`, clears state, joins threads. |
| `console_fb_unregister(ctx)` | **none** | Lets a transport disown a framebuffer on detach. |
| `console_fb_register(ctx, cb, arg)` | singleton | Per-instance registration. |
| `console_kbd_register(ctx, cb, arg, pri)` | singleton, single-cb | Per-instance, with a *list* of cbs (T2 input fan-out owns this). |

The plan correctly identifies these gaps; the T8 "console refactor" task in §14 is the work that closes them.

---

## 7. Limitations and extension points

### Limitations (constraints to be aware of for downstream tasks)

1. **Singleton in `console.c`.** All `console_*` calls reach a single file-static `struct console`. Multi-instance is impossible without a refactor.
2. **No fan-out for kbd/ptr input.** `console_kbd_register` and `console_ptr_register` are single-callback with priority; the lower-priority registrant is silently dropped. The plan calls this out in T2 (input fan-out audit).
3. **No async notification for size or fbaddr changes.** Consumers must poll. Acceptable for VNC (refresh is client-driven) but may not be for a future broker that needs to push frame metadata to its registry.
4. **Pixel format is hardcoded to RGBA32** (`uint32_t *data`, `width * height * sizeof(uint32_t)`). No YUV, no RGB565, no indexed palette. The RFB encoder adapts via `pixfmt` mutation, not by changing the buffer layout.
5. **`vga_render` and the text/draw code are amd64-specific** (live in `usr.sbin/bhyve/amd64/vga.c`). For non-x86 architectures (aarch64, riscv), bhyve ships with raw framebuffer only — there is no portable text console in bhyve today. This is an architecture constraint, not a bhyvegc defect.
6. **No `console_destroy` / no graceful teardown.** The framebuffer render cb cannot be unregistered; the kbd/ptr callbacks cannot be cleared. The whole process exits and the OS reclaims.
7. **`vgamode` is a flag, not a real mode descriptor.** It's `0` (raw) or "non-zero" (VGA). Whoever set the bit is responsible for calling `vga_render` if the data is meant to look like a VGA framebuffer.

### Extension points (where the abstraction lands cleanly)

1. **`bhyvegc` is the right type for the lower layer of a `display_backend`.** It already does (a) caller-provided pixel buffer, (b) self-allocated fallback, (c) resize, (d) fbaddr swap. The only thing to add is a `bhyvegc_destroy` for symmetry.
2. **`struct bhyvegc_image` is a good proto `fb_info_t`.** Add a pixel-format enum and a stride field and you have a transport-agnostic frame descriptor. Backward compat: zero the new fields, keep the existing 4-field layout for callers that don't care.
3. **`bhyvegc_set_fbaddr` is the swap point for a future `displayd.ko` mmap.** When the fbuf module is added to a jail, the jail's `console_init` equivalent will call `bhyvegc_init(w, h, fb_shm_ptr)` where `fb_shm_ptr` is an `mmap` of `/dev/fb0` rather than a `vm_create_devmem` pointer. bhyvegc does not care.
4. **`console.c` is the right place to grow a vtable.** It already exposes a callback-registration model. Add per-instance state, a `console_create`/`console_destroy` pair, and the existing `console_kbd_register` etc. become per-instance calls. The dispatchers in `console_key_event` / `console_ptr_event` become per-instance dispatch.
5. **RFB can be wrapped as a `struct display_transport` without touching `rfb.c`'s pixel-encoding core.** The current `rfb_init` is the only thing that needs to call `transport_register("rfb", ...)`. The rest of `rfb.c` (the encoder, the client thread, the msg parsers) is purely about producing/consuming pixel data on a socket, which is exactly what a transport vtable entry point does.

---

## 8. Function table — required deliverable per T1 acceptance

For convenience, the consolidated function table (per the plan's "Output" spec):

| Function | Inputs | VMM-coupled? | Lines (impl) |
|---|---|:---:|---:|
| `bhyvegc_init` | `(int w, int h, void *fbaddr)` | No | 100:42-64 |
| `bhyvegc_set_fbaddr` | `(struct bhyvegc *gc, void *fbaddr)` | No | 100:66-73 |
| `bhyvegc_resize` | `(struct bhyvegc *gc, int w, int h)` | No | 100:75-91 |
| `bhyvegc_get_image` | `(struct bhyvegc *gc)` | No | 100:93-100 |
| `console_init` | `(int w, int h, void *fbaddr)` | No (delegates) | 117:49-53 |
| `console_set_fbaddr` | `(void *fbaddr)` | No (delegates) | 117:55-59 |
| `console_get_image` | `()` | No (delegates) | 117:61-69 |
| `console_fb_register` | `(fb_render_func_t cb, void *arg)` | No | 117:71-76 |
| `console_refresh` | `()` | No | 117:78-83 |
| `console_kbd_register` | `(kbd_event_func_t cb, void *arg, int pri)` | No | 117:85-93 |
| `console_key_event` | `(int down, uint32_t sym, uint32_t code)` | No | 117:105-110 |
| `console_ptr_register` | `(ptr_event_func_t cb, void *arg, int pri)` | No | 117:95-103 |
| `console_ptr_event` | `(uint8_t button, int x, int y)` | No | 117:112-117 |

**Total entries: 13.** All non-VMM-coupled.

### Verdict (per the plan's "Output" spec, last line of the document)

> **bhyvegc is pure pixel manipulation, safe for non-VM use.**

0 hidden couplings. The only VMM dependency in the whole pipeline is the one line `sc->fb_base = vm_create_devmem(pi->pi_vmctx, VM_FRAMEBUFFER, "framebuffer", FB_SIZE);` in `pci_fbuf.c` — and even that is hidden behind a `void *` before it reaches bhyvegc.

---

## 9. Recommendations for downstream tasks

- **T4 (transport vtable)** can be designed against `struct bhyvegc_image` directly; no new fb_info struct is needed for V1. Add the format/stride fields when an actual second transport (RDP, BDP) needs them.
- **T5 (backend vtable)** can be designed against `bhyvegc` itself, with a thin wrapper that adds `backend_get_fb_info()` returning a `const struct bhyvegc_image *`.
- **T8 (console refactor)** must address: (1) the singleton → instance refactor, (2) the missing `console_destroy` / `console_fb_unregister`, (3) the fan-out question (T2 owns the input side).
- **T12 (displayd kernel module)** should expose its mmap as a `void *`; the userland side will hand it to a new `console_create` wrapper that calls `bhyvegc_init(w, h, shm_ptr)`. Zero changes to `bhyvegc.c` are needed.
- **T13 (pci_fbuf wire)** should add a `transport=...` config option that goes through a `display_transport_init("rfb", ...)` indirection, leaving `rfb_init` as the registered implementation. The "fb" half stays on the bhyve/consoles path.
