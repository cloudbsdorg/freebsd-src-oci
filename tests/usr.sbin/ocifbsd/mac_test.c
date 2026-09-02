/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for the MAC label model (usr.sbin/ocifbsd/security/mac.c):
 * argument-safety guard, label string parsing (Biba/MLS), label
 * stringification and its round-trip, and Biba label construction. Pure;
 * no mac(4), jail(8), or root required.
 */

#include <atf-c.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "security/mac.h"
#include "security/mac.c"

/* ----- mac_arg_is_safe ----- */

ATF_TC(arg_is_safe_accepts_label_chars);
ATF_TC_HEAD(arg_is_safe_accepts_label_chars, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "label-shaped arguments (alnum . - _ / ,) are accepted");
}
ATF_TC_BODY(arg_is_safe_accepts_label_chars, tc)
{
	ATF_CHECK(mac_arg_is_safe("biba/effective=1:3"));
	ATF_CHECK(mac_arg_is_safe("mls/level=low"));
	ATF_CHECK(mac_arg_is_safe("jail_name-01"));
}

ATF_TC(arg_is_safe_rejects_shell_metachars);
ATF_TC_HEAD(arg_is_safe_rejects_shell_metachars, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "whitespace, shell metacharacters, NULL and empty are rejected");
}
ATF_TC_BODY(arg_is_safe_rejects_shell_metachars, tc)
{
	ATF_CHECK(!mac_arg_is_safe(NULL));
	ATF_CHECK(!mac_arg_is_safe(""));
	ATF_CHECK(!mac_arg_is_safe("a b"));
	ATF_CHECK(!mac_arg_is_safe("a;rm -rf /"));
	ATF_CHECK(!mac_arg_is_safe("$(id)"));
	ATF_CHECK(!mac_arg_is_safe("a`b`"));
	ATF_CHECK(!mac_arg_is_safe("a|b"));
}

/* ----- mac_label_parse ----- */

ATF_TC(parse_null_and_empty);
ATF_TC_HEAD(parse_null_and_empty, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "NULL or empty label yields *label == NULL and success");
}
ATF_TC_BODY(parse_null_and_empty, tc)
{
	struct mac_label *l = (void *)0x1;
	ATF_CHECK_EQ(0, mac_label_parse(NULL, &l));
	ATF_CHECK(l == NULL);
	l = (void *)0x1;
	ATF_CHECK_EQ(0, mac_label_parse("", &l));
	ATF_CHECK(l == NULL);
}

ATF_TC(parse_biba_effective_and_range);
ATF_TC_HEAD(parse_biba_effective_and_range, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "a biba/ label parses effective and range fields");
}
ATF_TC_BODY(parse_biba_effective_and_range, tc)
{
	struct mac_label *l = NULL;
	ATF_REQUIRE_EQ(0,
	    mac_label_parse("biba/effective=high,range=low:high", &l));
	ATF_REQUIRE(l != NULL);
	ATF_CHECK_EQ(MAC_TYPE_BIBA, l->type);
	ATF_CHECK_STREQ("high", l->biba_effective);
	ATF_CHECK_STREQ("low", l->biba_range_low);
	ATF_CHECK_STREQ("high", l->biba_range_high);
	mac_label_free(l);
}

ATF_TC(parse_mls_level_and_range);
ATF_TC_HEAD(parse_mls_level_and_range, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "an mls/ label parses level and range fields");
}
ATF_TC_BODY(parse_mls_level_and_range, tc)
{
	struct mac_label *l = NULL;
	ATF_REQUIRE_EQ(0,
	    mac_label_parse("mls/level=low,range=low:high", &l));
	ATF_REQUIRE(l != NULL);
	ATF_CHECK_EQ(MAC_TYPE_MLS, l->type);
	ATF_CHECK_STREQ("low", l->mls_level);
	ATF_CHECK_STREQ("low", l->mls_range_low);
	ATF_CHECK_STREQ("high", l->mls_range_high);
	mac_label_free(l);
}

/* ----- mac_label_to_string ----- */

ATF_TC(to_string_null_is_empty);
ATF_TC_HEAD(to_string_null_is_empty, tc)
{
	atf_tc_set_md_var(tc, "descr", "a NULL label stringifies to \"\"");
}
ATF_TC_BODY(to_string_null_is_empty, tc)
{
	char *s = NULL;
	ATF_CHECK_EQ(0, mac_label_to_string(NULL, &s));
	ATF_REQUIRE(s != NULL);
	ATF_CHECK_STREQ("", s);
	free(s);
}

ATF_TC(to_string_biba_roundtrip);
ATF_TC_HEAD(to_string_biba_roundtrip, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "a parsed biba effective label round-trips through to_string "
	    "instead of collapsing to 0:0");
}
ATF_TC_BODY(to_string_biba_roundtrip, tc)
{
	struct mac_label *l = NULL;
	char *s = NULL;

	ATF_REQUIRE_EQ(0, mac_label_parse("biba/effective=high", &l));
	ATF_REQUIRE(l != NULL);
	ATF_CHECK_EQ(0, mac_label_to_string(l, &s));
	ATF_REQUIRE(s != NULL);
	ATF_CHECK_STREQ("biba/effective=high", s);
	free(s);
	mac_label_free(l);
}

ATF_TC(from_biba_builds_fields_and_string);
ATF_TC_HEAD(from_biba_builds_fields_and_string, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "mac_label_from_biba populates fields and stringifies to "
	    "biba/effective=<low>:<high>");
}
ATF_TC_BODY(from_biba_builds_fields_and_string, tc)
{
	struct mac_label *l = NULL;
	char *s = NULL;

	ATF_REQUIRE_EQ(0, mac_label_from_biba(1, 3, &l));
	ATF_REQUIRE(l != NULL);
	ATF_CHECK_EQ(MAC_TYPE_BIBA, l->type);
	ATF_CHECK_STREQ("1:3", l->biba_effective);
	ATF_CHECK_EQ(0, mac_label_to_string(l, &s));
	ATF_REQUIRE(s != NULL);
	ATF_CHECK_STREQ("biba/effective=1:3", s);
	free(s);
	mac_label_free(l);
}

ATF_TC(to_string_none_is_none);
ATF_TC_HEAD(to_string_none_is_none, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "a label with no MAC type stringifies to \"none\"");
}
ATF_TC_BODY(to_string_none_is_none, tc)
{
	struct mac_label *l = mac_label_alloc();
	char *s = NULL;

	ATF_REQUIRE(l != NULL);	/* type defaults to MAC_TYPE_NONE */
	ATF_CHECK_EQ(0, mac_label_to_string(l, &s));
	ATF_REQUIRE(s != NULL);
	ATF_CHECK_STREQ("none", s);
	free(s);
	mac_label_free(l);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, arg_is_safe_accepts_label_chars);
	ATF_TP_ADD_TC(tp, arg_is_safe_rejects_shell_metachars);
	ATF_TP_ADD_TC(tp, parse_null_and_empty);
	ATF_TP_ADD_TC(tp, parse_biba_effective_and_range);
	ATF_TP_ADD_TC(tp, parse_mls_level_and_range);
	ATF_TP_ADD_TC(tp, to_string_null_is_empty);
	ATF_TP_ADD_TC(tp, to_string_biba_roundtrip);
	ATF_TP_ADD_TC(tp, from_biba_builds_fields_and_string);
	ATF_TP_ADD_TC(tp, to_string_none_is_none);

	return (atf_no_error());
}
