/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for usr.sbin/ocifbsd/src/oci2jail.c (parse / validate /
 * jailparam mapping). No jail_create required; uses libjail for param
 * helpers only.
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <atf-c.h>
#include <errno.h>
#include <jail.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ocifbsd.h"
#include "src/oci2jail.c"

static void
write_config(const char *path, const char *json)
{
	FILE *f;

	f = fopen(path, "w");
	ATF_REQUIRE(f != NULL);
	ATF_REQUIRE(fputs(json, f) >= 0);
	ATF_REQUIRE(fclose(f) == 0);
}

static void
make_rootfs(const char *path)
{
	ATF_REQUIRE(mkdir(path, 0755) == 0 || errno == EEXIST);
}

/* ----- oci_parse_config ----- */

ATF_TC(parse_minimal);
ATF_TC_HEAD(parse_minimal, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "parse minimal OCI config with process.user object");
}
ATF_TC_BODY(parse_minimal, tc)
{
	struct oci_runtime_spec *spec;

	make_rootfs("rootfs");
	write_config("config.json",
	    "{\n"
	    "  \"ociVersion\": \"1.0.2\",\n"
	    "  \"hostname\": \"test-host\",\n"
	    "  \"process\": {\n"
	    "    \"user\": { \"uid\": 1000, \"gid\": 1001 },\n"
	    "    \"args\": [ \"/bin/true\" ],\n"
	    "    \"env\": [ \"PATH=/bin\" ],\n"
	    "    \"cwd\": \"/\"\n"
	    "  },\n"
	    "  \"root\": { \"path\": \"rootfs\", \"readonly\": false }\n"
	    "}\n");

	spec = oci_parse_config("config.json");
	ATF_REQUIRE(spec != NULL);
	ATF_REQUIRE(spec->root.path != NULL);
	ATF_CHECK_STREQ(spec->root.path, "rootfs");
	ATF_CHECK_STREQ(spec->hostname, "test-host");
	ATF_REQUIRE(spec->process.args != NULL);
	ATF_CHECK_STREQ(spec->process.args[0], "/bin/true");
	ATF_CHECK_EQ(spec->process.uid, (uid_t)1000);
	ATF_CHECK_EQ(spec->process.gid, (gid_t)1001);
	ATF_CHECK_STREQ(spec->process.cwd, "/");
	oci_free_spec(spec);
}

ATF_TC(parse_user_int);
ATF_TC_HEAD(parse_user_int, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "process.user as bare integer sets uid=gid");
}
ATF_TC_BODY(parse_user_int, tc)
{
	struct oci_runtime_spec *spec;

	make_rootfs("rootfs");
	write_config("config.json",
	    "{\n"
	    "  \"process\": {\n"
	    "    \"user\": 42,\n"
	    "    \"args\": [ \"/bin/sh\" ]\n"
	    "  },\n"
	    "  \"root\": { \"path\": \"rootfs\" }\n"
	    "}\n");

	spec = oci_parse_config("config.json");
	ATF_REQUIRE(spec != NULL);
	ATF_CHECK_EQ(spec->process.uid, (uid_t)42);
	ATF_CHECK_EQ(spec->process.gid, (gid_t)42);
	oci_free_spec(spec);
}

ATF_TC(parse_mounts);
ATF_TC_HEAD(parse_mounts, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "parse mounts with options array and readonly");
}
ATF_TC_BODY(parse_mounts, tc)
{
	struct oci_runtime_spec *spec;

	make_rootfs("rootfs");
	write_config("config.json",
	    "{\n"
	    "  \"process\": { \"args\": [ \"/bin/true\" ] },\n"
	    "  \"root\": { \"path\": \"rootfs\" },\n"
	    "  \"mounts\": [\n"
	    "    {\n"
	    "      \"destination\": \"/data\",\n"
	    "      \"type\": \"nullfs\",\n"
	    "      \"source\": \"/var/tmp\",\n"
	    "      \"options\": [ \"ro\", \"nosuid\" ]\n"
	    "    },\n"
	    "    {\n"
	    "      \"destination\": \"/dev\",\n"
	    "      \"type\": \"devfs\",\n"
	    "      \"source\": \"devfs\"\n"
	    "    }\n"
	    "  ]\n"
	    "}\n");

	spec = oci_parse_config("config.json");
	ATF_REQUIRE(spec != NULL);
	ATF_REQUIRE_EQ(spec->n_mounts, 2);
	ATF_REQUIRE(spec->mounts != NULL);
	ATF_CHECK_STREQ(spec->mounts[0].destination, "/data");
	ATF_CHECK_STREQ(spec->mounts[0].type, "nullfs");
	ATF_CHECK_STREQ(spec->mounts[0].source, "/var/tmp");
	ATF_REQUIRE(spec->mounts[0].options != NULL);
	ATF_CHECK(strstr(spec->mounts[0].options, "ro") != NULL);
	ATF_CHECK(spec->mounts[0].readonly);
	ATF_CHECK_STREQ(spec->mounts[1].type, "devfs");
	oci_free_spec(spec);
}

ATF_TC(parse_freebsd_ext);
ATF_TC_HEAD(parse_freebsd_ext, tc)
{
	atf_tc_set_md_var(tc, "descr", "parse freebsd extension block");
}
ATF_TC_BODY(parse_freebsd_ext, tc)
{
	struct oci_runtime_spec *spec;

	make_rootfs("rootfs");
	write_config("config.json",
	    "{\n"
	    "  \"process\": { \"args\": [ \"/bin/true\" ] },\n"
	    "  \"root\": { \"path\": \"rootfs\" },\n"
	    "  \"freebsd\": {\n"
	    "    \"vnet\": true,\n"
	    "    \"hostname\": \"jail-host\",\n"
	    "    \"ip4\": [ \"10.0.0.2\" ]\n"
	    "  }\n"
	    "}\n");

	spec = oci_parse_config("config.json");
	ATF_REQUIRE(spec != NULL);
	ATF_REQUIRE(spec->freebsd != NULL);
	ATF_CHECK(spec->freebsd->vnet);
	ATF_CHECK_STREQ(spec->freebsd->hostname, "jail-host");
	ATF_REQUIRE(spec->freebsd->ip4 != NULL);
	ATF_CHECK_STREQ(spec->freebsd->ip4[0], "10.0.0.2");
	oci_free_spec(spec);
}

ATF_TC(parse_missing_file);
ATF_TC_HEAD(parse_missing_file, tc)
{
	atf_tc_set_md_var(tc, "descr", "oci_parse_config missing file fails");
}
ATF_TC_BODY(parse_missing_file, tc)
{
	ATF_CHECK(oci_parse_config("/nonexistent/ocifbsd-config.json") == NULL);
}

ATF_TC(parse_bad_json);
ATF_TC_HEAD(parse_bad_json, tc)
{
	atf_tc_set_md_var(tc, "descr", "oci_parse_config rejects invalid JSON");
}
ATF_TC_BODY(parse_bad_json, tc)
{
	write_config("config.json", "{ not json");
	ATF_CHECK(oci_parse_config("config.json") == NULL);
}

/* ----- oci_validate_spec ----- */

ATF_TC(validate_ok);
ATF_TC_HEAD(validate_ok, tc)
{
	atf_tc_set_md_var(tc, "descr", "validate accepts complete local rootfs");
}
ATF_TC_BODY(validate_ok, tc)
{
	struct oci_runtime_spec *spec;

	make_rootfs("rootfs");
	write_config("config.json",
	    "{\n"
	    "  \"process\": { \"args\": [ \"/bin/true\" ] },\n"
	    "  \"root\": { \"path\": \"rootfs\" }\n"
	    "}\n");
	spec = oci_parse_config("config.json");
	ATF_REQUIRE(spec != NULL);
	ATF_CHECK_EQ(oci_validate_spec(spec), 0);
	oci_free_spec(spec);
}

ATF_TC(validate_missing_root);
ATF_TC_HEAD(validate_missing_root, tc)
{
	atf_tc_set_md_var(tc, "descr", "validate fails when root path missing");
}
ATF_TC_BODY(validate_missing_root, tc)
{
	struct oci_runtime_spec spec;

	memset(&spec, 0, sizeof(spec));
	spec.root.path = NULL;
	ATF_CHECK(oci_validate_spec(&spec) != 0);
}

ATF_TC(validate_missing_args);
ATF_TC_HEAD(validate_missing_args, tc)
{
	atf_tc_set_md_var(tc, "descr", "validate fails without process args");
}
ATF_TC_BODY(validate_missing_args, tc)
{
	struct oci_runtime_spec *spec;

	make_rootfs("rootfs");
	write_config("config.json",
	    "{ \"root\": { \"path\": \"rootfs\" }, \"process\": {} }\n");
	spec = oci_parse_config("config.json");
	ATF_REQUIRE(spec != NULL);
	ATF_CHECK(oci_validate_spec(spec) != 0);
	oci_free_spec(spec);
}

/* ----- oci_spec_to_jail_params ----- */

ATF_TC(jailparams_basic);
ATF_TC_HEAD(jailparams_basic, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "jail params include path, hostname, persist, name");
}
ATF_TC_BODY(jailparams_basic, tc)
{
	struct oci_runtime_spec *spec;
	struct jailparam *params;
	size_t nparams, i;
	int saw_path = 0, saw_host = 0, saw_persist = 0, saw_name = 0;

	make_rootfs("rootfs");
	write_config("config.json",
	    "{\n"
	    "  \"hostname\": \"jp-host\",\n"
	    "  \"process\": { \"args\": [ \"/bin/true\" ] },\n"
	    "  \"root\": { \"path\": \"rootfs\" }\n"
	    "}\n");
	spec = oci_parse_config("config.json");
	ATF_REQUIRE(spec != NULL);

	params = oci_spec_to_jail_params(spec, &nparams);
	ATF_REQUIRE(params != NULL);
	ATF_CHECK(nparams >= 3);

	for (i = 0; i < nparams; i++) {
		if (params[i].jp_name == NULL)
			continue;
		if (strcmp(params[i].jp_name, "path") == 0)
			saw_path = 1;
		if (strcmp(params[i].jp_name, "host.hostname") == 0)
			saw_host = 1;
		if (strcmp(params[i].jp_name, "persist") == 0)
			saw_persist = 1;
		if (strcmp(params[i].jp_name, "name") == 0)
			saw_name = 1;
	}
	ATF_CHECK(saw_path);
	ATF_CHECK(saw_host);
	ATF_CHECK(saw_persist);
	ATF_CHECK(saw_name);

	jailparam_free(params, nparams);
	oci_free_spec(spec);
}

ATF_TC(jailparams_vnet);
ATF_TC_HEAD(jailparams_vnet, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "freebsd.vnet maps to jailparam vnet=new");
}
ATF_TC_BODY(jailparams_vnet, tc)
{
	struct oci_runtime_spec *spec;
	struct jailparam *params;
	size_t nparams, i;
	int saw_vnet = 0;

	make_rootfs("rootfs");
	write_config("config.json",
	    "{\n"
	    "  \"process\": { \"args\": [ \"/bin/true\" ] },\n"
	    "  \"root\": { \"path\": \"rootfs\" },\n"
	    "  \"freebsd\": { \"vnet\": true }\n"
	    "}\n");
	spec = oci_parse_config("config.json");
	ATF_REQUIRE(spec != NULL);
	params = oci_spec_to_jail_params(spec, &nparams);
	ATF_REQUIRE(params != NULL);
	for (i = 0; i < nparams; i++) {
		if (params[i].jp_name != NULL &&
		    strcmp(params[i].jp_name, "vnet") == 0)
			saw_vnet = 1;
	}
	ATF_CHECK(saw_vnet);
	jailparam_free(params, nparams);
	oci_free_spec(spec);
}

ATF_TC(jailparams_null);
ATF_TC_HEAD(jailparams_null, tc)
{
	atf_tc_set_md_var(tc, "descr", "oci_spec_to_jail_params(NULL) is NULL");
}
ATF_TC_BODY(jailparams_null, tc)
{
	size_t n = 0;

	ATF_CHECK(oci_spec_to_jail_params(NULL, &n) == NULL);
}

ATF_TC(parse_rlimits);
ATF_TC_HEAD(parse_rlimits, tc)
{
	atf_tc_set_md_var(tc, "descr", "process.rlimits array is parsed");
}
ATF_TC_BODY(parse_rlimits, tc)
{
	struct oci_runtime_spec *spec;

	make_rootfs("rootfs");
	write_config("config.json",
	    "{\n"
	    "  \"process\": {\n"
	    "    \"args\": [ \"/bin/true\" ],\n"
	    "    \"rlimits\": [\n"
	    "      { \"type\": \"RLIMIT_NOFILE\", \"hard\": 256, \"soft\": 128 }\n"
	    "    ]\n"
	    "  },\n"
	    "  \"root\": { \"path\": \"rootfs\" }\n"
	    "}\n");
	spec = oci_parse_config("config.json");
	ATF_REQUIRE(spec != NULL);
	ATF_REQUIRE_EQ(spec->process.n_rlimits, 1);
	ATF_REQUIRE(spec->process.rlimits != NULL);
	ATF_CHECK_STREQ(spec->process.rlimits[0].type, "RLIMIT_NOFILE");
	ATF_CHECK_EQ(spec->process.rlimits[0].hard, (rlim_t)256);
	ATF_CHECK_EQ(spec->process.rlimits[0].soft, (rlim_t)128);
	oci_free_spec(spec);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, parse_minimal);
	ATF_TP_ADD_TC(tp, parse_user_int);
	ATF_TP_ADD_TC(tp, parse_mounts);
	ATF_TP_ADD_TC(tp, parse_freebsd_ext);
	ATF_TP_ADD_TC(tp, parse_missing_file);
	ATF_TP_ADD_TC(tp, parse_bad_json);
	ATF_TP_ADD_TC(tp, validate_ok);
	ATF_TP_ADD_TC(tp, validate_missing_root);
	ATF_TP_ADD_TC(tp, validate_missing_args);
	ATF_TP_ADD_TC(tp, jailparams_basic);
	ATF_TP_ADD_TC(tp, jailparams_vnet);
	ATF_TP_ADD_TC(tp, jailparams_null);
	ATF_TP_ADD_TC(tp, parse_rlimits);
	return (atf_no_error());
}
