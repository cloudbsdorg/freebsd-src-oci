/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for JSON pretty-printing (src/jsonfmt.c).
 */

#include <atf-c.h>
#include <stdlib.h>
#include <string.h>

#include "src/jsonfmt.h"
#include "src/jsonfmt.c"

ATF_TC(pretty_indents_object);
ATF_TC_HEAD(pretty_indents_object, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ocifbsd_json_pretty expands a compact object onto indented lines");
}
ATF_TC_BODY(pretty_indents_object, tc)
{
	char *p;

	p = ocifbsd_json_pretty("{\"id\":\"abc\",\"status\":\"running\"}");
	ATF_REQUIRE(p != NULL);
	/* Pretty output spans multiple lines and indents members. */
	ATF_CHECK(strchr(p, '\n') != NULL);
	ATF_CHECK(strstr(p, "\"id\"") != NULL);
	ATF_CHECK(strstr(p, "\"status\"") != NULL);
	ATF_CHECK(strstr(p, "running") != NULL);
	free(p);
}

ATF_TC(pretty_nested_and_arrays);
ATF_TC_HEAD(pretty_nested_and_arrays, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "nested objects and arrays are preserved and expanded");
}
ATF_TC_BODY(pretty_nested_and_arrays, tc)
{
	char *p;

	p = ocifbsd_json_pretty("{\"a\":[1,2,3],\"b\":{\"c\":true}}");
	ATF_REQUIRE(p != NULL);
	ATF_CHECK(strchr(p, '\n') != NULL);
	ATF_CHECK(strstr(p, "\"a\"") != NULL);
	ATF_CHECK(strstr(p, "\"c\"") != NULL);
	free(p);
}

ATF_TC(pretty_rejects_bad_json);
ATF_TC_HEAD(pretty_rejects_bad_json, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "invalid JSON yields NULL so callers can fall back to raw text");
}
ATF_TC_BODY(pretty_rejects_bad_json, tc)
{
	ATF_CHECK(ocifbsd_json_pretty("{not json") == NULL);
	ATF_CHECK(ocifbsd_json_pretty(NULL) == NULL);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, pretty_indents_object);
	ATF_TP_ADD_TC(tp, pretty_nested_and_arrays);
	ATF_TP_ADD_TC(tp, pretty_rejects_bad_json);
	return (atf_no_error());
}
