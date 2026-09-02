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

	ATF_REQUIRE_EQ(0, cni_build_env("ADD", "deadbeefcafe",
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

/* Write a file with the given content and mode. */
static void
write_file(const char *path, const char *content, mode_t mode)
{
	FILE *f = fopen(path, "w");
	ATF_REQUIRE(f != NULL);
	fputs(content, f);
	fclose(f);
	ATF_REQUIRE_EQ(0, chmod(path, mode));
}

ATF_TC(add_invokes_plugin);
ATF_TC_HEAD(add_invokes_plugin, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "cni_add execs the plugin with CNI_* env and the netconf on stdin, "
	    "and returns the plugin's result");
}
ATF_TC_BODY(add_invokes_plugin, tc)
{
	char cwd[PATH_MAX], bindir[PATH_MAX], confdir[PATH_MAX];
	char conf[PATH_MAX], plugin[PATH_MAX];
	char *result = NULL;
	FILE *mf;
	char line[256];
	int saw_add = 0, saw_cid = 0, saw_stdin = 0;

	ATF_REQUIRE(getcwd(cwd, sizeof(cwd)) != NULL);
	snprintf(bindir, sizeof(bindir), "%s/bin", cwd);
	snprintf(confdir, sizeof(confdir), "%s/conf", cwd);
	ATF_REQUIRE_EQ(0, mkdir(bindir, 0755));
	ATF_REQUIRE_EQ(0, mkdir(confdir, 0755));
	ATF_REQUIRE_EQ(0, setenv("OCIFBSD_CNI_BIN_DIR", bindir, 1));
	ATF_REQUIRE_EQ(0, setenv("OCIFBSD_CNI_CONF_DIR", confdir, 1));

	/*
	 * A mock CNI plugin: records the command, container id, and the netconf
	 * it received on stdin into marker.txt, then prints a CNI ADD result.
	 * The plugin's file name must equal the netconf "type".
	 */
	snprintf(plugin, sizeof(plugin), "%s/mockplugin", bindir);
	write_file(plugin,
	    "#!/bin/sh\n"
	    "cfg=$(cat)\n"
	    "{\n"
	    "  echo \"CMD=$CNI_COMMAND\"\n"
	    "  echo \"CID=$CNI_CONTAINERID\"\n"
	    "  echo \"IFNAME=$CNI_IFNAME\"\n"
	    "  echo \"STDIN=$cfg\"\n"
	    "} > marker.txt\n"
	    "echo '{\"cniVersion\":\"0.4.0\",\"interfaces\":[]}'\n",
	    0755);

	snprintf(conf, sizeof(conf), "%s/10-mock.conf", confdir);
	write_file(conf,
	    "{\"cniVersion\":\"0.4.0\",\"name\":\"mocknet\","
	    "\"type\":\"mockplugin\"}\n", 0644);

	ATF_REQUIRE_EQ(0,
	    cni_add("mocknet", "container-abc", "eth0", &result));
	ATF_REQUIRE(result != NULL);
	/* The plugin's stdout result came back to us. */
	ATF_CHECK(strstr(result, "cniVersion") != NULL);
	free(result);

	/* Verify the plugin actually received the env and stdin. */
	mf = fopen("marker.txt", "r");
	ATF_REQUIRE(mf != NULL);
	while (fgets(line, sizeof(line), mf) != NULL) {
		if (strstr(line, "CMD=ADD"))
			saw_add = 1;
		if (strstr(line, "CID=container-abc"))
			saw_cid = 1;
		if (strstr(line, "STDIN=") && strstr(line, "mockplugin"))
			saw_stdin = 1;
	}
	fclose(mf);
	ATF_CHECK_MSG(saw_add, "plugin did not receive CNI_COMMAND=ADD");
	ATF_CHECK_MSG(saw_cid, "plugin did not receive CNI_CONTAINERID");
	ATF_CHECK_MSG(saw_stdin, "plugin did not receive the netconf on stdin");
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, build_env);
	ATF_TP_ADD_TC(tp, parse_config);
	ATF_TP_ADD_TC(tp, parse_config_missing);
	ATF_TP_ADD_TC(tp, add_invokes_plugin);

	return (atf_no_error());
}
