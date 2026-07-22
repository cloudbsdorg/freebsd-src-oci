/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 CloudBSD
 *
 * Unit tests for image/whiteout.c pure helpers.
 */

#include <sys/param.h>

#include <atf-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image/unpack.h"
#include "image/whiteout.c"

ATF_TC(is_whiteout_basic);
ATF_TC_HEAD(is_whiteout_basic, tc)
{
	atf_tc_set_md_var(tc, "descr", "is_whiteout detects .wh. prefix");
}
ATF_TC_BODY(is_whiteout_basic, tc)
{
	ATF_CHECK(is_whiteout(".wh.foo"));
	ATF_CHECK(is_whiteout(".wh..wh..opq"));
	ATF_CHECK(!is_whiteout("foo"));
	ATF_CHECK(!is_whiteout("wh.foo"));
	ATF_CHECK(!is_whiteout(NULL));
	ATF_CHECK(!is_whiteout(""));
}

ATF_TC(get_whiteout_target_basic);
ATF_TC_HEAD(get_whiteout_target_basic, tc)
{
	atf_tc_set_md_var(tc, "descr", "get_whiteout_target strips .wh. prefix");
}
ATF_TC_BODY(get_whiteout_target_basic, tc)
{
	char *t;

	t = get_whiteout_target(".wh.foo");
	ATF_REQUIRE(t != NULL);
	ATF_CHECK_STREQ(t, "foo");
	free(t);

	t = get_whiteout_target(".wh.etc/passwd");
	ATF_REQUIRE(t != NULL);
	ATF_CHECK_STREQ(t, "etc/passwd");
	free(t);
}

ATF_TC(get_whiteout_opaque);
ATF_TC_HEAD(get_whiteout_opaque, tc)
{
	atf_tc_set_md_var(tc, "descr", "opaque marker yields NULL target");
}
ATF_TC_BODY(get_whiteout_opaque, tc)
{
	ATF_CHECK(get_whiteout_target(".wh..wh..opq") == NULL);
	ATF_CHECK(get_whiteout_target("not-a-whiteout") == NULL);
	ATF_CHECK(get_whiteout_target(NULL) == NULL);
}

ATF_TC(is_whiteout_edge);
ATF_TC_HEAD(is_whiteout_edge, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "is_whiteout edge cases: prefix only, empty, long name");
}
ATF_TC_BODY(is_whiteout_edge, tc)
{
	ATF_CHECK(is_whiteout(".wh."));
	ATF_CHECK(is_whiteout(".wh.a"));
	ATF_CHECK(!is_whiteout(".w"));
	ATF_CHECK(!is_whiteout(" .wh.x"));
}

ATF_TC(get_whiteout_empty_target);
ATF_TC_HEAD(get_whiteout_empty_target, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "get_whiteout_target(\".wh.\") yields empty string");
}
ATF_TC_BODY(get_whiteout_empty_target, tc)
{
	char *t;

	t = get_whiteout_target(".wh.");
	ATF_REQUIRE(t != NULL);
	ATF_CHECK_STREQ(t, "");
	free(t);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, is_whiteout_basic);
	ATF_TP_ADD_TC(tp, get_whiteout_target_basic);
	ATF_TP_ADD_TC(tp, get_whiteout_opaque);
	ATF_TP_ADD_TC(tp, is_whiteout_edge);
	ATF_TP_ADD_TC(tp, get_whiteout_empty_target);
	return (atf_no_error());
}
