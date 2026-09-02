/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for the CNI plugin interface (usr.sbin/ocifbsd/network/cni.c):
 * the parts that need no external plugin binary — building the CNI_* execution
 * environment and parsing a CNI netconf file. Plugin invocation and real CNI
 * plugin compatibility are a separate, plugin-dependent task (.plan 3.10).
 */

#include <atf-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * network.h carries the prototypes for cni.c's non-static entry points
 * (cni_add/cni_del/cni_check); include it first so the module compiles clean
 * under the test suite's WARNS=3 (-Wmissing-prototypes).
 */
#include "network/network.h"
#include "network/cni.c"

/* Find "KEY=" in the env array and return the value, or NULL. */
static const char *
env_lookup(char **env, int nenv, const char *key)
{
	size_t klen = strlen(key);
	int i;

	for (i = 0; i < nenv; i++) {
		if (env[i] != NULL && strncmp(env[i], key, klen) == 0 &&
		    env[i][klen] == '=')
			return (env[i] + klen + 1);
	}
	return (NULL);
}

ATF_TC(build_env);
ATF_TC_HEAD(build_env, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "cni_build_env sets the CNI_* variables a plugin expects");
}
ATF_TC_BODY(build_env, tc)
{
	char **env = NULL;
	int nenv = 0, i;

	ATF_REQUIRE_EQ(0, cni_build_env("deadbeefcafe",
	    "/var/run/netns/jail0", "eth0", &env, &nenv));
	ATF_REQUIRE(env != NULL);
	ATF_CHECK(nenv >= 5);

	ATF_CHECK_STREQ("ADD", env_lookup(env, nenv, "CNI_COMMAND"));
	ATF_CHECK_STREQ("deadbeefcafe",
	    env_lookup(env, nenv, "CNI_CONTAINERID"));
	ATF_CHECK_STREQ("/var/run/netns/jail0",
	    env_lookup(env, nenv, "CNI_NETNS"));
	ATF_CHECK_STREQ("eth0", env_lookup(env, nenv, "CNI_IFNAME"));
	/* CNI_PATH points at the plugin bin dir. */
	ATF_CHECK(env_lookup(env, nenv, "CNI_PATH") != NULL);

	for (i = 0; i < nenv; i++)
		free(env[i]);
	free(env);
}

ATF_TC(parse_config);
ATF_TC_HEAD(parse_config, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "cni_parse_config reads the name and type from a netconf file");
}
ATF_TC_BODY(parse_config, tc)
{
	struct cni_config *cfg = NULL;
	const char *path = "cni-netconf.json";
	FILE *f;

	f = fopen(path, "w");
	ATF_REQUIRE(f != NULL);
	fputs("{\n"
	    "  \"cniVersion\": \"0.4.0\",\n"
	    "  \"name\": \"ocifbsd-cni-test\",\n"
	    "  \"type\": \"bridge\"\n"
	    "}\n", f);
	fclose(f);

	ATF_REQUIRE_EQ(0, cni_parse_config(path, &cfg));
	ATF_REQUIRE(cfg != NULL);
	ATF_CHECK_STREQ("ocifbsd-cni-test", cfg->network_name);
	ATF_CHECK_STREQ("bridge", cfg->type);

	free(cfg->type);
	free(cfg->network_name);
	free(cfg);
}

ATF_TC(parse_config_missing);
ATF_TC_HEAD(parse_config_missing, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "cni_parse_config fails on a nonexistent path");
}
ATF_TC_BODY(parse_config_missing, tc)
{
	struct cni_config *cfg = NULL;

	ATF_CHECK(cni_parse_config("/no/such/cni.json", &cfg) != 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, build_env);
	ATF_TP_ADD_TC(tp, parse_config);
	ATF_TP_ADD_TC(tp, parse_config_missing);

	return (atf_no_error());
}
