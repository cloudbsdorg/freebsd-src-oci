/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for the RCTL resource-limit model
 * (usr.sbin/ocifbsd/security/rctl.c): size parsing/formatting, resource
 * and action name mapping, and the OCI Linux resources -> rctl_limits
 * translation. Pure; no rctl(8), jail(8), or root required.
 */

#include <atf-c.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "security/rctl.h"
#include "security/rctl.c"

/* ----- rctl_parse_size ----- */

ATF_TC(parse_size_plain);
ATF_TC_HEAD(parse_size_plain, tc)
{
	atf_tc_set_md_var(tc, "descr", "a bare number parses as bytes");
}
ATF_TC_BODY(parse_size_plain, tc)
{
	ATF_CHECK_EQ((uint64_t)512, rctl_parse_size("512"));
}

ATF_TC(parse_size_suffixes);
ATF_TC_HEAD(parse_size_suffixes, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "K/M/G/T suffixes scale by binary multiples, case-insensitive");
}
ATF_TC_BODY(parse_size_suffixes, tc)
{
	ATF_CHECK_EQ((uint64_t)2 * 1024, rctl_parse_size("2K"));
	ATF_CHECK_EQ((uint64_t)2 * 1024, rctl_parse_size("2k"));
	ATF_CHECK_EQ((uint64_t)4 * 1024 * 1024, rctl_parse_size("4M"));
	ATF_CHECK_EQ((uint64_t)4 * 1024 * 1024, rctl_parse_size("4m"));
	ATF_CHECK_EQ((uint64_t)1024 * 1024 * 1024, rctl_parse_size("1G"));
	ATF_CHECK_EQ((uint64_t)1024ULL * 1024 * 1024 * 1024,
	    rctl_parse_size("1T"));
}

ATF_TC(parse_size_empty_and_null);
ATF_TC_HEAD(parse_size_empty_and_null, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "an empty string or NULL is zero, not a crash");
}
ATF_TC_BODY(parse_size_empty_and_null, tc)
{
	ATF_CHECK_EQ((uint64_t)0, rctl_parse_size(""));
	ATF_CHECK_EQ((uint64_t)0, rctl_parse_size(NULL));
}

/* ----- rctl_format_size ----- */

ATF_TC(format_size_tiers);
ATF_TC_HEAD(format_size_tiers, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "each binary tier renders with its own suffix (a gibibyte is G, "
	    "not 1024M; a mebibyte is M, not 1024K)");
}
ATF_TC_BODY(format_size_tiers, tc)
{
	ATF_CHECK_STREQ("512", rctl_format_size(512));
	ATF_CHECK_STREQ("2K", rctl_format_size((uint64_t)2 * 1024));
	ATF_CHECK_STREQ("4M", rctl_format_size((uint64_t)4 * 1024 * 1024));
	ATF_CHECK_STREQ("1G", rctl_format_size((uint64_t)1024 * 1024 * 1024));
	ATF_CHECK_STREQ("3G",
	    rctl_format_size((uint64_t)3 * 1024 * 1024 * 1024));
	ATF_CHECK_STREQ("1T",
	    rctl_format_size((uint64_t)1024ULL * 1024 * 1024 * 1024));
}

/* ----- resource + action name mapping ----- */

ATF_TC(resource_name_roundtrip);
ATF_TC_HEAD(resource_name_roundtrip, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "resource name <-> enum round-trips; out-of-range is 'unknown'");
}
ATF_TC_BODY(resource_name_roundtrip, tc)
{
	ATF_CHECK_STREQ("nproc", rctl_resource_name(RCTL_RESOURCE_NPROC));
	ATF_CHECK_STREQ("memoryuse",
	    rctl_resource_name(RCTL_RESOURCE_MEMORYUSE));
	ATF_CHECK_EQ(RCTL_RESOURCE_MEMORYUSE, rctl_parse_resource("memoryuse"));
	ATF_CHECK_EQ(RCTL_RESOURCE_NPROC, rctl_parse_resource("nproc"));
	/* Unknown name -> (rctl_resource_t)-1 */
	ATF_CHECK_EQ((rctl_resource_t)-1, rctl_parse_resource("nonsense"));
	/* Out-of-range enum -> "unknown" (not an OOB read) */
	ATF_CHECK_STREQ("unknown", rctl_resource_name((rctl_resource_t)9999));
	ATF_CHECK_STREQ("unknown", rctl_resource_name((rctl_resource_t)-1));
}

ATF_TC(action_parse);
ATF_TC_HEAD(action_parse, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "action names map to enum; an unknown action defaults to deny");
}
ATF_TC_BODY(action_parse, tc)
{
	ATF_CHECK_EQ(RCTL_ACTION_DENY, rctl_parse_action("deny"));
	ATF_CHECK_EQ(RCTL_ACTION_LOG, rctl_parse_action("log"));
	ATF_CHECK_EQ(RCTL_ACTION_SIGINFO, rctl_parse_action("siginfo"));
	ATF_CHECK_EQ(RCTL_ACTION_SIGTERM, rctl_parse_action("sigterm"));
	ATF_CHECK_EQ(RCTL_ACTION_SIGKILL, rctl_parse_action("sigkill"));
	/* Fail closed: unknown action is deny, not allow. */
	ATF_CHECK_EQ(RCTL_ACTION_DENY, rctl_parse_action("whatever"));
}

/* ----- OCI resources -> rctl_limits ----- */

ATF_TC(oci_resources_translation);
ATF_TC_HEAD(oci_resources_translation, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "an OCI linux.resources object maps onto the rctl_limits fields");
}
ATF_TC_BODY(oci_resources_translation, tc)
{
	const char *json =
	    "{ \"cpu\": { \"shares\": 1024, \"quota\": 50000, "
	    "\"period\": 100000 }, "
	    "\"memory\": { \"limit\": 536870912, \"reservation\": 268435456, "
	    "\"disableOOMKiller\": 1 }, "
	    "\"pids\": { \"pidsLimit\": 128 } }";
	struct rctl_limits limits;

	ATF_CHECK_EQ(0, rctl_parse_oci_resources(&limits, json));
	ATF_CHECK_EQ((uint64_t)1024, limits.cpu_shares);
	ATF_CHECK_EQ((uint64_t)50000, limits.cpu_quota);
	ATF_CHECK_EQ((uint64_t)100000, limits.cpu_period);
	ATF_CHECK_EQ((uint64_t)536870912, limits.memory_limit);
	ATF_CHECK_EQ((uint64_t)268435456, limits.memory_reservation);
	ATF_CHECK(limits.memory_oom_kill_disable);
	ATF_CHECK_EQ((uint64_t)128, limits.proc_limit);
}

ATF_TC(oci_resources_null_json_zeroes);
ATF_TC_HEAD(oci_resources_null_json_zeroes, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "NULL limits is an error; NULL json leaves a fully zeroed struct");
}
ATF_TC_BODY(oci_resources_null_json_zeroes, tc)
{
	struct rctl_limits limits;

	/* NULL destination is rejected. */
	ATF_CHECK_EQ(-1, rctl_parse_oci_resources(NULL, "{}"));

	/* Pre-dirty the struct, then confirm NULL json zeroes it. */
	memset(&limits, 0xff, sizeof(limits));
	ATF_CHECK_EQ(0, rctl_parse_oci_resources(&limits, NULL));
	ATF_CHECK_EQ((uint64_t)0, limits.cpu_shares);
	ATF_CHECK_EQ((uint64_t)0, limits.memory_limit);
	ATF_CHECK_EQ((uint64_t)0, limits.proc_limit);
	ATF_CHECK(!limits.memory_oom_kill_disable);
}

/* ----- CPU quota/period -> pcpu ----- */

ATF_TC(quota_to_pcpu);
ATF_TC_HEAD(quota_to_pcpu, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "OCI cpu.quota/cpu.period converts to an RCTL pcpu percentage, "
	    "not the raw microsecond quota");
}
ATF_TC_BODY(quota_to_pcpu, tc)
{
	/* 50000us / 100000us = 50%. */
	ATF_CHECK_EQ((uint64_t)50, rctl_quota_to_pcpu(50000, 100000));
	/* A full core. */
	ATF_CHECK_EQ((uint64_t)100, rctl_quota_to_pcpu(100000, 100000));
	/* Two cores' worth may exceed 100. */
	ATF_CHECK_EQ((uint64_t)200, rctl_quota_to_pcpu(200000, 100000));
	/* Unspecified period defaults to 100000us. */
	ATF_CHECK_EQ((uint64_t)25, rctl_quota_to_pcpu(25000, 0));
	/* A tiny positive quota floors at 1%, never 0 (which is unlimited). */
	ATF_CHECK_EQ((uint64_t)1, rctl_quota_to_pcpu(1, 100000));
	/* Zero quota is zero (no rule). */
	ATF_CHECK_EQ((uint64_t)0, rctl_quota_to_pcpu(0, 100000));
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, quota_to_pcpu);
	ATF_TP_ADD_TC(tp, parse_size_plain);
	ATF_TP_ADD_TC(tp, parse_size_suffixes);
	ATF_TP_ADD_TC(tp, parse_size_empty_and_null);
	ATF_TP_ADD_TC(tp, format_size_tiers);
	ATF_TP_ADD_TC(tp, resource_name_roundtrip);
	ATF_TP_ADD_TC(tp, action_parse);
	ATF_TP_ADD_TC(tp, oci_resources_translation);
	ATF_TP_ADD_TC(tp, oci_resources_null_json_zeroes);

	return (atf_no_error());
}
