/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2015 Tycho Nightingale <tycho.nightingale@pluribusnetworks.com>
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

#include <sys/types.h>
#include <sys/queue.h>

#include <errno.h>
#include <stdlib.h>

#include "bhyvegc.h"
#include "console.h"

/*
 * T8.B: per-instance console core + 9 singleton shims.  Non-locking
 * (concurrency is a separate ticket).
 *
 * Lifetime:
 *   console_create()  -> malloc + bhyvegc_init, STAILQ_INSERT_TAIL.
 *   console_destroy() -> unlink, bhyvegc_destroy, free.  Idempotent on NULL.
 *   console_init()    -> the legacy shim: if default_ctx is NULL, create one
 *                        with CONSOLE_FB_BHYVEGC; else if fbaddr differs from
 *                        the current image, set_fbaddr.  Idempotent on
 *                        re-init.
 *
 * Single-consumer-per-instance: ctx->fb / ctx->kbd / ctx->ptr hold one
 * (cb, arg, pri) each.  Register replaces when new_pri > old_pri; the
 * consumer then never gets fan-out.  Unregister by cb pointer returns 0
 * on hit, ENOENT on miss, EINVAL on NULL ctx.  This matches the
 * pre-T8 single-cb semantics in usr.sbin/bhyve/console.c:88,98.
 */

struct console_consumer {
	STAILQ_ENTRY(console_consumer) link;
	void			*cb;
	void			*arg;
	int			 pri;
};

struct console_ctx {
	struct bhyvegc		*gc;
	STAILQ_ENTRY(console_ctx) link;
	int			 flags;
	int			 w;
	int			 h;
	struct console_consumer	 fb;
	struct console_consumer	 kbd;
	struct console_consumer	 ptr;
};

static struct console_ctx	*default_ctx;
static STAILQ_HEAD(, console_ctx) instances =
    STAILQ_HEAD_INITIALIZER(instances);

struct console_ctx *
console_create(int w, int h, void *fbaddr, int flags)
{
	struct console_ctx *ctx;

	if (w <= 0 || h <= 0)
		return (NULL);

	ctx = calloc(1, sizeof(*ctx));
	if (ctx == NULL)
		return (NULL);

	ctx->flags = flags;
	ctx->w = w;
	ctx->h = h;

	/* CONSOLE_FB_RAW means the caller owns the framebuffer (e.g.
	 * the displayd module mmap'd /dev/fb0 from a jail).  In that
	 * mode we require a non-NULL fbaddr; we do NOT call bhyvegc_init
	 * (it would calloc a throwaway buffer).  This is the path the
	 * T8.C test tc_console_create_with_null_fb_raw_fails covers. */
	if ((flags & CONSOLE_FB_RAW) && fbaddr == NULL) {
		free(ctx);
		return (NULL);
	}

	/* Default path (and the "raw but with fb" path): bhyvegc_init
	 * handles fbaddr!=NULL (raw, caller owns) vs NULL (calloc'd,
	 * bhyvegc owns). */
	ctx->gc = bhyvegc_init(w, h, fbaddr);
	if (ctx->gc == NULL) {
		free(ctx);
		return (NULL);
	}

	STAILQ_INSERT_TAIL(&instances, ctx, link);

	if (default_ctx == NULL)
		default_ctx = ctx;

	return (ctx);
}

void
console_destroy(struct console_ctx *ctx)
{
	struct console_ctx *prev, *cur;

	if (ctx == NULL)
		return;

	/* STAILQ_REMOVE on the tail mishandles stqh_last; do it by hand. */
	prev = NULL;
	STAILQ_FOREACH(cur, &instances, link) {
		if (cur == ctx)
			break;
		prev = cur;
	}
	if (cur == NULL)
		return;			/* not in the list; already destroyed */

	if (prev == NULL)
		STAILQ_REMOVE_HEAD(&instances, link);
	else {
		if (STAILQ_NEXT(cur, link) == NULL)
			instances.stqh_last = &prev->link.stqe_next;
		prev->link.stqe_next = STAILQ_NEXT(cur, link);
	}

	if (default_ctx == ctx)
		default_ctx = NULL;

	if (ctx->gc != NULL)
		bhyvegc_destroy(ctx->gc);

	free(ctx);
}

struct console_ctx *
console_get_default(void)
{

	return (default_ctx);
}

/*
 * Return non-zero if `ctx` is currently in the live instances list.
 * Used by every per-instance op to defend against use-after-destroy
 * (the T8.C test tc_console_get_image_returns_null_after_destroy
 * covers this).  Linear scan is fine: instances is a small list.
 */
static int
ctx_is_live(const struct console_ctx *ctx)
{
	const struct console_ctx *cur;

	if (ctx == NULL)
		return (0);
	STAILQ_FOREACH(cur, &instances, link) {
		if (cur == ctx)
			return (1);
	}
	return (0);
}

void
console_set_fbaddr_ctx(struct console_ctx *ctx, void *fbaddr)
{

	if (!ctx_is_live(ctx) || ctx->gc == NULL)
		return;
	bhyvegc_set_fbaddr(ctx->gc, fbaddr);
}

struct bhyvegc_image *
console_get_image_ctx(struct console_ctx *ctx)
{

	if (!ctx_is_live(ctx) || ctx->gc == NULL)
		return (NULL);
	return (bhyvegc_get_image(ctx->gc));
}

void
console_fb_register_ctx(struct console_ctx *ctx, fb_render_func_t render_cb,
    void *arg, int pri)
{

	if (!ctx_is_live(ctx))
		return;
	if (pri > ctx->fb.pri) {
		ctx->fb.cb = render_cb;
		ctx->fb.arg = arg;
		ctx->fb.pri = pri;
	}
}

int
console_fb_unregister_ctx(struct console_ctx *ctx, fb_render_func_t render_cb)
{

	if (!ctx_is_live(ctx))
		return (EINVAL);
	if ((fb_render_func_t)ctx->fb.cb == render_cb) {
		ctx->fb.cb = NULL;
		ctx->fb.arg = NULL;
		ctx->fb.pri = 0;
		return (0);
	}
	return (ENOENT);
}

void
console_refresh_ctx(struct console_ctx *ctx)
{

	if (!ctx_is_live(ctx) || ctx->fb.cb == NULL)
		return;
	(*(fb_render_func_t)ctx->fb.cb)(ctx->gc, ctx->fb.arg);
}

void
console_kbd_register_ctx(struct console_ctx *ctx, kbd_event_func_t event_cb,
    void *arg, int pri)
{

	if (!ctx_is_live(ctx))
		return;
	if (pri > ctx->kbd.pri) {
		ctx->kbd.cb = event_cb;
		ctx->kbd.arg = arg;
		ctx->kbd.pri = pri;
	}
}

int
console_kbd_unregister_ctx(struct console_ctx *ctx, kbd_event_func_t event_cb)
{

	if (!ctx_is_live(ctx))
		return (EINVAL);
	if ((kbd_event_func_t)ctx->kbd.cb == event_cb) {
		ctx->kbd.cb = NULL;
		ctx->kbd.arg = NULL;
		ctx->kbd.pri = 0;
		return (0);
	}
	return (ENOENT);
}

void
console_key_event_ctx(struct console_ctx *ctx, int down, uint32_t keysym,
    uint32_t keycode)
{

	if (!ctx_is_live(ctx) || ctx->kbd.cb == NULL)
		return;
	(*(kbd_event_func_t)ctx->kbd.cb)(down, keysym, keycode, ctx->kbd.arg);
}

void
console_ptr_register_ctx(struct console_ctx *ctx, ptr_event_func_t event_cb,
    void *arg, int pri)
{

	if (!ctx_is_live(ctx))
		return;
	if (pri > ctx->ptr.pri) {
		ctx->ptr.cb = event_cb;
		ctx->ptr.arg = arg;
		ctx->ptr.pri = pri;
	}
}

int
console_ptr_unregister_ctx(struct console_ctx *ctx, ptr_event_func_t event_cb)
{

	if (!ctx_is_live(ctx))
		return (EINVAL);
	if ((ptr_event_func_t)ctx->ptr.cb == event_cb) {
		ctx->ptr.cb = NULL;
		ctx->ptr.arg = NULL;
		ctx->ptr.pri = 0;
		return (0);
	}
	return (ENOENT);
}

void
console_ptr_event_ctx(struct console_ctx *ctx, uint8_t button, int x, int y)
{

	if (!ctx_is_live(ctx) || ctx->ptr.cb == NULL)
		return;
	(*(ptr_event_func_t)ctx->ptr.cb)(button, x, y, ctx->ptr.arg);
}

/* ------------------------------------------------------------------ */
/* Legacy singleton shims.  The 14 in-tree call sites (pci_fbuf, rfb,  */
/* usb_mouse, ps2kbd, ps2mouse, vga) are unchanged and call these.     */
/* Each reads default_ctx at call-time, so the shims work after a      */
/* console_destroy(default_ctx) + console_create() round trip.          */
/* ------------------------------------------------------------------ */

void
console_init(int w, int h, void *fbaddr)
{
	struct bhyvegc_image *img;

	if (default_ctx == NULL) {
		default_ctx = console_create(w, h, fbaddr, CONSOLE_FB_BHYVEGC);
		return;
	}
	img = console_get_image_ctx(default_ctx);
	if (img != NULL && img->data != fbaddr)
		console_set_fbaddr_ctx(default_ctx, fbaddr);
}

void
console_set_fbaddr(void *fbaddr)
{

	if (default_ctx != NULL)
		console_set_fbaddr_ctx(default_ctx, fbaddr);
}

struct bhyvegc_image *
console_get_image(void)
{

	return (default_ctx != NULL ? console_get_image_ctx(default_ctx) : NULL);
}

void
console_fb_register(fb_render_func_t render_cb, void *arg)
{

	if (default_ctx != NULL)
		console_fb_register_ctx(default_ctx, render_cb, arg, 0);
}

void
console_refresh(void)
{

	if (default_ctx != NULL)
		console_refresh_ctx(default_ctx);
}

void
console_kbd_register(kbd_event_func_t event_cb, void *arg, int pri)
{

	if (default_ctx != NULL)
		console_kbd_register_ctx(default_ctx, event_cb, arg, pri);
}

void
console_key_event(int down, uint32_t keysym, uint32_t keycode)
{

	if (default_ctx != NULL)
		console_key_event_ctx(default_ctx, down, keysym, keycode);
}

void
console_ptr_register(ptr_event_func_t event_cb, void *arg, int pri)
{

	if (default_ctx != NULL)
		console_ptr_register_ctx(default_ctx, event_cb, arg, pri);
}

void
console_ptr_event(uint8_t button, int x, int y)
{

	if (default_ctx != NULL)
		console_ptr_event_ctx(default_ctx, button, x, y);
}
