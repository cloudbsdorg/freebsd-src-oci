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

#ifndef _DISPLAY_TRANSPORT_H_
#define	_DISPLAY_TRANSPORT_H_

#include <sys/socket.h>
#include <sys/types.h>

/*
 * display_transport -- pluggable wire-protocol abstraction for bhyve's
 * framebuffer output and client-side input.
 *
 * Today, bhyve hard-codes RFB (VNC) via rfb_init() at usr.sbin/bhyve/pci_fbuf.c:455.
 * The display_transport vtable lets new transports (RDP, BDP, multicast UDP,
 * a future local-only jail consumer) register themselves and be looked up by
 * name at bhyve config-parse time.
 *
 * Lifetime:
 *   1. A static struct display_transport is declared in the transport's
 *      compilation unit (e.g. usr.sbin/bhyve/rfb.c).
 *   2. pci_fbuf (or a future bhyve displayd consumer) calls
 *      display_transport_init_by_name("rfb", args, &handle) at instance
 *      creation.  The registry finds the static struct, calls its init op,
 *      and returns an opaque handle.
 *   3. The consumer calls dt_send_pixels / dt_send_input via the handle.
 *   4. On bhyve shutdown (or displayd consumer exit), the consumer calls
 *      display_transport_shutdown(handle).
 *
 * The init signature is intentionally close to the existing rfb_init:
 *   rfb_init(sa_family_t family, const char *hostname, int port, int wait,
 *            const char *password)
 * so that the rfb_transport's init op can be a near-drop-in.  New transports
 * extend the args via a key/value list (the plan's "transport=rfb,...,
 * foo=bar" syntax).
 *
 * Threading: a transport may spawn its own threads (rfb does).  The transport
 * is responsible for its own thread lifecycle.  The bhyve process is single-
 * threaded for VM execution; transport threads run in parallel.
 */

struct display_transport;

/*
 * Opaque handle returned by display_transport_init_by_name.  Carries the
 * transport instance state (open sockets, per-client state, threads).
 * Transport-specific casts inside the transport's .c file.
 */
typedef struct display_transport *display_transport_t;

/*
 * Pixel format handed to a transport on attach.  All fields are host-endian.
 * Today the only supported format is 32-bit RGBA (the bhyvegc_image default).
 * A future transport (BDP) may extend this with a pixel_format enum.
 */
struct display_pixels {
	int	 width;		/* width in pixels */
	int	 height;	/* height in pixels */
	int	 stride;	/* bytes per row (>= width * 4) */
	const uint32_t *data;	/* pixel storage; row-major, RGBA32 */
};

/*
 * Input event handed from a transport to the bhyve-side consumer.  Mirrors
 * the existing console_key_event / console_ptr_event signatures.
 */
struct display_key_event {
	int	 down;		/* 1 = key down, 0 = key up */
	uint32_t keysym;	/* RFB keysym (X11 keysym set) */
	uint32_t keycode;	/* raw keycode (e.g. evdev) when available */
};

struct display_ptr_event {
	uint8_t button;		/* bit 0 = left, 1 = middle, 2 = right */
	int	 x;		/* X coordinate, 0..width-1 */
	int	 y;		/* Y coordinate, 0..height-1 */
};

/*
 * The vtable.
 *
 * name:        required; used by the registry for lookup.
 * init:        required; parse args, open sockets/threads, return handle.
 * shutdown:    required; tear down.  Idempotent.
 * attach:      required; bind a framebuffer.  May start per-client threads.
 * detach:      required; unbind.  Must stop sending pixel updates.
 * send_pixels: required; encode and send a pixel rect.
 * send_input:  optional; bhyve -> transport forward of a key event.
 * send_input_ptr: optional; bhyve -> transport forward of a pointer event.
 * on_client_event: optional; transport -> consumer notification.
 */
struct display_transport {
	const char	*name;

	/* Required.  Parse `args` (a comma-separated key=value list) and
	 * open the transport's sockets / threads.  Returns 0 on success,
	 * an errno on failure.  On success, *handle_out is set to the new
	 * instance. */
	int	(*init)(const char *args, display_transport_t *handle_out);

	/* Required.  Tear down the transport, close sockets, join threads,
	 * free per-instance state.  Idempotent. */
	void	(*shutdown)(display_transport_t handle);

	/* Required.  Bind a framebuffer to the transport.  The transport
	 * may begin sending pixel updates for the attached fb.  Returns 0
	 * on success, errno on failure (e.g. transport not yet init'd). */
	int	(*attach)(display_transport_t handle,
	    const struct display_pixels *fb);

	/* Required.  Unbind the framebuffer.  The transport must stop
	 * sending pixel updates and drop any in-flight state. */
	void	(*detach)(display_transport_t handle);

	/* Required.  Encode and send a pixel rect to the transport's
	 * clients.  Coordinates are absolute in the attached fb.  Returns
	 * 0 on success; the transport is responsible for rate-limiting
	 * via the security.display.* sysctls (see the plan's Tunables
	 * Reference). */
	int	(*send_pixels)(display_transport_t handle,
	    int x, int y, int w, int h);

	/* Optional.  The bhyve-side consumer (e.g. pci_fbuf, displayd_consumer)
	 * can call this to forward an input event into the transport's
	 * client-input pipeline.  For a passive transport (e.g. a one-way
	 * multicast) this is a no-op. */
	int	(*send_input)(display_transport_t handle,
	    const struct display_key_event *key);
	int	(*send_input_ptr)(display_transport_t handle,
	    const struct display_ptr_event *ptr);

	/* Optional.  The transport calls this on client attach / detach
	 * / input_detected so the consumer (or a future broker) can log
	 * or update a registry.  Returns 0 on success, errno on failure
	 * (e.g. consumer rejects the attach). */
	int	(*on_client_event)(display_transport_t handle, int event,
	    int client_id);
};

/*
 * Registry API.  The registry is a fixed-size array (the plan's design
 * notes cap it at 8 transports; the constant is here so the limit is
 * one place).  Adding a new transport is two steps:
 *   1. Declare `static struct display_transport my_transport = { ... };`
 *      in the transport's .c file.
 *   2. Call display_transport_register(&my_transport) at process start,
 *      or rely on a linker_set to do so automatically.
 */
#define	DISPLAY_TRANSPORT_MAX	8

/*
 * Register a transport.  Idempotent (re-registering the same .name is a
 * no-op).  Returns 0 on success, ENOMEM if the registry is full.
 */
int	 display_transport_register(const struct display_transport *t);

/*
 * Look up a transport by name and create an instance.  Parses the args
 * string (comma-separated key=value pairs) and hands the result to the
 * transport's init op.  On success, *handle_out is set.
 *
 * Returns 0 on success, ENOENT if no transport is registered with the
 * given name, or whatever the init op returned.
 */
int	 display_transport_init_by_name(const char *name, const char *args,
	    display_transport_t *handle_out);

/*
 * Convenience: shut down a handle and look up the transport's shutdown op
 * by handle.  The handle is invalidated on return.
 */
void	 display_transport_shutdown(display_transport_t handle);

/*
 * Shut down all active transport instances.  Called at bhyve process exit
 * (and on signal).  Idempotent.
 */
void	 display_transport_shutdown_all(void);

/*
 * For diagnostics: list the names of all registered transports.  Output
 * goes to the supplied printf-like callback, one name per call.
 */
void	 display_transport_list_names(int (*pr)(const char *fmt, ...));

/*
 * Backward-compatibility shim.  The existing rfb_init() entry point is
 * preserved as a thin wrapper that calls display_transport_init_by_name
 * with the "rfb" transport and a synthesized args string.  This keeps
 * legacy bhyve config files (rfb=...) working unchanged.  The shim lives
 * in rfb.c, not here.
 */

#endif /* _DISPLAY_TRANSPORT_H_ */
