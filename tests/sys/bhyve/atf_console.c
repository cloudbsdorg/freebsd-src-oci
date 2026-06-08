/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 displayd-reconcile-wave1 contributors
 */

/*
 * ATF C test suite for the per-instance console API (T8.C).
 *
 * Depends on T8.A symbols in usr.sbin/bhyve/console.h (landed at
 * commit 47db57117e3):
 *   struct console_ctx, CONSOLE_FB_RAW, CONSOLE_FB_BHYVEGC,
 *   console_create/destroy/get_default,
 *   console_set_fbaddr_ctx/get_image_ctx,
 *   console_fb_{register,unregister,refresh}_ctx,
 *   console_kbd_{register,unregister}_ctx, console_key_event_ctx,
 *   console_ptr_{register,unregister}_ctx, console_ptr_event_ctx.
 *
 * The T8.A functions are declared in console.h but the corresponding
 * implementations land in T8.B. The test source therefore compiles
 * cleanly against the new header, but the link step fails with
 * "undefined reference to console_create" etc. until T8.B is committed.
 * Each test is decorated with "expected_failure" so Kyua reports the
 * result as XPASS/XFAIL until T8.B lands. Drop the EXPECT_T8B_FAIL(tc)
 * call from each HEAD to flip the suite green.
 *
 * TDD status: RED. Compile is green; link is red. See
 * .sisyphus/evidence/task-8-atf-compile.txt for the captured output.
 */

#include <atf-c.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "bhyvegc.h"
#include "console.h"

#define	TEST_W	64
#define	TEST_H	64
#define	FB_SIZE	(TEST_W * TEST_H)

struct capture {
	int	called;
	int	down;
	uint32_t keysym;
	uint32_t keycode;
	uint8_t	mask;
	int	x;
	int	y;
	void	*passed_arg;
};

static void
kbd_capture(int down, uint32_t keysym, uint32_t keycode, void *arg)
{
	struct capture *c = arg;
	c->called++; c->down = down;
	c->keysym = keysym; c->keycode = keycode;
	c->passed_arg = arg;
}

static void
ptr_capture(uint8_t mask, int x, int y, void *arg)
{
	struct capture *c = arg;
	c->called++; c->mask = mask;
	c->x = x; c->y = y;
	c->passed_arg = arg;
}

static void
noop_fb_render(struct bhyvegc *gc, void *arg)
{
	(void)gc; (void)arg;
}

/*
 * T8.B is now landed; the suite is GREEN.  The EXPECT_T8B_FAIL
 * shim is no longer needed and all its call sites have been
 * deleted.
 */

ATF_TC(tc_console_create_with_provided_fb);
ATF_TC_HEAD(tc_console_create_with_provided_fb, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "console_create with explicit fb returns a context whose image "
	    "exposes the supplied width, height, and pixel data pointer");
}
ATF_TC_BODY(tc_console_create_with_provided_fb, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	struct bhyvegc_image *img;
	memset(fb, 0, sizeof(fb));
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	img = console_get_image_ctx(ctx);
	ATF_REQUIRE(img != NULL);
	ATF_CHECK_EQ(img->width, TEST_W);
	ATF_CHECK_EQ(img->height, TEST_H);
	ATF_CHECK_EQ(img->data, fb);
	console_destroy(ctx);
}

ATF_TC(tc_console_create_with_null_fb_raw_fails);
ATF_TC_HEAD(tc_console_create_with_null_fb_raw_fails, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "console_create with NULL fb and CONSOLE_FB_RAW returns NULL");
}
ATF_TC_BODY(tc_console_create_with_null_fb_raw_fails, tc)
{
	struct console_ctx *ctx;
	ctx = console_create(TEST_W, TEST_H, NULL, CONSOLE_FB_RAW);
	ATF_CHECK(ctx == NULL);
}

ATF_TC(tc_console_create_with_bhyvegc_default);
ATF_TC_HEAD(tc_console_create_with_bhyvegc_default, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "console_create with NULL fb and the bhyvegc default flavour "
	    "allocates its own back-buffer and exposes a non-NULL image");
}
ATF_TC_BODY(tc_console_create_with_bhyvegc_default, tc)
{
	struct console_ctx *ctx;
	struct bhyvegc_image *img;
	ctx = console_create(TEST_W, TEST_H, NULL, 0);
	ATF_REQUIRE(ctx != NULL);
	img = console_get_image_ctx(ctx);
	ATF_CHECK(img != NULL);
	ATF_CHECK_EQ(img->width, TEST_W);
	ATF_CHECK_EQ(img->height, TEST_H);
	ATF_CHECK(img->data != NULL);
	console_destroy(ctx);
}

ATF_TC(tc_console_destroy_idempotent);
ATF_TC_HEAD(tc_console_destroy_idempotent, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Calling console_destroy twice on the same context is safe "
	    "and does not trip the address sanitizer");
}
ATF_TC_BODY(tc_console_destroy_idempotent, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	console_destroy(ctx);
	console_destroy(ctx);	/* must be a no-op, not a double-free */
}

ATF_TC(tc_console_destroy_null_safe);
ATF_TC_HEAD(tc_console_destroy_null_safe, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "console_destroy(NULL) is a no-op and must not crash");
}
ATF_TC_BODY(tc_console_destroy_null_safe, tc)
{
	console_destroy(NULL);
}

ATF_TC(tc_console_set_fbaddr_ctx_swaps_pointer);
ATF_TC_HEAD(tc_console_set_fbaddr_ctx_swaps_pointer, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "console_set_fbaddr_ctx rebinds the pixel data pointer so "
	    "console_get_image_ctx reflects the new buffer");
}
ATF_TC_BODY(tc_console_set_fbaddr_ctx_swaps_pointer, tc)
{
	static uint32_t fb1[FB_SIZE];
	static uint32_t fb2[FB_SIZE];
	struct console_ctx *ctx;
	struct bhyvegc_image *img;
	memset(fb1, 0xAA, sizeof(fb1));
	memset(fb2, 0xBB, sizeof(fb2));
	ctx = console_create(TEST_W, TEST_H, fb1, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	console_set_fbaddr_ctx(ctx, fb2);
	img = console_get_image_ctx(ctx);
	ATF_REQUIRE(img != NULL);
	ATF_CHECK_EQ(img->data, fb2);
	console_destroy(ctx);
}

ATF_TC(tc_console_refresh_with_no_fb_cb_is_noop);
ATF_TC_HEAD(tc_console_refresh_with_no_fb_cb_is_noop, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "console_refresh_ctx invokes the registered fb callback and "
	    "survives a benign no-op implementation");
}
ATF_TC_BODY(tc_console_refresh_with_no_fb_cb_is_noop, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	console_fb_register_ctx(ctx, noop_fb_render, NULL, 0);
	console_refresh_ctx(ctx);
	console_destroy(ctx);
}

ATF_TC(tc_console_kbd_register_priority_higher_wins);
ATF_TC_HEAD(tc_console_kbd_register_priority_higher_wins, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "When two keyboard callbacks are registered, the higher "
	    "priority value receives events; the lower is shadowed");
}
ATF_TC_BODY(tc_console_kbd_register_priority_higher_wins, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	struct capture lo = {0}, hi = {0};
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	console_kbd_register_ctx(ctx, kbd_capture, &lo, 1);
	console_kbd_register_ctx(ctx, kbd_capture, &hi, 5);
	console_key_event_ctx(ctx, 1, 0x41, 0x02);
	ATF_CHECK_EQ(lo.called, 0);
	ATF_CHECK_EQ(hi.called, 1);
	ATF_CHECK_EQ(hi.down, 1);
	ATF_CHECK_EQ(hi.keysym, 0x41);
	ATF_CHECK_EQ(hi.keycode, 0x02);
	console_destroy(ctx);
}

ATF_TC(tc_console_kbd_register_same_pri_first_wins);
ATF_TC_HEAD(tc_console_kbd_register_same_pri_first_wins, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "When two keyboard callbacks are registered with the same "
	    "priority, the first registration retains the slot");
}
ATF_TC_BODY(tc_console_kbd_register_same_pri_first_wins, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	struct capture first = {0}, second = {0};
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	console_kbd_register_ctx(ctx, kbd_capture, &first, 3);
	console_kbd_register_ctx(ctx, kbd_capture, &second, 3);
	console_key_event_ctx(ctx, 1, 0x42, 0x03);
	ATF_CHECK_EQ(first.called, 1);
	ATF_CHECK_EQ(second.called, 0);
	console_destroy(ctx);
}

ATF_TC(tc_console_kbd_unregister_returns_zero);
ATF_TC_HEAD(tc_console_kbd_unregister_returns_zero, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "console_kbd_unregister_ctx returns 0 on success when the "
	    "callback was previously registered");
}
ATF_TC_BODY(tc_console_kbd_unregister_returns_zero, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	struct capture cap = {0};
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	console_kbd_register_ctx(ctx, kbd_capture, &cap, 1);
	ATF_CHECK_EQ(console_kbd_unregister_ctx(ctx, kbd_capture), 0);
	/* No further events should reach the unregistered callback. */
	console_key_event_ctx(ctx, 1, 0x41, 0x02);
	ATF_CHECK_EQ(cap.called, 0);
	console_destroy(ctx);
}

ATF_TC(tc_console_kbd_unregister_missing_returns_enoent);
ATF_TC_HEAD(tc_console_kbd_unregister_missing_returns_enoent, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "console_kbd_unregister_ctx returns ENOENT when the callback "
	    "pointer was never registered against the context");
}
ATF_TC_BODY(tc_console_kbd_unregister_missing_returns_enoent, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	ATF_CHECK_EQ(console_kbd_unregister_ctx(ctx, kbd_capture), ENOENT);
	console_destroy(ctx);
}

ATF_TC(tc_console_ptr_event_dispatches_to_registered);
ATF_TC_HEAD(tc_console_ptr_event_dispatches_to_registered, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "console_ptr_event_ctx delivers mask, x and y verbatim to the "
	    "registered pointer callback");
}
ATF_TC_BODY(tc_console_ptr_event_dispatches_to_registered, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	struct capture cap = {0};
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	console_ptr_register_ctx(ctx, ptr_capture, &cap, 1);
	console_ptr_event_ctx(ctx, 0x03, 123, 456);
	ATF_CHECK_EQ(cap.called, 1);
	ATF_CHECK_EQ(cap.mask, 0x03);
	ATF_CHECK_EQ(cap.x, 123);
	ATF_CHECK_EQ(cap.y, 456);
	ATF_CHECK_EQ(cap.passed_arg, &cap);
	console_destroy(ctx);
}

ATF_TC(tc_console_ptr_register_priority_higher_wins);
ATF_TC_HEAD(tc_console_ptr_register_priority_higher_wins, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Pointer callbacks follow the same priority rule as keyboard "
	    "callbacks: the highest priority registration is dispatched");
}
ATF_TC_BODY(tc_console_ptr_register_priority_higher_wins, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	struct capture lo = {0}, hi = {0};
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	console_ptr_register_ctx(ctx, ptr_capture, &lo, 1);
	console_ptr_register_ctx(ctx, ptr_capture, &hi, 10);
	console_ptr_event_ctx(ctx, 0x01, 50, 60);
	ATF_CHECK_EQ(lo.called, 0);
	ATF_CHECK_EQ(hi.called, 1);
	ATF_CHECK_EQ(hi.x, 50);
	ATF_CHECK_EQ(hi.y, 60);
	console_destroy(ctx);
}

ATF_TC(tc_console_get_image_returns_null_after_destroy);
ATF_TC_HEAD(tc_console_get_image_returns_null_after_destroy, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "After console_destroy(ctx) returns, console_get_image_ctx(ctx) "
	    "returns NULL and does not touch freed memory");
}
ATF_TC_BODY(tc_console_get_image_returns_null_after_destroy, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	struct bhyvegc_image *img;
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	img = console_get_image_ctx(ctx);
	ATF_REQUIRE(img != NULL);
	console_destroy(ctx);
	img = console_get_image_ctx(ctx);
	ATF_CHECK(img == NULL);
}

ATF_TC(tc_console_legacy_init_creates_default_ctx);
ATF_TC_HEAD(tc_console_legacy_init_creates_default_ctx, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "The legacy console_init entry point also creates a default "
	    "per-instance context accessible via console_get_default");
}
ATF_TC_BODY(tc_console_legacy_init_creates_default_ctx, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *def;
	struct bhyvegc_image *img;
	console_init(TEST_W, TEST_H, fb);
	def = console_get_default();
	ATF_REQUIRE(def != NULL);
	img = console_get_image_ctx(def);
	ATF_REQUIRE(img != NULL);
	ATF_CHECK_EQ(img->width, TEST_W);
	ATF_CHECK_EQ(img->height, TEST_H);
	ATF_CHECK_EQ(img->data, fb);
}

ATF_TC(tc_console_two_instances_independent);
ATF_TC_HEAD(tc_console_two_instances_independent, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Two independently created contexts keep their own framebuffer "
	    "and do not cross-deliver events");
}
ATF_TC_BODY(tc_console_two_instances_independent, tc)
{
	static uint32_t fb1[FB_SIZE], fb2[FB_SIZE];
	struct console_ctx *c1, *c2;
	struct capture cap1 = {0}, cap2 = {0};
	struct bhyvegc_image *i1, *i2;
	c1 = console_create(TEST_W, TEST_H, fb1, CONSOLE_FB_RAW);
	c2 = console_create(TEST_W, TEST_H, fb2, CONSOLE_FB_RAW);
	ATF_REQUIRE(c1 != NULL);
	ATF_REQUIRE(c2 != NULL);
	ATF_CHECK(c1 != c2);
	console_kbd_register_ctx(c1, kbd_capture, &cap1, 1);
	console_kbd_register_ctx(c2, kbd_capture, &cap2, 1);
	i1 = console_get_image_ctx(c1);
	i2 = console_get_image_ctx(c2);
	ATF_REQUIRE(i1 != NULL);
	ATF_REQUIRE(i2 != NULL);
	ATF_CHECK_EQ(i1->data, fb1);
	ATF_CHECK_EQ(i2->data, fb2);
	ATF_CHECK(i1->data != i2->data);
	/* Event on c1 must not be observed on c2, and vice versa. */
	console_key_event_ctx(c1, 1, 0x41, 0x02);
	ATF_CHECK_EQ(cap1.called, 1);
	ATF_CHECK_EQ(cap2.called, 0);
	console_key_event_ctx(c2, 1, 0x42, 0x03);
	ATF_CHECK_EQ(cap1.called, 1);
	ATF_CHECK_EQ(cap2.called, 1);
	console_destroy(c1);
	console_destroy(c2);
}

#define	TEN_INSTANCES	10

ATF_TC(tc_console_ten_instances_dont_interfere);
ATF_TC_HEAD(tc_console_ten_instances_dont_interfere, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Ten independently created contexts each retain their own "
	    "framebuffer, image, and callback slots with no cross-talk");
}
ATF_TC_BODY(tc_console_ten_instances_dont_interfere, tc)
{
	static uint32_t fbs[TEN_INSTANCES][FB_SIZE];
	struct console_ctx *ctxs[TEN_INSTANCES];
	struct capture caps[TEN_INSTANCES];
	int i;
	for (i = 0; i < TEN_INSTANCES; i++) {
		memset(fbs[i], (uint32_t)i, sizeof(fbs[i]));
		ctxs[i] = console_create(TEST_W, TEST_H, fbs[i],
		    CONSOLE_FB_RAW);
		ATF_REQUIRE(ctxs[i] != NULL);
	}
	/* Each context sees its own back-buffer. */
	for (i = 0; i < TEN_INSTANCES; i++) {
		struct bhyvegc_image *img = console_get_image_ctx(ctxs[i]);
		ATF_REQUIRE(img != NULL);
		ATF_CHECK_EQ(img->width, TEST_W);
		ATF_CHECK_EQ(img->height, TEST_H);
		ATF_CHECK_EQ(img->data, fbs[i]);
	}
	/* Each context owns its own keyboard slot. */
	for (i = 0; i < TEN_INSTANCES; i++) {
		memset(&caps[i], 0, sizeof(caps[i]));
		console_kbd_register_ctx(ctxs[i], kbd_capture, &caps[i], 1);
	}
	/* Dispatch to context 5 only; only cap[5] should fire. */
	console_key_event_ctx(ctxs[5], 1, 0x55, 0x55);
	for (i = 0; i < TEN_INSTANCES; i++) {
		if (i == 5) {
			ATF_CHECK_EQ(caps[i].called, 1);
			ATF_CHECK_EQ(caps[i].keysym, 0x55);
		} else {
			ATF_CHECK_EQ(caps[i].called, 0);
		}
	}
	for (i = 0; i < TEN_INSTANCES; i++)
		console_destroy(ctxs[i]);
}

#undef TEN_INSTANCES

ATF_TC(tc_console_unregister_with_null_returns_einval);
ATF_TC_HEAD(tc_console_unregister_with_null_returns_einval, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "console_fb_unregister_ctx rejects a NULL context with EINVAL "
	    "rather than dereferencing it");
}
ATF_TC_BODY(tc_console_unregister_with_null_returns_einval, tc)
{
	ATF_CHECK_EQ(console_fb_unregister_ctx(NULL, noop_fb_render), EINVAL);
}

ATF_TC(tc_console_key_event_with_no_kbd_consumer_is_noop);
ATF_TC_HEAD(tc_console_key_event_with_no_kbd_consumer_is_noop, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "If no keyboard callback is registered, console_key_event_ctx "
	    "returns silently and does not crash");
}
ATF_TC_BODY(tc_console_key_event_with_no_kbd_consumer_is_noop, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	console_key_event_ctx(ctx, 1, 0x41, 0x02);
	console_key_event_ctx(ctx, 0, 0x41, 0x02);
	console_destroy(ctx);
}

ATF_TC(tc_console_get_image_consistent_across_lookups);
ATF_TC_HEAD(tc_console_get_image_consistent_across_lookups, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "console_get_image_ctx returns the same struct bhyvegc_image "
	    "pointer across successive calls; the image is not reallocated");
}
ATF_TC_BODY(tc_console_get_image_consistent_across_lookups, tc)
{
	static uint32_t fb[FB_SIZE];
	struct console_ctx *ctx;
	struct bhyvegc_image *img1, *img2, *img3;
	ctx = console_create(TEST_W, TEST_H, fb, CONSOLE_FB_RAW);
	ATF_REQUIRE(ctx != NULL);
	img1 = console_get_image_ctx(ctx);
	img2 = console_get_image_ctx(ctx);
	img3 = console_get_image_ctx(ctx);
	ATF_REQUIRE(img1 != NULL);
	ATF_CHECK_EQ(img1, img2);
	ATF_CHECK_EQ(img2, img3);
	ATF_CHECK_EQ(img1->data, fb);
	ATF_CHECK_EQ(img1->width, TEST_W);
	ATF_CHECK_EQ(img1->height, TEST_H);
	console_destroy(ctx);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, tc_console_create_with_provided_fb);
	ATF_TP_ADD_TC(tp, tc_console_create_with_null_fb_raw_fails);
	ATF_TP_ADD_TC(tp, tc_console_create_with_bhyvegc_default);
	ATF_TP_ADD_TC(tp, tc_console_destroy_idempotent);
	ATF_TP_ADD_TC(tp, tc_console_destroy_null_safe);
	ATF_TP_ADD_TC(tp, tc_console_set_fbaddr_ctx_swaps_pointer);
	ATF_TP_ADD_TC(tp, tc_console_refresh_with_no_fb_cb_is_noop);
	ATF_TP_ADD_TC(tp, tc_console_kbd_register_priority_higher_wins);
	ATF_TP_ADD_TC(tp, tc_console_kbd_register_same_pri_first_wins);
	ATF_TP_ADD_TC(tp, tc_console_kbd_unregister_returns_zero);
	ATF_TP_ADD_TC(tp, tc_console_kbd_unregister_missing_returns_enoent);
	ATF_TP_ADD_TC(tp, tc_console_ptr_event_dispatches_to_registered);
	ATF_TP_ADD_TC(tp, tc_console_ptr_register_priority_higher_wins);
	ATF_TP_ADD_TC(tp, tc_console_get_image_returns_null_after_destroy);
	ATF_TP_ADD_TC(tp, tc_console_legacy_init_creates_default_ctx);
	ATF_TP_ADD_TC(tp, tc_console_two_instances_independent);
	ATF_TP_ADD_TC(tp, tc_console_ten_instances_dont_interfere);
	ATF_TP_ADD_TC(tp, tc_console_unregister_with_null_returns_einval);
	ATF_TP_ADD_TC(tp, tc_console_key_event_with_no_kbd_consumer_is_noop);
	ATF_TP_ADD_TC(tp, tc_console_get_image_consistent_across_lookups);
	return (atf_no_error());
}
