/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for usr.sbin/ocifbsd/src/utils.c pure helpers.
 * No jail(8) or root required.
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <atf-c.h>
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ocifbsd.h"
#include "src/utils.c"

/* ----- generate_container_id ----- */

ATF_TC(cid_format);
ATF_TC_HEAD(cid_format, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "generate_container_id returns 64 lowercase hex digits");
}
ATF_TC_BODY(cid_format, tc)
{
	char *id;
	size_t i, n;

	id = generate_container_id();
	ATF_REQUIRE(id != NULL);
	n = strlen(id);
	ATF_CHECK_EQ(n, (size_t)(OCIFBSD_MAX_CONTAINER_ID_LENGTH - 1));
	for (i = 0; i < n; i++)
		ATF_CHECK(isxdigit((unsigned char)id[i]) != 0 &&
		    (isdigit((unsigned char)id[i]) != 0 ||
		    (id[i] >= 'a' && id[i] <= 'f')));
	free(id);
}

ATF_TC(cid_unique);
ATF_TC_HEAD(cid_unique, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "two generate_container_id calls produce different IDs");
}
ATF_TC_BODY(cid_unique, tc)
{
	char *a, *b;

	a = generate_container_id();
	b = generate_container_id();
	ATF_REQUIRE(a != NULL);
	ATF_REQUIRE(b != NULL);
	ATF_CHECK(strcmp(a, b) != 0);
	free(a);
	free(b);
}

/* ----- canonical_name ----- */

ATF_TC(canonical_null);
ATF_TC_HEAD(canonical_null, tc)
{
	atf_tc_set_md_var(tc, "descr", "canonical_name(NULL) returns NULL");
}
ATF_TC_BODY(canonical_null, tc)
{
	ATF_CHECK(canonical_name(NULL) == NULL);
}

ATF_TC(canonical_lower_and_hyphen);
ATF_TC_HEAD(canonical_lower_and_hyphen, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "canonical_name lowercases and replaces spaces");
}
ATF_TC_BODY(canonical_lower_and_hyphen, tc)
{
	char *c;

	c = canonical_name("Hello World");
	ATF_REQUIRE(c != NULL);
	ATF_CHECK_STREQ(c, "hello-world");
	free(c);
}

ATF_TC(canonical_keeps_alnum_underscore);
ATF_TC_HEAD(canonical_keeps_alnum_underscore, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "canonical_name keeps alnum, hyphen, underscore");
}
ATF_TC_BODY(canonical_keeps_alnum_underscore, tc)
{
	char *c;

	c = canonical_name("web_app-01");
	ATF_REQUIRE(c != NULL);
	ATF_CHECK_STREQ(c, "web_app-01");
	free(c);
}

ATF_TC(canonical_empty_after_trim);
ATF_TC_HEAD(canonical_empty_after_trim, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "canonical_name of only punctuation yields NULL");
}
ATF_TC_BODY(canonical_empty_after_trim, tc)
{
	char *c;

	c = canonical_name("@@@");
	ATF_CHECK(c == NULL);
}

/* ----- ocifbsd_state_to_string ----- */

ATF_TC(state_strings);
ATF_TC_HEAD(state_strings, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ocifbsd_state_to_string covers known states");
}
ATF_TC_BODY(state_strings, tc)
{
	ATF_CHECK_STREQ(ocifbsd_state_to_string(OCIFBSD_STATE_CREATED),
	    "created");
	ATF_CHECK_STREQ(ocifbsd_state_to_string(OCIFBSD_STATE_RUNNING),
	    "running");
	ATF_CHECK_STREQ(ocifbsd_state_to_string(OCIFBSD_STATE_STOPPED),
	    "stopped");
	ATF_CHECK_STREQ(ocifbsd_state_to_string(OCIFBSD_STATE_UNKNOWN),
	    "unknown");
}

/* ----- ocifbsd_reconcile_state ----- */

ATF_TC(reconcile_running_dead_jail);
ATF_TC_HEAD(reconcile_running_dead_jail, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "a running container whose jail is gone reconciles to stopped");
}
ATF_TC_BODY(reconcile_running_dead_jail, tc)
{
	ATF_CHECK_EQ(ocifbsd_reconcile_state(OCIFBSD_STATE_RUNNING, false),
	    OCIFBSD_STATE_STOPPED);
}

ATF_TC(reconcile_running_live_jail);
ATF_TC_HEAD(reconcile_running_live_jail, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "a running container with a live jail stays running");
}
ATF_TC_BODY(reconcile_running_live_jail, tc)
{
	ATF_CHECK_EQ(ocifbsd_reconcile_state(OCIFBSD_STATE_RUNNING, true),
	    OCIFBSD_STATE_RUNNING);
}

ATF_TC(reconcile_paused_dead_jail);
ATF_TC_HEAD(reconcile_paused_dead_jail, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "a paused container whose jail is gone reconciles to stopped");
}
ATF_TC_BODY(reconcile_paused_dead_jail, tc)
{
	ATF_CHECK_EQ(ocifbsd_reconcile_state(OCIFBSD_STATE_PAUSED, false),
	    OCIFBSD_STATE_STOPPED);
	ATF_CHECK_EQ(ocifbsd_reconcile_state(OCIFBSD_STATE_PAUSED_HIGH, false),
	    OCIFBSD_STATE_STOPPED);
}

ATF_TC(reconcile_nonrunning_unaffected);
ATF_TC_HEAD(reconcile_nonrunning_unaffected, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "created/stopped states are authoritative regardless of jail");
}
ATF_TC_BODY(reconcile_nonrunning_unaffected, tc)
{
	/* A created container has no jail yet; liveness must not matter. */
	ATF_CHECK_EQ(ocifbsd_reconcile_state(OCIFBSD_STATE_CREATED, false),
	    OCIFBSD_STATE_CREATED);
	ATF_CHECK_EQ(ocifbsd_reconcile_state(OCIFBSD_STATE_STOPPED, false),
	    OCIFBSD_STATE_STOPPED);
	ATF_CHECK_EQ(ocifbsd_reconcile_state(OCIFBSD_STATE_STOPPED, true),
	    OCIFBSD_STATE_STOPPED);
}

/* ----- ensure_directory / resolve_bundle_path ----- */

ATF_TC(ensure_directory_create);
ATF_TC_HEAD(ensure_directory_create, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ensure_directory creates a missing directory");
}
ATF_TC_BODY(ensure_directory_create, tc)
{
	char path[] = "testdir.XXXXXX";
	char *dir;
	struct stat sb;

	dir = mkdtemp(path);
	ATF_REQUIRE(dir != NULL);
	/* remove so ensure_directory must create */
	ATF_REQUIRE(rmdir(dir) == 0);
	ATF_REQUIRE_EQ(ensure_directory(dir, 0755), 0);
	ATF_REQUIRE(stat(dir, &sb) == 0);
	ATF_CHECK(S_ISDIR(sb.st_mode));
	ATF_REQUIRE_EQ(ensure_directory(dir, 0755), 0); /* idempotent */
	(void)rmdir(dir);
}

ATF_TC(resolve_bundle_absolute);
ATF_TC_HEAD(resolve_bundle_absolute, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "resolve_bundle_path leaves absolute paths unchanged");
}
ATF_TC_BODY(resolve_bundle_absolute, tc)
{
	char *p;

	p = resolve_bundle_path("/tmp/oci-bundle");
	ATF_REQUIRE(p != NULL);
	ATF_CHECK_STREQ(p, "/tmp/oci-bundle");
	free(p);
}

ATF_TC(resolve_bundle_null);
ATF_TC_HEAD(resolve_bundle_null, tc)
{
	atf_tc_set_md_var(tc, "descr", "resolve_bundle_path(NULL) is NULL");
}
ATF_TC_BODY(resolve_bundle_null, tc)
{
	ATF_CHECK(resolve_bundle_path(NULL) == NULL);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, cid_format);
	ATF_TP_ADD_TC(tp, cid_unique);
	ATF_TP_ADD_TC(tp, canonical_null);
	ATF_TP_ADD_TC(tp, canonical_lower_and_hyphen);
	ATF_TP_ADD_TC(tp, canonical_keeps_alnum_underscore);
	ATF_TP_ADD_TC(tp, canonical_empty_after_trim);
	ATF_TP_ADD_TC(tp, state_strings);
	ATF_TP_ADD_TC(tp, reconcile_running_dead_jail);
	ATF_TP_ADD_TC(tp, reconcile_running_live_jail);
	ATF_TP_ADD_TC(tp, reconcile_paused_dead_jail);
	ATF_TP_ADD_TC(tp, reconcile_nonrunning_unaffected);
	ATF_TP_ADD_TC(tp, ensure_directory_create);
	ATF_TP_ADD_TC(tp, resolve_bundle_absolute);
	ATF_TP_ADD_TC(tp, resolve_bundle_null);
	return (atf_no_error());
}
