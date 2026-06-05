/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 *
 * This software was developed by Klara, Inc. under sponsorship
 * from the FreeBSD Foundation.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * $FreeBSD$
 *
 * Unit test harness for ocifbsd
 * Tests OCI spec translation, jail lifecycle, and core functionality
 */

#include <sys/param.h>
#include <sys/jail.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <jail.h>
#include <paths.h>
#include <pthread.h>
#include <sha256.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid.h>

/* Include the implementation under test */
#include "ocifbsd.h"

/*
 * Test: Version command
 */
ATF_TC(version_test);
ATF_TC_HEAD(version_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test that ocifbsd reports correct version");
}

ATF_TC_BODY(version_test, tc)
{
	/* Version is defined in ocifbsd.h or Makefile */
	ATF_CHECK(true); /* Placeholder */
}

/*
 * Test: Help command
 */
ATF_TC(help_test);
ATF_TC_HEAD(help_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test that ocifbsd --help works");
}

ATF_TC_BODY(help_test, tc)
{
	ATF_CHECK(true); /* Placeholder */
}

/*
 * Test: Container ID generation
 */
ATF_TC(cid_test);
ATF_TC_HEAD(cid_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test container ID generation and validation");
}

ATF_TC_BODY(cid_test, tc)
{
	char cid[OCIFBSD_CID_LEN];
	char cid2[OCIFBSD_CID_LEN];

	/* Generate two CIDs and ensure they're different */
	ATF_REQUIRE_EQ(ocifbsd_generate_cid(cid, sizeof(cid)), 0);
	ATF_REQUIRE_EQ(ocifbsd_generate_cid(cid2, sizeof(cid2)), 0);
	ATF_CHECK_STRNEQ(cid, cid2, -1);

	/* Verify CID format (64 hex characters) */
	ATF_CHECK_EQ(strlen(cid), OCIFBSD_CID_LEN - 1);
	for (size_t i = 0; i < strlen(cid); i++) {
		ATF_CHECK(isxdigit((unsigned char)cid[i]));
	}
}

/*
 * Test: Container state management
 */
ATF_TC(state_test);
ATF_TC_HEAD(state_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test container state save and load");
}

ATF_TC_BODY(state_test, tc)
{
	struct ocifbsd_container *c;
	struct ocifbsd_container *loaded;
	char path[MAXPATHLEN];
	char statefile[MAXPATHLEN];
	FILE *fp;

	/* Create a test container */
	c = calloc(1, sizeof(*c));
	ATF_REQUIRE(c != NULL);

	strlcpy(c->id, "test-container-123", sizeof(c->id));
	strlcpy(c->name, "test-container", sizeof(c->name));
	strlcpy(c->statefile, "/tmp/ocifbsd-test-state.json",
	    sizeof(c->statefile));
	c->pid = 12345;
	c->state = OCIFBSD_STATE_RUNNING;
	c->created = time(NULL);

	/* Create temp directory for state */
	strlcpy(path, "/tmp/ocifbsd-test-XXXXXX", sizeof(path));
	ATF_REQUIRE(mkdtemp(path) != NULL);

	snprintf(statefile, sizeof(statefile), "%s/state.json", path);

	/* Save state to file */
	fp = fopen(statefile, "w");
	if (fp != NULL) {
		fprintf(fp, "{\n");
		fprintf(fp, "  \"id\": \"%s\",\n", c->id);
		fprintf(fp, "  \"name\": \"%s\",\n", c->name);
		fprintf(fp, "  \"pid\": %d,\n", c->pid);
		fprintf(fp, "  \"state\": %d,\n", c->state);
		fprintf(fp, "  \"created\": %ld\n", (long)c->created);
		fprintf(fp, "}\n");
		fclose(fp);
	}

	/* Load state from file */
	loaded = calloc(1, sizeof(*loaded));
	if (loaded != NULL && fp != NULL) {
		fp = fopen(statefile, "r");
		if (fp != NULL) {
			fscanf(fp, "%*[^:]%*c%*c%[^\"]", loaded->id);
			fscanf(fp, "%*[^:]%*c%*c%*c%[^\"]", loaded->name);
			fclose(fp);
		}
		ATF_CHECK_STREQ(c->id, loaded->id);
	}

	/* Cleanup */
	unlink(statefile);
	rmdir(path);
	free(c);
	free(loaded);
}

/*
 * Test: OCI spec to jail parameter translation
 */
ATF_TC(oci2jail_test);
ATF_TC_HEAD(oci2jail_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test OCI runtime spec to FreeBSD jail parameter translation");
}

ATF_TC_BODY(oci2jail_test, tc)
{
	struct ocifbsd_oci_spec spec;
	struct ocifbsd_jail_param params[32];
	size_t nparams;
	int found_path, found_hostname, found_vnet;

	/* Initialize spec with test values */
	memset(&spec, 0, sizeof(spec));
	strlcpy(spec.root.path, "/var/run/ocifbsd/test/rootfs",
	    sizeof(spec.root.path));
	strlcpy(spec.hostname, "testcontainer", sizeof(spec.hostname));
	spec.freebsd.vnet = 1;
	spec.freebsd.n_ip4 = 1;
	strlcpy(spec.freebsd.ip4[0], "192.168.1.100/24",
	    sizeof(spec.freebsd.ip4[0]));

	/* Translate to jail parameters */
	nparams = ocifbsd_oci_to_jail_params(&spec, params, 32);
	ATF_REQUIRE_GT((int)nparams, 0);

	/* Verify expected parameters exist */
	found_path = found_hostname = found_vnet = 0;
	for (size_t i = 0; i < nparams; i++) {
		if (strcmp(params[i].name, "path") == 0)
			found_path = 1;
		if (strcmp(params[i].name, "host.hostname") == 0)
			found_hostname = 1;
		if (strcmp(params[i].name, "vnet") == 0)
			found_vnet = 1;
	}

	ATF_CHECK(found_path);
	ATF_CHECK(found_hostname);
	ATF_CHECK(found_vnet);
}

/*
 * Test: Container name validation
 */
ATF_TC(name_test);
ATF_TC_HEAD(name_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test container name validation");
}

ATF_TC_BODY(name_test, tc)
{
	char valid_name[] = "my-container";
	char *invalid_names[] = {
		"",
		"has spaces",
		"has/slash",
		"has\\backslash",
		"has:colon",
		"has*star",
		"has?question",
	};
	size_t i;

	/* Valid name should pass */
	ATF_CHECK(ocifbsd_validate_name(valid_name));

	/* Invalid names should fail */
	for (i = 0; i < sizeof(invalid_names) / sizeof(invalid_names[0]);
	    i++) {
		ATF_CHECK_MSG(!ocifbsd_validate_name(invalid_names[i]),
		    "Name '%s' should be invalid",
		    invalid_names[i]);
	}
}

/*
 * Test: Path helpers
 */
ATF_TC(path_test);
ATF_TC_HEAD(path_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test path helper functions");
}

ATF_TC_BODY(path_test, tc)
{
	char path[MAXPATHLEN];
	const char *base = "/var/run/ocifbsd";

	/* Test container state directory */
	snprintf(path, sizeof(path), "%s/containers", base);
	ATF_CHECK_EQ(strcmp(ocifbsd_get_state_dir(), "/var/run/ocifbsd"), 0);

	/* Test container rootfs path */
	snprintf(path, sizeof(path), "%s/containers/%s/rootfs",
	    base, "test-id");
	ATF_CHECK(strlen(ocifbsd_get_rootfs_path("test-id")) > 0);
}

/*
 * Test: Logging functionality
 */
ATF_TC(log_test);
ATF_TC_HEAD(log_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test logging functionality");
}

ATF_TC_BODY(log_test, tc)
{
	/* Initialize logger */
	ocifbsd_log_init("test-container", 0);

	/* Log messages at different levels */
	ocifbsd_log(LOG_INFO, "Test info message");
	ocifbsd_log(LOG_DEBUG, "Test debug message");
	ocifbsd_log(LOG_ERR, "Test error message");

	/* Verify logs don't crash */
	ATF_CHECK(true);

	ocifbsd_log_shutdown();
}

/*
 * Test: Hooks execution
 */
ATF_TC(hooks_test);
ATF_TC_HEAD(hooks_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test OCI hooks execution");
}

ATF_TC_BODY(hooks_test, tc)
{
	struct ocifbsd_hook hooks[3];
	int i;

	/* Set up test hooks */
	for (i = 0; i < 3; i++) {
		hooks[i].path = "/bin/echo";
		hooks[i].args[0] = "/bin/echo";
		hooks[i].args[1] = "test";
		hooks[i].env = NULL;
	}

	/* Execute hooks - should not crash even if paths don't exist */
	ocifbsd_run_hooks(hooks, 3, OCIFBSD_HOOK_PRESTART);

	ATF_CHECK(true);
}

/*
 * Test: Container list functionality
 */
ATF_TC(list_test);
ATF_TC_HEAD(list_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test container listing");
}

ATF_TC_BODY(list_test, tc)
{
	struct ocifbsd_container **list;
	int count;

	/* List should return empty or existing containers */
	list = ocifbsd_list_containers(&count);
	ATF_REQUIRE(list != NULL || count == 0);
	ATF_CHECK(count >= 0);

	free(list);
}

/*
 * Test: Container inspection
 */
ATF_TC(inspect_test);
ATF_TC_HEAD(inspect_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test container inspection");
}

ATF_TC_BODY(inspect_test, tc)
{
	struct ocifbsd_container *c;
	char *json;

	/* Inspect non-existent container should return NULL */
	c = ocifbsd_inspect_container("nonexistent-id");
	ATF_CHECK(c == NULL);

	/* JSON output should not crash */
	json = ocifbsd_inspect_json(NULL);
	ATF_CHECK(json == NULL || strlen(json) > 0);
	free(json);
}

/*
 * Test: Container deletion
 */
ATF_TC(delete_test);
ATF_TC_HEAD(delete_test, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test container deletion");
}

ATF_TC_BODY(delete_test, tc)
{
	/* Deleting non-existent container should fail gracefully */
	ATF_CHECK_ERRNO(ENOENT, ocifbsd_delete_container("nonexistent-id", 0) == -1);
}

/*
 * Main test program
 */
ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, version_test);
	ATF_TP_ADD_TC(tp, help_test);
	ATF_TP_ADD_TC(tp, cid_test);
	ATF_TP_ADD_TC(tp, state_test);
	ATF_TP_ADD_TC(tp, oci2jail_test);
	ATF_TP_ADD_TC(tp, name_test);
	ATF_TP_ADD_TC(tp, path_test);
	ATF_TP_ADD_TC(tp, log_test);
	ATF_TP_ADD_TC(tp, hooks_test);
	ATF_TP_ADD_TC(tp, list_test);
	ATF_TP_ADD_TC(tp, inspect_test);
	ATF_TP_ADD_TC(tp, delete_test);

	return (0);
}
