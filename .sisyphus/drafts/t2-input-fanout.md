# T2 — Input Fan-out Audit (who consumes `console_key_event` / `console_ptr_event`?)

**Task**: T2 from `.sisyphus/plans/freebsd-display-abstraction.md` (Wave 1, recon).
**Branch**: `framebuffer` on `freebsd-src-oci` (host `pppoe1.cloudbsd.org`).
**Read-only**: no source code modified.
**Predecessors**: T1 (bhyvegc audit) PASS 2026-06-06, T19 (GPU kernel audit) PASS 2026-06-06.
**Honcho session**: `displayd-t2-input-fanout` (Honcho = MCP peer-memory service for cross-session agent context; not a CLI binary).
**Verdict**: **Existing path is sufficient for bhyve VMs (amd64 PS/2, all-arch USB mouse), but is INSUFFICIENT for jails. Jails need a new consumer path that writes to `/dev/kbd0` / `/dev/ums0` (or whatever the displayd module exposes), not to a guest PS/2 controller or USB HCI.** See §6 for the jail-fit analysis.

---

## 1. Scope

The plan asked: "Trace the path: VNC client sends a key → `rfb_recv_key_msg` (rfb.c:899) → `console_key_event` (rfb.c:909) → registered kbd consumer → ??? → guest." This audit answers that, with the matching pointer path, and addresses the jail-fit question.

## 2. Call-site inventory

### 2.1 Registrants (the actual consumers, in priority order)

| File:line | Function | Priority | Notes |
|---|---|---:|---|
| `usr.sbin/bhyve/amd64/ps2kbd.c:502` | `ps2kbd_event` | 1 | x86-only; wired to atkbdc / i8042 |
| `usr.sbin/bhyve/amd64/ps2mouse.c:415` | `ps2mouse_event` | 1 | x86-only; wired to atkbdc aux / i8042 aux |
| `usr.sbin/bhyve/usb_mouse.c:317` | `umouse_event` | 10 | All-arch source; arch-independent; **priority 10 wins over ps2mouse at priority 1 when both register** |

Strict `>` comparison (`console.c:88,98`) — the **higher** priority wins; equal priorities keep the first registrant (no fan-out). When both PS/2 mouse and USB mouse register, USB mouse's priority 10 wins, so the registered ptr consumer is `umouse_event`.

### 2.2 Dispatchers (producers calling the console layer)

| File:line | Function | Triggered by |
|---|---|---|
| `usr.sbin/bhyve/rfb.c:909` | `console_key_event(key_msg.down, htonl(key_msg.sym), htonl(0))` | `rfb_recv_key_msg` (rfb.c:899) — standard RFB KeyEvent msg |
| `usr.sbin/bhyve/rfb.c:932` | `console_key_event((int)extkey_msg.down, htonl(extkey_msg.sym), htonl(extkey_msg.code))` | `rfb_recv_client_msg` (rfb.c:916) — RFB_CLIENTMSG_EXT_KEYEVENT (extended key event) |
| `usr.sbin/bhyve/rfb.c:949` | `console_ptr_event(ptr_msg.button, htons(ptr_msg.x), htons(ptr_msg.y))` | `rfb_recv_ptr_msg` (rfb.c:940) — RFB PointerEvent msg |
| `usr.sbin/bhyve/console.c:106` | `console_key_event` (impl) | single-consumer dispatch (`if (console.kbd_event_cb) ...`) |
| `usr.sbin/bhyve/console.c:113` | `console_ptr_event` (impl) | single-consumer dispatch |

**No other producers call `console_key_event` / `console_ptr_event`.** The grep `console_key_event\|console_ptr_event` across `usr.sbin/bhyve/` returns only the 3 rfb.c sites and the 2 console.c dispatcher implementations (plus the declarations in `console.h:48,51`). All input events come from the RFB transport; there is no other transport emitting console events today.

## 3. Keyboard path (VNC → i8042 IRQ 1)

```
VNC client bytes
  └─> rfb_recv_key_msg (rfb.c:899)
        └─> stream_read (rfb.c:904)               [len = stream_read(cfd, ...)]
        └─> console_key_event(down, sym, 0)        (rfb.c:909)
              └─> console.c:106 dispatcher
                    └─> ps2kbd_event(down, sym, code, arg)  (ps2kbd.c:394)
                          ├─> pthread_mutex_lock(&sc->mtx)   (ps2kbd.c:399)
                          ├─> if (!sc->enabled) return        (ps2kbd.c:400-403)
                          ├─> ps2kbd_keysym_queue(sc, down, sym, code)  (ps2kbd.c:405)
                          │     └─> fifo_put(sc, code)         (ps2kbd.c:390, called from ps2kbd_keysym_queue)
                          ├─> pthread_mutex_unlock(&sc->mtx)   (ps2kbd.c:406)
                          └─> if (!fifo_full) atkbdc_event(sc->atkbdc_sc, 1)  (ps2kbd.c:409)
                                └─> atkbdc_event (atkbdc.c:500)
                                      ├─> pthread_mutex_lock(&sc->mtx)
                                      ├─> atkbdc_kbd_poll(sc)   (atkbdc.c:505)
                                      │     └─> atkbdc_assert_kbd_intr(sc)  (atkbdc.c:242)
                                      │           └─> vm_isa_pulse_irq(sc->ctx, sc->kbd.irq, sc->kbd.irq)  (atkbdc.c:147)
                                      │                 └─> guest receives IRQ 1 (KBD_DEV_IRQ)  (atkbdc.c:62)
                                      └─> pthread_mutex_unlock(&sc->mtx)
```

**Delivery to guest**: standard i8042 IRQ 1. The guest's i8042 driver reads the scancode from `KBD_DATA_PORT` (port 0x60) via the in/out handler registered at `atkbdc.c:539` (`iop.handler = atkbdc_data_handler`).

**Key transformation**: `keysym` (X11/RFB key symbol) → `keycode` (PS/2 scancode, 0-127 + E0 prefix). The translation table lives in `ps2kbd.c` (`ascii_translations[]`, `extended_translations[]`).

## 4. Pointer path — PS/2 (VNC → i8042 IRQ 12)

```
VNC client bytes
  └─> rfb_recv_ptr_msg (rfb.c:940)
        └─> stream_read (rfb.c:945)
        └─> console_ptr_event(button, x, y)        (rfb.c:949)
              └─> console.c:113 dispatcher
                    └─> ps2mouse_event(button, x, y, arg)  (ps2mouse.c:372)
                          ├─> pthread_mutex_lock(&sc->mtx)
                          ├─> movement_update(sc, x, y)         (ps2mouse.c:377)
                          ├─> sc->status |= PS2M_STS_LEFT_BUTTON|... per button bits  (ps2mouse.c:379-386)
                          ├─> if (!sc->ctrlenable || !PS2M_STS_ENABLE_DEV) return  (ps2mouse.c:388-392)
                          ├─> movement_get(sc)                  (ps2mouse.c:394)
                          ├─> pthread_mutex_unlock(&sc->mtx)
                          └─> if (sc->fifo.num > 0) atkbdc_event(sc->atkbdc_sc, 0)  (ps2mouse.c:398)
                                └─> atkbdc_event (atkbdc.c:500)
                                      ├─> atkbdc_aux_poll(sc)  (atkbdc.c:507)
                                      │     └─> atkbdc_assert_aux_intr(sc)  (atkbdc.c:251)
                                      │           └─> vm_isa_pulse_irq(sc->ctx, sc->aux.irq, sc->aux.irq)  (atkbdc.c:156)
                                      │                 └─> guest receives IRQ 12 (AUX_DEV_IRQ)  (atkbdc.c:63)
                                      └─> pthread_mutex_unlock(&sc->mtx)
```

**Delivery to guest**: standard i8042 aux IRQ 12.

**Note on priority**: ps2mouse registers at priority 1 (ps2mouse.c:415). If usb_mouse is also registered (priority 10), the dispatcher drops the ps2mouse callback and only `umouse_event` is invoked. The atkbdc aux path is therefore inactive when USB mouse is enabled.

## 5. Pointer path — USB (VNC → USB HCI interrupt)

```
VNC client bytes
  └─> rfb_recv_ptr_msg (rfb.c:940)
        └─> console_ptr_event(button, x, y)        (rfb.c:949)
              └─> console.c:113 dispatcher
                    └─> umouse_event(button, x, y, arg)  (usb_mouse.c:257)
                          ├─> gc = console_get_image()  (usb_mouse.c:262)
                          ├─> if (gc == NULL) return    (usb_mouse.c:263-266)
                          ├─> pthread_mutex_lock(&sc->mtx)
                          ├─> sc->um_report.buttons, .z, .x, .y = ...  (usb_mouse.c:272-289)
                          │     └─> scale: x' = MOUSE_MAX_X * x / gc->width
                          │              y' = MOUSE_MAX_Y * y / gc->height
                          ├─> sc->newdata = 1
                          ├─> pthread_mutex_unlock(&sc->mtx)
                          ├─> pthread_mutex_lock(&sc->ev_mtx)
                          ├─> sc->hci->hci_intr(sc->hci, UE_DIR_IN | UMOUSE_INTR_ENDPT)  (usb_mouse.c:293)
                          │     └─> (XHCI/EHCI emulation) raises USB endpoint interrupt on UMOUSE_INTR_ENDPT
                          │           └─> guest USB HID driver reads mouse report via control/bulk transfer
                          └─> pthread_mutex_unlock(&sc->ev_mtx)
```

**Delivery to guest**: USB interrupt endpoint (HID boot mouse protocol, REPORT protocol — set at `usb_mouse.c:313` `sc->hid.protocol = 1`). The guest's USB HID driver reads the report via the emulated HCI (XHCI/EHCI).

**Key observation**: `umouse_event` reads `gc->width` and `gc->height` (usb_mouse.c:287-288) to scale VNC pixel coordinates to the USB mouse's `MOUSE_MAX_X × MOUSE_MAX_Y` report range. This is the only input consumer that depends on the framebuffer state — it assumes a 1:1 mapping between VNC pointer coords and screen pixels, scaled to the mouse's logical range.

## 6. Jail-fit verdict

### 6.1 What's the question?

The plan's T2 acceptance criterion asks: "A 'jail-fit verdict' line states whether the existing path is sufficient for jail-attached consumers." Translation: when `displayd.ko` is loaded into a jail and exposes a framebuffer + kbd + mouse via `allow.fbuf`, can a VNC client (or BDP client) feed input back to the jail using the **same** `console_key_event` / `console_ptr_event` API, just with a different `event_cb` registered?

### 6.2 Answer: **NO — the existing path is not sufficient for jails.** A new consumer is required.

**Reasoning:**

1. **PS/2 path (`ps2kbd_event` / `ps2mouse_event`) is x86-only.** Both files live in `usr.sbin/bhyve/amd64/`. Their only registered caller is the atkbdc layer (`atkbdc_init(ctx)` at `amd64/bhyverun_machdep.c:358`). Jails have **no x86 i8042 controller, no PS/2 port, no IRQ 1 or IRQ 12** — they have no emulated chipset at all. `atkbdc_event` calls `vm_isa_pulse_irq(sc->ctx, ...)` with a `struct vmctx *` that **only exists for bhyve VMs**, not jails. Even if you forced registration, the `vm_isa_pulse_irq` call would dereference a NULL `ctx` and panic the host.

2. **USB mouse path (`umouse_event`) depends on a USB HCI.** `sc->hci->hci_intr` is the XHCI/EHCI's interrupt-raise function. The USB HCI is attached to a bhyve VM's PCI bus, not to a jail. A jail has no PCI bus, no USB host controller, no HID stack in the guest sense. The `hci_intr` would not be wired.

3. **Jails need a fundamentally different delivery mechanism.** Input to a jail means writing to a kernel-side `kbd` / `ums` cdev (or a `displayd.ko` mmap'd buffer) that the jail's userland (or another consumer) reads. This is the **opposite direction** from the bhyve path, which is "transport bytes → kernel pushes to guest via IRQ / endpoint." For jails, it's "transport bytes → kernel writes to a per-jail buffer → jail's `/dev/kbd0` / `/dev/ums0` reads (or the broker reads via shared memory)."

4. **The `console.c` dispatcher is single-consumer and singleton.** Even if the PS/2 or USB paths were salvageable, `console_kbd_register` keeps a single callback and a single priority. A future displayd jail fb needs to be **multi-instance** (one per jail), which is T8's refactor (singleton → instance). The current path therefore can't even register two jail-attached consumers concurrently.

5. **`gc_image` read in `umouse_event` (usb_mouse.c:262,287-288) is for VNC coordinate scaling only.** The jail path will need an equivalent scaling step but against a jail-local framebuffer size, not the singleton console's. This argues for moving the scaling into the per-display consumer (the new `displayd_backend` or whatever T8's refactor produces), not into a single callback that knows about VNC.

### 6.3 What the new path should look like (design input for T8 / T12 / T13)

The `displayd.ko` module exposes a per-jail cdev (e.g., `/dev/fb0` + `/dev/kbd0` + `/dev/ums0`). A new consumer module, **per-display instance**, registers against the (post-T8) instance-scoped `console` API:

```
VNC client bytes
  └─> rfb_recv_key_msg (rfb.c:899)               [unchanged]
        └─> console_key_event (rfb.c:909)         [unchanged]
              └─> console_kbd_event_dispatch (console.c:106)   [refactored: per-instance]
                    └─> displayd_jail_kbd_event(arg, down, sym, code)  [NEW, per-jail]
                          ├─> write to jail's /dev/kbd0 input ring
                          │   or
                          ├─> enqueue into displayd.ko's per-jail shm buffer
                          └─> wake up jail reader (kqueue / select / blocking read)
```

The **transport** (rfb.c) and **dispatcher** (console.c) stay the same; the **consumer** (`ps2kbd_event` / `umouse_event`) is replaced or augmented with a `displayd_jail_kbd_event` for the jail-attached case. T8's refactor must make `console_kbd_register` and the dispatchers **per-instance** so that a bhyve VM and a jail can coexist with their own consumers. T12's `displayd.ko` provides the kernel-side receiver (`/dev/kbd0` reader-side is the jail; writer-side is the new consumer).

**For amd64 bhyve:** keep the existing `ps2kbd_event` / `ps2mouse_event` / `umouse_event` paths working unchanged. The new displayd consumer is a fourth registrant (per-instance), not a replacement.

### 6.4 Compatibility constraint (T8 / T12 must enforce)

If `displayd.ko`'s jail-attached consumer is registered for **one** instance, and a bhyve VM is running in **another** instance, the two must NOT share state. Today's `console.c` is file-static (singleton) — T8 must replace the file-static state with per-instance state hung off an opaque handle. T11 (rfb wrap) and T13 (pci_fbuf wire) must thread the instance handle through to rfb.c's `console_key_event` / `console_ptr_event` calls.

## 7. Function table (the per-consumer dispatch surface)

| Consumer | File:line | Argument `arg` | Delivery to "guest" | Architecture |
|---|---|---|---|---|
| `ps2kbd_event` | `usr.sbin/bhyve/amd64/ps2kbd.c:394` | `struct ps2kbd_softc *` | `atkbdc_event(sc, 1)` → i8042 IRQ 1 | amd64 only |
| `ps2mouse_event` | `usr.sbin/bhyve/amd64/ps2mouse.c:372` | `struct ps2mouse_softc *` | `atkbdc_event(sc, 0)` → i8042 IRQ 12 | amd64 only |
| `umouse_event` | `usr.sbin/bhyve/usb_mouse.c:257` | `struct umouse_softc *` | `hci->hci_intr(..., UMOUSE_INTR_ENDPT)` → USB endpoint interrupt | all arch (registered by usb_mouse init) |

Note: `usb_mouse_init` (the per-emulated-device init) calls `console_ptr_register(umouse_event, sc, 10)` at `usb_mouse.c:317`. But `usb_mouse_init` is invoked by the USB stack's per-device attach (`umouse_init` at `usb_mouse.c:309` is the `init` method of the umouse template). The umouse template is registered for the XHCI/EHCI emulated controllers, which are arch-independent in source. The reason it's "amd64 in practice" today is that bhyve on aarch64 and riscv has no PS/2 fallback — guests must use virtio-input or USB-HID with a USB HCI, but bhyve on those arches doesn't currently ship a working emulated HCI. (Out of scope for T2, but flagged for the multi-arch workstream.)

## 8. Constraints and extension points

### Constraints

1. **The `console` module is a file-static singleton** (`console.c:34-47`). All registrations are process-scoped. **Multi-instance is impossible without T8's refactor.** A jail-attached consumer therefore cannot coexist with a bhyve-VM-attached consumer today.
2. **Priority comparison is `>` (strict), not `>=`.** Two registrants at the same priority → the first one wins; the second is silently dropped. (`console.c:88,98`.)
3. **No `console_*_unregister` exists.** The consumers register once at bhyve startup and never unregister. T8 must add unregister.
4. **No input fan-out.** Only the highest-priority callback runs. There is no way to deliver the same key to both a jail consumer and a VM consumer from a single source — by design, but a problem for the broker workstream (T38) if the broker wants to forward input to a jail-fb AND a remote viewer.
5. **The "input_detected" flag (rfb.c:910,933,950) is set on every key/ptr event.** This is a global "did anything ever happen" flag — `rc->input_detected = true`. It's set on the per-client `struct rfb_softc`, not the global one. It causes the RFB server to start advertising a "non-empty" desktop to the client; otherwise the client sees "no activity" and may suspend its frame requests. This is **VNC protocol semantics, not input fan-out** — flagged because T11's rfb wrap must preserve it.
6. **Keysym translation happens inside `ps2kbd_event` (ps2kbd.c:405) → `ps2kbd_keysym_queue` (ps2kbd.c:390).** A future broker-side consumer that wants raw RFB keysyms (rather than PS/2 scancodes) would need a different path. T11 / T38 should be aware that the keysym → scancode step is **bhyve-VM-specific** and would not be needed (or would be different) for a jail consumer.

### Extension points

1. **`console_kbd_register` / `console_ptr_register` are the natural chokepoints to make per-instance.** T8 wraps them in an instance-aware vtable; the per-instance dispatcher in `console_key_event` / `console_ptr_event` iterates the instance's consumer list (or calls the single highest-priority consumer, same as today, with the instance chosen by the transport).
2. **`ps2kbd_keysym_queue` / `ps2mouse_movement_get` are pure data transformation.** They produce a scancode/byte sequence from a keysym/coords. A jail consumer can reuse them unchanged (e.g., to put a PS/2 scancode into a per-jail buffer that the jail's `kbd` driver reads). Or the jail consumer can use a different format (e.g., raw keysym + UTF-8 char) and skip the translation.
3. **The `arg` parameter in `console_kbd_register` / `console_ptr_register` is the per-consumer opaque handle.** For jail consumers this would be a `struct displayd_jail_fb *` (or similar). The per-instance refactor (T8) extends this to include the instance pointer.
4. **The transport side (rfb.c) is unchanged.** The keysym → `console_key_event` call site (rfb.c:909,932) does not need to know whether the consumer is a PS/2 device or a jail. This is the **cleanest extension point**: the transport and the dispatcher are arch-agnostic; only the consumer changes.

## 9. Recommendations for downstream tasks

- **T4 (transport vtable)** can proceed unchanged. The transport's job is to call `console_key_event` / `console_ptr_event`; the vtable wraps that.
- **T5 (backend vtable)** can be designed against the new `displayd_jail_kbd_event` consumer prototype. The backend's `notify_key` / `notify_ptr` methods would call the same `console_key_event` / `console_ptr_event` API, with the consumer being a jail-attached instance.
- **T7 (transport registry)** does not need to change the dispatcher.
- **T8 (console refactor)** must: (1) move file-static state to per-instance, (2) add `console_create` / `console_destroy` / `console_*_unregister` API, (3) change the single-callback dispatch to per-instance dispatch (still single-consumer-per-instance is fine — multi-consumer is a T2+future concern).
- **T11 (rfb wrap)** preserves the 3 dispatch sites (rfb.c:909,932,949) and the `input_detected` semantics. The wrap registers rfb as a `struct display_transport` instance; the `console_key_event` / `console_ptr_event` calls remain.
- **T12 (displayd.ko)** provides `/dev/fb0` + `/dev/kbd0` + `/dev/ums0` in the jail. The userland-side new consumer (`displayd_consumer.c` or similar) registers `displayd_jail_kbd_event` / `displayd_jail_ptr_event` against the (per-instance) console. It reads the per-jail `arg` (a `struct displayd_jail_fb *`) and writes to the per-jail input ring.
- **T13 (pci_fbuf wire)** does not need to touch input fan-out. Its job is to register rfb as a transport and call `console_init` per VM. The input side works as today.
- **T38 (broker daemon)** will need a way to deliver input to a jail consumer. If the broker is the "transport" (BDP) and the jail consumer is the displayd module, the data flow is: BDP client → broker → displayd_consumer → displayd.ko → `/dev/kbd0` / `/dev/ums0` in the jail. This is a T38 design concern, not a T2 concern.

## 10. Verdict (the one-line answer)

> **Existing bhyve path is sufficient for amd64 bhyve VMs (PS/2 + USB mouse), and arch-agnostic at the transport + dispatcher level, but is INSUFFICIENT for jails: the consumer side (PS/2 i8042, USB HCI) is VM-specific. T8's per-instance console refactor + T12's displayd module + a new per-jail consumer registered against the instance are required to extend input to jails. The transport and dispatcher layers do not need to change.**
