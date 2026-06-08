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

#ifndef _CONSOLE_H_
#define	_CONSOLE_H_

struct bhyvegc;

typedef void (*fb_render_func_t)(struct bhyvegc *gc, void *arg);
typedef void (*kbd_event_func_t)(int down, uint32_t keysym, uint32_t keycode, void *arg);
typedef void (*ptr_event_func_t)(uint8_t mask, int x, int y, void *arg);

#include <sys/queue.h>

struct console_ctx;

#define CONSOLE_FB_RAW    0x0001  /* skip bhyvegc_init (jail path) */
#define CONSOLE_FB_BHYVEGC 0x0000  /* default: bhyvegc enabled */

struct console_ctx *console_create(int w, int h, void *fbaddr, int flags);
void                console_destroy(struct console_ctx *ctx);
struct console_ctx *console_get_default(void);

void                console_set_fbaddr_ctx(struct console_ctx *ctx, void *fbaddr);
struct bhyvegc_image *console_get_image_ctx(struct console_ctx *ctx);

void                console_fb_register_ctx(struct console_ctx *ctx,
                              fb_render_func_t render_cb, void *arg, int pri);
int                 console_fb_unregister_ctx(struct console_ctx *ctx,
                                fb_render_func_t render_cb);
void                console_refresh_ctx(struct console_ctx *ctx);

void                console_kbd_register_ctx(struct console_ctx *ctx,
                               kbd_event_func_t event_cb, void *arg, int pri);
int                 console_kbd_unregister_ctx(struct console_ctx *ctx,
                                 kbd_event_func_t event_cb);
void                console_key_event_ctx(struct console_ctx *ctx,
                             int down, uint32_t keysym, uint32_t keycode);

void                console_ptr_register_ctx(struct console_ctx *ctx,
                               ptr_event_func_t event_cb, void *arg, int pri);
int                 console_ptr_unregister_ctx(struct console_ctx *ctx,
                                 ptr_event_func_t event_cb);
void                console_ptr_event_ctx(struct console_ctx *ctx,
                             uint8_t button, int x, int y);

void console_init(int w, int h, void *fbaddr);

void console_set_fbaddr(void *fbaddr);

struct bhyvegc_image *console_get_image(void);

void console_fb_register(fb_render_func_t render_cb, void *arg);
void console_refresh(void);

void console_kbd_register(kbd_event_func_t event_cb, void *arg, int pri);
void console_key_event(int down, uint32_t keysym, uint32_t keycode);

void console_ptr_register(ptr_event_func_t event_cb, void *arg, int pri);
void console_ptr_event(uint8_t button, int x, int y);

#endif /* _CONSOLE_H_ */
