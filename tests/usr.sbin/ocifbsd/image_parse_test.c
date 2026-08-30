/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Unit tests for pure/network-free image parse + digest helpers in
 * image/pull.c (manifest, config, verify_layer, compute_digest).
 */

#include <sys/param.h>
#include <sys/stat.h>

#include <atf-c.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "image/pull.h"
#include "image/zfs_store.h"
#include "image/reference.c"
#include "image/paths.c"
#include "image/whiteout.c"
#include "image/unpack.c"
#include "image/pull.c"

/* Minimal Docker v2 / OCI image manifest with one layer */
static const char *MANIFEST_V2 =
    "{"
    "\"schemaVersion\":2,"
    "\"mediaType\":\"application/vnd.docker.distribution.manifest.v2+json\","
    "\"config\":{"
    "  \"mediaType\":\"application/vnd.docker.container.image.v1+json\","
    "  \"size\":100,"
    "  \"digest\":\"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\""
    "},"
    "\"layers\":[{"
    "  \"mediaType\":\"application/vnd.docker.image.rootfs.diff.tar.gzip\","
    "  \"size\":42,"
    "  \"digest\":\"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\""
    "}]"
    "}";

/* Image config with Entrypoint + Cmd + Env */
static const char *IMAGE_CONFIG =
    "{"
    "\"architecture\":\"amd64\","
    "\"os\":\"linux\","
    "\"config\":{"
    "  \"Env\":[\"PATH=/bin\",\"FOO=bar\"],"
    "  \"Entrypoint\":[\"/bin/sh\",\"-c\"],"
    "  \"Cmd\":[\"echo hi\"],"
    "  \"WorkingDir\":\"/work\","
    "  \"User\":\"0:0\""
    "}"
    "}";

static const char *MANIFEST_BAD = "{ not json";
static const char *MANIFEST_EMPTY_LAYERS =
    "{"
    "\"schemaVersion\":2,"
    "\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\","
    "\"config\":{\"digest\":\"sha256:cc\"},"
    "\"layers\":[]"
    "}";

ATF_TC(parse_manifest_v2);
ATF_TC_HEAD(parse_manifest_v2, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "parse_manifest accepts Docker v2 manifest with layers");
}
ATF_TC_BODY(parse_manifest_v2, tc)
{
	struct oci_manifest *m = NULL;

	ATF_REQUIRE_EQ(parse_manifest(MANIFEST_V2, strlen(MANIFEST_V2), &m),
	    0);
	ATF_REQUIRE(m != NULL);
	ATF_REQUIRE(m->config != NULL);
	ATF_REQUIRE(m->config->config != NULL);
	ATF_CHECK(strstr(m->config->config, "sha256:aaa") != NULL);
	ATF_REQUIRE_EQ(m->nlayers, 1);
	ATF_REQUIRE(m->layers != NULL);
	ATF_REQUIRE(m->layers[0] != NULL);
	ATF_CHECK_STREQ(m->layers[0]->digest,
	    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
	ATF_CHECK_EQ(m->layers[0]->size, 42);
	free_manifest(m);
}

ATF_TC(parse_manifest_bad_json);
ATF_TC_HEAD(parse_manifest_bad_json, tc)
{
	atf_tc_set_md_var(tc, "descr", "parse_manifest rejects invalid JSON");
}
ATF_TC_BODY(parse_manifest_bad_json, tc)
{
	struct oci_manifest *m = NULL;

	ATF_CHECK(parse_manifest(MANIFEST_BAD, strlen(MANIFEST_BAD), &m) != 0);
	ATF_CHECK(m == NULL);
}

ATF_TC(parse_manifest_empty_layers);
ATF_TC_HEAD(parse_manifest_empty_layers, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "parse_manifest allows empty layers array");
}
ATF_TC_BODY(parse_manifest_empty_layers, tc)
{
	struct oci_manifest *m = NULL;

	ATF_REQUIRE_EQ(parse_manifest(MANIFEST_EMPTY_LAYERS,
	    strlen(MANIFEST_EMPTY_LAYERS), &m), 0);
	ATF_REQUIRE(m != NULL);
	ATF_CHECK_EQ(m->nlayers, 0);
	free_manifest(m);
}

ATF_TC(parse_manifest_null);
ATF_TC_HEAD(parse_manifest_null, tc)
{
	atf_tc_set_md_var(tc, "descr", "parse_manifest null/empty input fails");
}
ATF_TC_BODY(parse_manifest_null, tc)
{
	struct oci_manifest *m = NULL;

	/* empty string is not valid JSON object for our purposes */
	ATF_CHECK(parse_manifest("", 0, &m) != 0);
}

ATF_TC(parse_config_entrypoint_cmd);
ATF_TC_HEAD(parse_config_entrypoint_cmd, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "parse_config merges Entrypoint+Cmd and Env/WorkingDir");
}
ATF_TC_BODY(parse_config_entrypoint_cmd, tc)
{
	struct oci_config *c = NULL;

	ATF_REQUIRE_EQ(parse_config(IMAGE_CONFIG, strlen(IMAGE_CONFIG), &c),
	    0);
	ATF_REQUIRE(c != NULL);
	ATF_CHECK_STREQ(c->architecture, "amd64");
	ATF_CHECK_STREQ(c->os, "linux");
	ATF_CHECK_STREQ(c->workdir, "/work");
	ATF_CHECK_STREQ(c->user, "0:0");
	ATF_REQUIRE(c->env != NULL);
	ATF_CHECK_STREQ(c->env[0], "PATH=/bin");
	ATF_CHECK_STREQ(c->env[1], "FOO=bar");
	ATF_REQUIRE(c->cmd != NULL);
	/* Entrypoint then Cmd */
	ATF_CHECK_STREQ(c->cmd[0], "/bin/sh");
	ATF_CHECK_STREQ(c->cmd[1], "-c");
	ATF_CHECK_STREQ(c->cmd[2], "echo hi");
	ATF_CHECK(c->cmd[3] == NULL);
	ATF_REQUIRE(c->config != NULL);
	free_config(c);
}

ATF_TC(parse_config_bad);
ATF_TC_HEAD(parse_config_bad, tc)
{
	atf_tc_set_md_var(tc, "descr", "parse_config rejects bad JSON");
}
ATF_TC_BODY(parse_config_bad, tc)
{
	struct oci_config *c = NULL;

	ATF_CHECK(parse_config("{", 1, &c) != 0);
}

ATF_TC(compute_and_verify_digest);
ATF_TC_HEAD(compute_and_verify_digest, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "compute_digest + verify_layer match known content");
}
ATF_TC_BODY(compute_and_verify_digest, tc)
{
	char path[PATH_MAX];
	char *digest = NULL;
	char expected[80];
	int fd;
	const char *payload = "ocifbsd-digest-fixture\n";

	snprintf(path, sizeof(path), "%s/layer.bin",
	    atf_tc_get_config_var(tc, "srcdir"));
	/* Write into work dir */
	snprintf(path, sizeof(path), "fixture.layer");
	fd = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
	ATF_REQUIRE(fd >= 0);
	ATF_REQUIRE(write(fd, payload, strlen(payload)) ==
	    (ssize_t)strlen(payload));
	close(fd);

	ATF_REQUIRE_EQ(compute_digest(path, &digest), 0);
	ATF_REQUIRE(digest != NULL);
	ATF_CHECK_EQ(strlen(digest), 64);

	snprintf(expected, sizeof(expected), "sha256:%s", digest);
	ATF_CHECK_EQ(verify_layer(path, expected), 0);

	/* Wrong digest must fail */
	ATF_CHECK(verify_layer(path,
	    "sha256:0000000000000000000000000000000000000000000000000000000000000000")
	    != 0);

	/* Missing colon format fails */
	ATF_CHECK(verify_layer(path, digest) != 0);

	free(digest);
	unlink(path);
}

ATF_TC(verify_layer_missing_file);
ATF_TC_HEAD(verify_layer_missing_file, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "verify_layer fails on missing path");
}
ATF_TC_BODY(verify_layer_missing_file, tc)
{
	ATF_CHECK(verify_layer("/nonexistent/ocifbsd-layer-missing",
	    "sha256:deadbeef") != 0);
}

ATF_TC(registry_init_docker_hub);
ATF_TC_HEAD(registry_init_docker_hub, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "registry_init remaps docker.io to registry-1 and library/");
}
ATF_TC_BODY(registry_init_docker_hub, tc)
{
	struct registry reg;

	ATF_REQUIRE_EQ(registry_init(&reg, "hello-world:latest"), 0);
	ATF_REQUIRE(reg.host != NULL);
	ATF_CHECK_STREQ(reg.host, "registry-1.docker.io");
	ATF_REQUIRE(reg.repository != NULL);
	ATF_CHECK_STREQ(reg.repository, "library/hello-world");
	ATF_REQUIRE(reg.tag != NULL);
	ATF_CHECK_STREQ(reg.tag, "latest");
	ATF_REQUIRE(reg.auth != NULL);
	ATF_REQUIRE(reg.auth->service != NULL);
	ATF_CHECK_STREQ(reg.auth->service, "registry.docker.io");
	registry_free(&reg);
}

ATF_TC(registry_init_invalid);
ATF_TC_HEAD(registry_init_invalid, tc)
{
	atf_tc_set_md_var(tc, "descr", "registry_init fails on empty ref");
}
ATF_TC_BODY(registry_init_invalid, tc)
{
	struct registry reg;

	ATF_CHECK(registry_init(&reg, "") != 0);
	ATF_CHECK(registry_init(&reg, NULL) != 0);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, parse_manifest_v2);
	ATF_TP_ADD_TC(tp, parse_manifest_bad_json);
	ATF_TP_ADD_TC(tp, parse_manifest_empty_layers);
	ATF_TP_ADD_TC(tp, parse_manifest_null);
	ATF_TP_ADD_TC(tp, parse_config_entrypoint_cmd);
	ATF_TP_ADD_TC(tp, parse_config_bad);
	ATF_TP_ADD_TC(tp, compute_and_verify_digest);
	ATF_TP_ADD_TC(tp, verify_layer_missing_file);
	ATF_TP_ADD_TC(tp, registry_init_docker_hub);
	ATF_TP_ADD_TC(tp, registry_init_invalid);
	return (atf_no_error());
}
