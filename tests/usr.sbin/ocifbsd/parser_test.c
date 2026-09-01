/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Unit tests for usr.sbin/ocifbsd/convert/parser.c
 *
 * Tests the pure escape + native-format functions which have no
 * external dependencies (no jail, no ZFS, no networking). The
 * source-under-test is #include'd directly so static functions
 * are visible if needed.
 */

#include <sys/param.h>

#include <atf-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "convert.h"
#include "convert/parser.c"

/* ----- yaml_escape ----- */

ATF_TC(yaml_escape_null);
ATF_TC_HEAD(yaml_escape_null, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "yaml_escape(NULL) returns the quoted empty string");
}
ATF_TC_BODY(yaml_escape_null, tc)
{
	/*
	 * A NULL (absent) value must serialize to the quoted empty string
	 * "" so the emitted YAML stays valid; a bare empty string would
	 * produce a dangling "key:" with no value.
	 */
	char *r = yaml_escape(NULL);
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"\"");
	free(r);
}

ATF_TC(yaml_escape_empty);
ATF_TC_HEAD(yaml_escape_empty, tc)
{
	atf_tc_set_md_var(tc, "descr", "yaml_escape(\"\") returns \"\"");
}
ATF_TC_BODY(yaml_escape_empty, tc)
{
	char *r = yaml_escape("");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"\"");
	free(r);
}

ATF_TC(yaml_escape_plain);
ATF_TC_HEAD(yaml_escape_plain, tc)
{
	atf_tc_set_md_var(tc, "descr", "yaml_escape of plain text wraps in quotes");
}
ATF_TC_BODY(yaml_escape_plain, tc)
{
	char *r = yaml_escape("hello");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"hello\"");
	free(r);
}

ATF_TC(yaml_escape_quote);
ATF_TC_HEAD(yaml_escape_quote, tc)
{
	atf_tc_set_md_var(tc, "descr", "yaml_escape escapes embedded double-quote");
}
ATF_TC_BODY(yaml_escape_quote, tc)
{
	char *r = yaml_escape("he said \"hi\"");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"he said \\\"hi\\\"\"");
	free(r);
}

ATF_TC(yaml_escape_backslash);
ATF_TC_HEAD(yaml_escape_backslash, tc)
{
	atf_tc_set_md_var(tc, "descr", "yaml_escape escapes backslash");
}
ATF_TC_BODY(yaml_escape_backslash, tc)
{
	char *r = yaml_escape("a\\b");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"a\\\\b\"");
	free(r);
}

ATF_TC(yaml_escape_newline);
ATF_TC_HEAD(yaml_escape_newline, tc)
{
	atf_tc_set_md_var(tc, "descr", "yaml_escape escapes LF as \\n literal");
}
ATF_TC_BODY(yaml_escape_newline, tc)
{
	char *r = yaml_escape("line1\nline2");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"line1\\nline2\"");
	free(r);
}

ATF_TC(yaml_escape_tab);
ATF_TC_HEAD(yaml_escape_tab, tc)
{
	atf_tc_set_md_var(tc, "descr", "yaml_escape escapes TAB as \\t literal");
}
ATF_TC_BODY(yaml_escape_tab, tc)
{
	char *r = yaml_escape("a\tb");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"a\\tb\"");
	free(r);
}

ATF_TC(yaml_escape_cr);
ATF_TC_HEAD(yaml_escape_cr, tc)
{
	atf_tc_set_md_var(tc, "descr", "yaml_escape escapes CR as \\r literal");
}
ATF_TC_BODY(yaml_escape_cr, tc)
{
	char *r = yaml_escape("a\rb");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"a\\rb\"");
	free(r);
}

/* ----- json_escape ----- */

ATF_TC(json_escape_null);
ATF_TC_HEAD(json_escape_null, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "json_escape(NULL) returns the quoted empty string");
}
ATF_TC_BODY(json_escape_null, tc)
{
	/* NULL must serialize to "" so the emitted JSON stays valid. */
	char *r = json_escape(NULL);
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"\"");
	free(r);
}

ATF_TC(json_escape_plain);
ATF_TC_HEAD(json_escape_plain, tc)
{
	atf_tc_set_md_var(tc, "descr", "json_escape of plain text wraps in quotes");
}
ATF_TC_BODY(json_escape_plain, tc)
{
	char *r = json_escape("hello");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"hello\"");
	free(r);
}

ATF_TC(json_escape_quote);
ATF_TC_HEAD(json_escape_quote, tc)
{
	atf_tc_set_md_var(tc, "descr", "json_escape escapes embedded double-quote");
}
ATF_TC_BODY(json_escape_quote, tc)
{
	char *r = json_escape("a\"b");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"a\\\"b\"");
	free(r);
}

ATF_TC(json_escape_backslash);
ATF_TC_HEAD(json_escape_backslash, tc)
{
	atf_tc_set_md_var(tc, "descr", "json_escape escapes backslash");
}
ATF_TC_BODY(json_escape_backslash, tc)
{
	char *r = json_escape("a\\b");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"a\\\\b\"");
	free(r);
}

ATF_TC(json_escape_control_char);
ATF_TC_HEAD(json_escape_control_char, tc)
{
	atf_tc_set_md_var(tc, "descr", "json_escape encodes control char (<32) as \\uXXXX");
}
ATF_TC_BODY(json_escape_control_char, tc)
{
	/*
	 * String-literal pitfall: "a\x01b" is parsed as the single hex
	 * escape \x01b (= 0x1b = ESC) followed by nothing, not \x01 + b.
	 * Use string concatenation to terminate the hex escape.
	 */
	char *r = json_escape("a\x01" "b");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"a\\u0001b\"");
	free(r);
}

ATF_TC(json_escape_high_char_passthrough);
ATF_TC_HEAD(json_escape_high_char_passthrough, tc)
{
	atf_tc_set_md_var(tc, "descr", "json_escape passes through chars >= 32 unchanged");
}
ATF_TC_BODY(json_escape_high_char_passthrough, tc)
{
	char *r = json_escape("a~b");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"a~b\"");
	free(r);
}

ATF_TC(json_escape_utf8_high_byte);
ATF_TC_HEAD(json_escape_utf8_high_byte, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "json_escape passes UTF-8 high bytes (>=0x80) through unchanged");
}
ATF_TC_BODY(json_escape_utf8_high_byte, tc)
{
	/*
	 * A high-bit byte is negative as a signed char; the old code tested
	 * *p < 32, treated it as a control char, and both overflowed the
	 * buffer and emitted a bogus 8-digit \u escape. It must pass through
	 * verbatim (e.g. the two bytes of U+00E9 "é" in UTF-8: 0xC3 0xA9).
	 */
	char *r = json_escape("caf\xc3\xa9");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"caf\xc3\xa9\"");
	free(r);
}

ATF_TC(yaml_escape_utf8_high_byte);
ATF_TC_HEAD(yaml_escape_utf8_high_byte, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "yaml_escape passes UTF-8 high bytes through unchanged");
}
ATF_TC_BODY(yaml_escape_utf8_high_byte, tc)
{
	char *r = yaml_escape("caf\xc3\xa9");
	ATF_REQUIRE(r != NULL);
	ATF_CHECK_STREQ(r, "\"caf\xc3\xa9\"");
	free(r);
}

/* ----- native_format_service ----- */

ATF_TC(native_format_service_full);
ATF_TC_HEAD(native_format_service_full, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "native_format_service with all fields includes ports line");
}
ATF_TC_BODY(native_format_service_full, tc)
{
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *r = native_format_service("web", "nginx:1.25", 3,
	    "80:80", NULL, NULL, NULL, NULL, &opts);
	ATF_REQUIRE(r != NULL);
	ATF_CHECK(strstr(r, "name: web") != NULL);
	ATF_CHECK(strstr(r, "image: nginx:1.25") != NULL);
	ATF_CHECK(strstr(r, "replicas: 3") != NULL);
	ATF_CHECK(strstr(r, "ports:") != NULL);
	ATF_CHECK(strstr(r, "80:80") != NULL);
	free(r);
}

ATF_TC(native_format_service_null_fields);
ATF_TC_HEAD(native_format_service_null_fields, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "native_format_service defaults NULL name/image, omits ports line");
}
ATF_TC_BODY(native_format_service_null_fields, tc)
{
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *r = native_format_service(NULL, NULL, 1, NULL, NULL, NULL, NULL,
	    NULL, &opts);
	ATF_REQUIRE(r != NULL);
	ATF_CHECK(strstr(r, "name: unnamed") != NULL);
	ATF_CHECK(strstr(r, "image: nginx:latest") != NULL);
	ATF_CHECK(strstr(r, "replicas: 1") != NULL);
	ATF_CHECK(strstr(r, "ports:") == NULL);
	free(r);
}

/* ----- native_format_network ----- */

ATF_TC(native_format_network_full);
ATF_TC_HEAD(native_format_network_full, tc)
{
	atf_tc_set_md_var(tc, "descr", "native_format_network with all fields");
}
ATF_TC_BODY(native_format_network_full, tc)
{
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *r = native_format_network("net1", "bridge", "10.0.0.0/24", &opts);
	ATF_REQUIRE(r != NULL);
	ATF_CHECK(strstr(r, "name: net1") != NULL);
	ATF_CHECK(strstr(r, "driver: bridge") != NULL);
	ATF_CHECK(strstr(r, "subnet: 10.0.0.0/24") != NULL);
	free(r);
}

ATF_TC(native_format_network_null_fields);
ATF_TC_HEAD(native_format_network_null_fields, tc)
{
	atf_tc_set_md_var(tc, "descr", "native_format_network defaults NULL fields");
}
ATF_TC_BODY(native_format_network_null_fields, tc)
{
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *r = native_format_network(NULL, NULL, NULL, &opts);
	ATF_REQUIRE(r != NULL);
	ATF_CHECK(strstr(r, "name: default") != NULL);
	ATF_CHECK(strstr(r, "driver: bridge") != NULL);
	ATF_CHECK(strstr(r, "subnet:") == NULL);
	free(r);
}

/* ----- native_format_volume ----- */

ATF_TC(native_format_volume_full);
ATF_TC_HEAD(native_format_volume_full, tc)
{
	atf_tc_set_md_var(tc, "descr", "native_format_volume with all fields");
}
ATF_TC_BODY(native_format_volume_full, tc)
{
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *r = native_format_volume("data", "zfs", NULL, &opts);
	ATF_REQUIRE(r != NULL);
	ATF_CHECK(strstr(r, "name: data") != NULL);
	ATF_CHECK(strstr(r, "driver: zfs") != NULL);
	free(r);
}

/* ----- native_format_stack ----- */

ATF_TC(native_format_stack_with_services);
ATF_TC_HEAD(native_format_stack_with_services, tc)
{
	atf_tc_set_md_var(tc, "descr", "native_format_stack with services");
}
ATF_TC_BODY(native_format_stack_with_services, tc)
{
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *r = native_format_stack("mystack",
	    "  - name: web\n    image: nginx\n", NULL, NULL, &opts);
	ATF_REQUIRE(r != NULL);
	ATF_CHECK(strstr(r, "name: mystack") != NULL);
	ATF_CHECK(strstr(r, "namespace: default") != NULL);
	ATF_CHECK(strstr(r, "services:") != NULL);
	ATF_CHECK(strstr(r, "- name: web") != NULL);
	free(r);
}

ATF_TC(native_format_stack_null);
ATF_TC_HEAD(native_format_stack_null, tc)
{
	atf_tc_set_md_var(tc, "descr", "native_format_stack with all NULLs");
}
ATF_TC_BODY(native_format_stack_null, tc)
{
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *r = native_format_stack(NULL, NULL, NULL, NULL, &opts);
	ATF_REQUIRE(r != NULL);
	ATF_CHECK(strstr(r, "name: unnamed") != NULL);
	free(r);
}

/* ----- parse_yaml / parse_json (stubs; just verify copy semantics) ----- */

ATF_TC(parse_yaml_copies);
ATF_TC_HEAD(parse_yaml_copies, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "parse_yaml stub returns a copy of the input");
}
ATF_TC_BODY(parse_yaml_copies, tc)
{
	char src[] = "hello world";
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *out = NULL;
	int rc = parse_yaml(src, &out, &opts);
	ATF_CHECK_EQ(rc, CONVERT_SUCCESS);
	ATF_REQUIRE(out != NULL);
	ATF_CHECK_STREQ(out, "hello world");
	ATF_CHECK(out != src); /* must be a copy, not the same ptr */
	free(out);
}

ATF_TC(parse_json_copies);
ATF_TC_HEAD(parse_json_copies, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "parse_json stub returns a copy of the input");
}
ATF_TC_BODY(parse_json_copies, tc)
{
	char src[] = "{\"a\":1}";
	struct convert_options opts;
	memset(&opts, 0, sizeof(opts));
	char *out = NULL;
	int rc = parse_json(src, &out, &opts);
	ATF_CHECK_EQ(rc, CONVERT_SUCCESS);
	ATF_REQUIRE(out != NULL);
	ATF_CHECK_STREQ(out, "{\"a\":1}");
	ATF_CHECK(out != src);
	free(out);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, yaml_escape_null);
	ATF_TP_ADD_TC(tp, yaml_escape_empty);
	ATF_TP_ADD_TC(tp, yaml_escape_plain);
	ATF_TP_ADD_TC(tp, yaml_escape_quote);
	ATF_TP_ADD_TC(tp, yaml_escape_backslash);
	ATF_TP_ADD_TC(tp, yaml_escape_newline);
	ATF_TP_ADD_TC(tp, yaml_escape_tab);
	ATF_TP_ADD_TC(tp, yaml_escape_cr);

	ATF_TP_ADD_TC(tp, json_escape_null);
	ATF_TP_ADD_TC(tp, json_escape_plain);
	ATF_TP_ADD_TC(tp, json_escape_quote);
	ATF_TP_ADD_TC(tp, json_escape_backslash);
	ATF_TP_ADD_TC(tp, json_escape_control_char);
	ATF_TP_ADD_TC(tp, json_escape_high_char_passthrough);
	ATF_TP_ADD_TC(tp, json_escape_utf8_high_byte);
	ATF_TP_ADD_TC(tp, yaml_escape_utf8_high_byte);

	ATF_TP_ADD_TC(tp, native_format_service_full);
	ATF_TP_ADD_TC(tp, native_format_service_null_fields);
	ATF_TP_ADD_TC(tp, native_format_network_full);
	ATF_TP_ADD_TC(tp, native_format_network_null_fields);
	ATF_TP_ADD_TC(tp, native_format_volume_full);
	ATF_TP_ADD_TC(tp, native_format_stack_with_services);
	ATF_TP_ADD_TC(tp, native_format_stack_null);

	ATF_TP_ADD_TC(tp, parse_yaml_copies);
	ATF_TP_ADD_TC(tp, parse_json_copies);

	return (atf_no_error());
}
