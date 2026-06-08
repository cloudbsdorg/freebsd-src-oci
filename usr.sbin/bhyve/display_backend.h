/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The FreeBSD Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _DISPLAY_BACKEND_H_
#define	_DISPLAY_BACKEND_H_

#include <sys/types.h>

/*
 * display_backend -- pluggable framebuffer-producer abstraction.
 *
 * A backend owns the framebuffer memory (or a view into it) and the
 * input-injection path.  Today, the only backend is pci_fbuf: a VMM-
 * attached PCI framebuffer device that maps guest-painted pixels into
 * a vm_create_devmem() region.  The future displayd_backend will own a
 * mmapped view of a jail-attached /dev/fb0 plus a cdev-write path for
 * kbd/ptr input.
 *
 * Pairing with display_transport:
 *   - The bhyve process owns one or more (transport, backend) pairs.
 *   - The bridge in console.c (or its post-T8 per-instance successor)
 *     connects the two: the backend's fb is wrapped in a bhyvegc and
 *     handed to the transport via display_transport_attach().
 *   - For a bhyve VM: backend = pci_fbuf_backend, transport = rfb.
 *   - For a jail-attached display: backend = displayd_backend,
 *     transport = (rfb for a remote viewer, or BDP for the broker).
 *
 * The vtable below is small by design.  It does NOT include VM-specific
 * ops (vm_inject_irq, vm_isa_pulse_irq) -- those live in the pci_fbuf
 * implementation, not in the abstraction.  The abstraction is "this is
 * the framebuffer and here is how to send input to the consumer", not
 * "this is a VM".
 */

struct display_backend;

/*
 * Opaque handle.  One per backend instance (one per bhyve VM, or one
 * per jail-attached display).
 */
typedef struct display_backend *display_backend_t;

/*
 * Framebuffer metadata returned by get_fb_info.  The consumer (bhyvegc
 * or its per-instance successor) reads width/height/data and presents
 * them to a transport via display_transport_attach().
 *
 * `data` is owned by the backend.  The consumer MUST NOT free it.
 * The pointer is valid until the backend is destroyed (or until the
 * backend calls display_backend_fb_invalidate(), if the backend
 * supports that -- today neither pci_fbuf nor the planned displayd
 * do; a future multi-display backend may).
 */
struct display_fb_info {
	int	 width;
	int	 height;
	int	 stride;	/* bytes per row; >= width * 4 */
	const void *data;	/* RGBA32, row-major; backend-owned */
};

/*
 * The vtable.
 *
 * init:        required.  Per-instance setup.  The backend is free to
 *              read its args (e.g. a backend name + a per-instance
 *              config blob) here.
 * destroy:     required.  Release backend-owned memory, close fds.
 *              Idempotent.
 * get_fb_info: required.  Return a pointer to backend-owned fb info.
 *              The pointer is valid until destroy() is called.
 * refresh:    optional.  Force a re-render of the backend's framebuffer
 *              (e.g. after the bhyvegc is resized).  A passive backend
 *              (one that just mmaps a /dev/fb0) may leave this NULL --
 *              the framebuffer updates lazily as the producer writes.
 * notify_key:  required.  The bhyve-side consumer (or a transport
 *              calling back) hands a kbd event to the backend; the
 *              backend delivers it to the guest (pci_fbuf) or to the
 *              jail's displayd kbd (displayd).
 * notify_ptr:  required.  Same for pointer events.
 */
struct display_backend {
	const char	*name;

	/* Required.  Per-instance setup. */
	int	(*init)(const char *args, display_backend_t *handle_out);

	/* Required.  Release per-instance resources.  Idempotent. */
	void	(*destroy)(display_backend_t handle);

	/* Required.  Return the backend's framebuffer info.  Pointer
	 * remains valid until destroy() is called.  Returns NULL if
	 * the backend has no fb (e.g. an audio-only backend, future). */
	const struct display_fb_info *(*get_fb_info)(display_backend_t handle);

	/* Optional.  Force a re-render.  NULL is a no-op. */
	void	(*refresh)(display_backend_t handle);

	/* Required.  Deliver a keyboard event to the guest / jail.
	 * Returns 0 on success, errno on failure. */
	int	(*notify_key)(display_backend_t handle,
	    int down, uint32_t keysym, uint32_t keycode);

	/* Required.  Deliver a pointer event to the guest / jail. */
	int	(*notify_ptr)(display_backend_t handle,
	    uint8_t button, int x, int y);
};

/*
 * Registry API.  Mirrors the display_transport registry in shape and
 * size limit.  Adding a new backend is two steps:
 *   1. Declare `static struct display_backend my_backend = { ... };`
 *      in the backend's .c file.
 *   2. Call display_backend_register(&my_backend) at process start.
 */
#define	DISPLAY_BACKEND_MAX	8

int	 display_backend_register(const struct display_backend *b);

int	 display_backend_init_by_name(const char *name, const char *args,
	    display_backend_t *handle_out);

void	 display_backend_destroy(display_backend_t handle);

void	 display_backend_destroy_all(void);

void	 display_backend_list_names(int (*pr)(const char *fmt, ...));

/*
 * The existing pci_fbuf module will become the first backend.  The
 * pci_fbuf_init() entry point is preserved as a thin wrapper that
 * calls display_backend_init_by_name("pci_fbuf", ...).  The legacy
 * `bhyve -s 0,fbuf,...` config key is preserved by the wrapper.
 *
 * The future displayd_backend (T12) will register itself as the
 * "displayd" backend.  Its notify_key / notify_ptr ops will write
 * into the jail's /dev/kbd0 / /dev/ums0 instead of calling
 * vm_isa_pulse_irq.
 */

#endif /* _DISPLAY_BACKEND_H_ */
