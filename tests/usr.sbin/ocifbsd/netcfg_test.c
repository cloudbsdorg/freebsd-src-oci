/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for the container network configuration model
 * (usr.sbin/ocifbsd/network/netcfg.c). Pure; no jail(8) or root required.
 */

#include <atf-c.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "network/netcfg.h"
#include "network/netcfg.c"

/* ----- validation ----- */

ATF_TC(valid_ip4_cidr);
ATF_TC_HEAD(valid_ip4_cidr, tc)
{
	atf_tc_set_md_var(tc, "descr", "IPv4 CIDR validation accepts/rejects");
}
ATF_TC_BODY(valid_ip4_cidr, tc)
{
	ATF_CHECK(netcfg_valid_ip4_cidr("10.0.0.5/24"));
	ATF_CHECK(netcfg_valid_ip4_cidr("192.168.1.1/32"));
	ATF_CHECK(netcfg_valid_ip4_cidr("0.0.0.0/0"));
	/* No prefix, bad prefix, out-of-range, junk, v6 in a v4 slot. */
	ATF_CHECK(!netcfg_valid_ip4_cidr("10.0.0.5"));
	ATF_CHECK(!netcfg_valid_ip4_cidr("10.0.0.5/33"));
	ATF_CHECK(!netcfg_valid_ip4_cidr("10.0.0.5/-1"));
	ATF_CHECK(!netcfg_valid_ip4_cidr("10.0.0.5/x"));
	ATF_CHECK(!netcfg_valid_ip4_cidr("999.1.1.1/24"));
	ATF_CHECK(!netcfg_valid_ip4_cidr("nonsense"));
	ATF_CHECK(!netcfg_valid_ip4_cidr("fe80::1/64"));
	ATF_CHECK(!netcfg_valid_ip4_cidr(NULL));
}

ATF_TC(valid_ip6_cidr);
ATF_TC_HEAD(valid_ip6_cidr, tc)
{
	atf_tc_set_md_var(tc, "descr", "IPv6 CIDR validation accepts/rejects");
}
ATF_TC_BODY(valid_ip6_cidr, tc)
{
	ATF_CHECK(netcfg_valid_ip6_cidr("fe80::1/64"));
	ATF_CHECK(netcfg_valid_ip6_cidr("2001:db8::/32"));
	ATF_CHECK(netcfg_valid_ip6_cidr("::1/128"));
	ATF_CHECK(!netcfg_valid_ip6_cidr("fe80::1"));
	ATF_CHECK(!netcfg_valid_ip6_cidr("fe80::1/129"));
	ATF_CHECK(!netcfg_valid_ip6_cidr("10.0.0.1/24"));
	ATF_CHECK(!netcfg_valid_ip6_cidr("gg::/16"));
}

/* ----- mutators + validation coupling ----- */

ATF_TC(add_ip_validates);
ATF_TC_HEAD(add_ip_validates, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "add_ip4/add_ip6 reject invalid values and keep the config intact");
}
ATF_TC_BODY(add_ip_validates, tc)
{
	struct netcfg nc;

	netcfg_init(&nc);
	ATF_CHECK_EQ(netcfg_add_ip4(&nc, "10.0.0.2/24"), 0);
	ATF_CHECK_EQ(nc.n_ip4, (size_t)1);
	/* Invalid must fail and not grow the array. */
	ATF_CHECK(netcfg_add_ip4(&nc, "bogus") != 0);
	ATF_CHECK_EQ(nc.n_ip4, (size_t)1);
	ATF_CHECK_STREQ(nc.ip4[0], "10.0.0.2/24");

	ATF_CHECK_EQ(netcfg_add_ip6(&nc, "2001:db8::2/64"), 0);
	ATF_CHECK_EQ(nc.n_ip6, (size_t)1);
	ATF_CHECK(netcfg_add_ip6(&nc, "10.0.0.1/24") != 0);
	ATF_CHECK_EQ(nc.n_ip6, (size_t)1);
	netcfg_free(&nc);
}

ATF_TC(set_gateway_validates);
ATF_TC_HEAD(set_gateway_validates, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "gateways take a plain address and reject CIDR/garbage");
}
ATF_TC_BODY(set_gateway_validates, tc)
{
	struct netcfg nc;

	netcfg_init(&nc);
	ATF_CHECK_EQ(netcfg_set_gateway4(&nc, "10.0.0.1"), 0);
	ATF_CHECK_STREQ(nc.gateway4, "10.0.0.1");
	ATF_CHECK(netcfg_set_gateway4(&nc, "10.0.0.1/24") != 0);
	ATF_CHECK(netcfg_set_gateway4(&nc, "nope") != 0);
	/* Unchanged after failures. */
	ATF_CHECK_STREQ(nc.gateway4, "10.0.0.1");

	ATF_CHECK_EQ(netcfg_set_gateway6(&nc, "fe80::1"), 0);
	ATF_CHECK_STREQ(nc.gateway6, "fe80::1");
	ATF_CHECK(netcfg_set_gateway6(&nc, "10.0.0.1") != 0);
	netcfg_free(&nc);
}

/* ----- serialize / parse round trip ----- */

ATF_TC(json_round_trip);
ATF_TC_HEAD(json_round_trip, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "to_json then parse reproduces the configuration");
}
ATF_TC_BODY(json_round_trip, tc)
{
	struct netcfg a, b;
	char *json;

	netcfg_init(&a);
	ATF_CHECK_EQ(netcfg_set_vnet(&a, true), 0);
	ATF_CHECK_EQ(netcfg_add_ip4(&a, "10.0.0.2/24"), 0);
	ATF_CHECK_EQ(netcfg_add_ip4(&a, "10.0.0.3/24"), 0);
	ATF_CHECK_EQ(netcfg_add_ip6(&a, "2001:db8::2/64"), 0);
	ATF_CHECK_EQ(netcfg_set_gateway4(&a, "10.0.0.1"), 0);
	ATF_CHECK_EQ(netcfg_add_dns(&a, "1.1.1.1"), 0);

	json = netcfg_to_json(&a);
	ATF_REQUIRE(json != NULL);

	netcfg_init(&b);
	ATF_REQUIRE_EQ(netcfg_parse(json, &b), 0);
	ATF_CHECK_EQ(b.vnet, 1);
	ATF_REQUIRE_EQ(b.n_ip4, (size_t)2);
	ATF_CHECK_STREQ(b.ip4[0], "10.0.0.2/24");
	ATF_CHECK_STREQ(b.ip4[1], "10.0.0.3/24");
	ATF_REQUIRE_EQ(b.n_ip6, (size_t)1);
	ATF_CHECK_STREQ(b.ip6[0], "2001:db8::2/64");
	ATF_CHECK_STREQ(b.gateway4, "10.0.0.1");
	ATF_REQUIRE_EQ(b.n_dns, (size_t)1);
	ATF_CHECK_STREQ(b.dns[0], "1.1.1.1");

	free(json);
	netcfg_free(&a);
	netcfg_free(&b);
}

ATF_TC(parse_empty_and_bad);
ATF_TC_HEAD(parse_empty_and_bad, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "parsing an empty object yields defaults; malformed JSON fails");
}
ATF_TC_BODY(parse_empty_and_bad, tc)
{
	struct netcfg nc;

	netcfg_init(&nc);
	ATF_CHECK_EQ(netcfg_parse("{}", &nc), 0);
	ATF_CHECK_EQ(nc.vnet, -1);		/* unset */
	ATF_CHECK_EQ(nc.n_ip4, (size_t)0);
	netcfg_free(&nc);

	netcfg_init(&nc);
	ATF_CHECK(netcfg_parse("{ not json", &nc) != 0);
	netcfg_free(&nc);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, valid_ip4_cidr);
	ATF_TP_ADD_TC(tp, valid_ip6_cidr);
	ATF_TP_ADD_TC(tp, add_ip_validates);
	ATF_TP_ADD_TC(tp, set_gateway_validates);
	ATF_TP_ADD_TC(tp, json_round_trip);
	ATF_TP_ADD_TC(tp, parse_empty_and_bad);
	return (atf_no_error());
}
