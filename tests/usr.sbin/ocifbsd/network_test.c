/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for the container network primitives
 * (usr.sbin/ocifbsd/network/network.c). Covers the epair peer-name
 * derivation, which underpins epair_create now that interface names come
 * from the kernel rather than being fabricated. Pure; no ifconfig or root.
 */

#include <atf-c.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>

/*
 * network.c references mkdirp (used only by the on-disk network store paths,
 * not by anything under test). It is not exported by FreeBSD 16 libutil and
 * the network archive is not linked into the product, so provide a stub to
 * satisfy the link for this pure unit test.
 */
int mkdirp(const char *path, mode_t mode);
int
mkdirp(const char *path, mode_t mode)
{
	(void)path;
	(void)mode;
	return (0);
}

#include "network/network.h"
#include "network/network.c"

ATF_TC(peer_name_basic);
ATF_TC_HEAD(peer_name_basic, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "the 'a' side of an epair maps to the matching 'b' side");
}
ATF_TC_BODY(peer_name_basic, tc)
{
	char buf[IFNAMSIZ];

	ATF_CHECK_EQ(0, epair_peer_name("epair0a", buf, sizeof(buf)));
	ATF_CHECK_STREQ("epair0b", buf);

	ATF_CHECK_EQ(0, epair_peer_name("epair17a", buf, sizeof(buf)));
	ATF_CHECK_STREQ("epair17b", buf);
}

ATF_TC(peer_name_only_last_a_changes);
ATF_TC_HEAD(peer_name_only_last_a_changes, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "only the trailing 'a' is rewritten, not an 'a' inside the name");
}
ATF_TC_BODY(peer_name_only_last_a_changes, tc)
{
	char buf[IFNAMSIZ];

	/* A prefix containing 'a' must survive; only the final char flips. */
	ATF_CHECK_EQ(0, epair_peer_name("epaira3a", buf, sizeof(buf)));
	ATF_CHECK_STREQ("epaira3b", buf);
}

ATF_TC(peer_name_rejects_bad_input);
ATF_TC_HEAD(peer_name_rejects_bad_input, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "empty, non-'a'-terminated, NULL, and oversized names are rejected");
}
ATF_TC_BODY(peer_name_rejects_bad_input, tc)
{
	char buf[IFNAMSIZ];
	char small[4];

	ATF_CHECK(epair_peer_name(NULL, buf, sizeof(buf)) != 0);
	ATF_CHECK(epair_peer_name("", buf, sizeof(buf)) != 0);
	/* Does not end in 'a' (e.g. the 'b' side was passed by mistake). */
	ATF_CHECK(epair_peer_name("epair0b", buf, sizeof(buf)) != 0);
	ATF_CHECK(epair_peer_name("epair0", buf, sizeof(buf)) != 0);
	/* Too long for the destination buffer. */
	ATF_CHECK(epair_peer_name("epair12345a", small, sizeof(small)) != 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, peer_name_basic);
	ATF_TP_ADD_TC(tp, peer_name_only_last_a_changes);
	ATF_TP_ADD_TC(tp, peer_name_rejects_bad_input);

	return (atf_no_error());
}
