/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for the namespace validators
 * (usr.sbin/ocifbsd/namespace/namespace.c). Pure; no jail/root/threads.
 */

#include <atf-c.h>
#include <stdbool.h>
#include <string.h>
#include <sys/types.h>

/*
 * namespace.c uses mkdirp (a program-global from the ocifbsd binary) in its
 * state-save path, which nothing under test exercises. Provide a stub so the
 * pure validators link.
 */
int mkdirp(const char *path, mode_t mode);
int
mkdirp(const char *path, mode_t mode)
{
	(void)path;
	(void)mode;
	return (0);
}

#include "namespace/namespace.c"

ATF_TC(name_valid);
ATF_TC_HEAD(name_valid, tc)
{
	atf_tc_set_md_var(tc, "descr", "namespace name charset/length rules");
}
ATF_TC_BODY(name_valid, tc)
{
	char longname[80];

	ATF_CHECK(ns_name_is_valid("prod"));
	ATF_CHECK(ns_name_is_valid("team-a_1.dev"));
	/* rejects: NULL, empty, leading dot/dash, bad chars, too long */
	ATF_CHECK(!ns_name_is_valid(NULL));
	ATF_CHECK(!ns_name_is_valid(""));
	ATF_CHECK(!ns_name_is_valid(".hidden"));
	ATF_CHECK(!ns_name_is_valid("-lead"));
	ATF_CHECK(!ns_name_is_valid("has space"));
	ATF_CHECK(!ns_name_is_valid("slash/no"));
	memset(longname, 'a', sizeof(longname));
	longname[sizeof(longname) - 1] = '\0';	/* 79 chars > 63 */
	ATF_CHECK(!ns_name_is_valid(longname));
}

ATF_TC(mac_label_valid);
ATF_TC_HEAD(mac_label_valid, tc)
{
	atf_tc_set_md_var(tc, "descr", "namespace MAC label charset/length rules");
}
ATF_TC_BODY(mac_label_valid, tc)
{
	/* Allowed charset: alnum plus / - _ . : , */
	ATF_CHECK(ns_mac_label_is_valid("biba/high"));
	ATF_CHECK(ns_mac_label_is_valid("prod/high:low"));
	ATF_CHECK(ns_mac_label_is_valid("a-b_c.d,e/f"));
	/* Rejected: NULL, empty, and any char outside the set. */
	ATF_CHECK(!ns_mac_label_is_valid(NULL));
	ATF_CHECK(!ns_mac_label_is_valid(""));
	ATF_CHECK(!ns_mac_label_is_valid("has space"));
	ATF_CHECK(!ns_mac_label_is_valid("semi;colon"));
	ATF_CHECK(!ns_mac_label_is_valid("level=x"));	/* '=' not allowed */
	ATF_CHECK(!ns_mac_label_is_valid("paren(s)"));	/* '(' ')' not allowed */
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, name_valid);
	ATF_TP_ADD_TC(tp, mac_label_valid);

	return (atf_no_error());
}
