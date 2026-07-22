/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 CloudBSD
 *
 * Unit tests for image/reference.c (no network).
 */

#include <sys/param.h>

#include <atf-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "image/pull.h"
#include "image/zfs_store.h"
#include "image/reference.c"
#include "image/paths.c"

ATF_TC(ref_docker_hub_short);
ATF_TC_HEAD(ref_docker_hub_short, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "namespace/name defaults to docker.io");
}
ATF_TC_BODY(ref_docker_hub_short, tc)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;

	ATF_REQUIRE_EQ(parse_reference("freebsd/freebsd:14.0",
	    &registry, &repo, &tag, &digest), 0);
	ATF_REQUIRE(registry != NULL);
	ATF_CHECK_STREQ(registry, "docker.io");
	ATF_REQUIRE(repo != NULL);
	ATF_CHECK_STREQ(repo, "freebsd/freebsd");
	ATF_REQUIRE(tag != NULL);
	ATF_CHECK_STREQ(tag, "14.0");
	free(registry);
	free(repo);
	free(tag);
	free(digest);
}

ATF_TC(ref_explicit_registry);
ATF_TC_HEAD(ref_explicit_registry, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "ghcr.io/ns/img:tag parses registry and tag");
}
ATF_TC_BODY(ref_explicit_registry, tc)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;

	ATF_REQUIRE_EQ(parse_reference(
	    "ghcr.io/cloudbsd/ocifbsd:latest",
	    &registry, &repo, &tag, &digest), 0);
	ATF_REQUIRE(registry != NULL);
	ATF_CHECK_STREQ(registry, "ghcr.io");
	ATF_REQUIRE(repo != NULL);
	ATF_CHECK_STREQ(repo, "cloudbsd/ocifbsd");
	ATF_REQUIRE(tag != NULL);
	ATF_CHECK_STREQ(tag, "latest");
	free(registry);
	free(repo);
	free(tag);
	free(digest);
}

ATF_TC(ref_localhost);
ATF_TC_HEAD(ref_localhost, tc)
{
	atf_tc_set_md_var(tc, "descr", "localhost is treated as registry");
}
ATF_TC_BODY(ref_localhost, tc)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;

	ATF_REQUIRE_EQ(parse_reference("localhost:5000/my/app:dev",
	    &registry, &repo, &tag, &digest), 0);
	ATF_REQUIRE(registry != NULL);
	ATF_CHECK(strstr(registry, "localhost") != NULL);
	ATF_REQUIRE(tag != NULL);
	ATF_CHECK_STREQ(tag, "dev");
	free(registry);
	free(repo);
	free(tag);
	free(digest);
}

ATF_TC(ref_null);
ATF_TC_HEAD(ref_null, tc)
{
	atf_tc_set_md_var(tc, "descr", "NULL/empty reference fails");
}
ATF_TC_BODY(ref_null, tc)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;

	ATF_CHECK(parse_reference(NULL, &registry, &repo, &tag, &digest) != 0);
	ATF_CHECK(parse_reference("", &registry, &repo, &tag, &digest) != 0);
}

ATF_TC(ref_default_tag);
ATF_TC_HEAD(ref_default_tag, tc)
{
	atf_tc_set_md_var(tc, "descr", "missing tag becomes latest");
}
ATF_TC_BODY(ref_default_tag, tc)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;

	ATF_REQUIRE_EQ(parse_reference("ghcr.io/a/b",
	    &registry, &repo, &tag, &digest), 0);
	ATF_CHECK_STREQ(tag, "latest");
	free(registry);
	free(repo);
	free(tag);
	free(digest);
}

ATF_TC(ref_canonical);
ATF_TC_HEAD(ref_canonical, tc)
{
	atf_tc_set_md_var(tc, "descr", "canonicalize_reference yields repo:tag");
}
ATF_TC_BODY(ref_canonical, tc)
{
	char *c = NULL;

	ATF_REQUIRE_EQ(canonicalize_reference("ghcr.io/x/y:1", &c), 0);
	ATF_REQUIRE(c != NULL);
	ATF_CHECK_STREQ(c, "x/y:1");
	free(c);
}

ATF_TC(ref_docker_hub_official);
ATF_TC_HEAD(ref_docker_hub_official, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "single-component docker.io name maps to library/");
}
ATF_TC_BODY(ref_docker_hub_official, tc)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;

	ATF_REQUIRE_EQ(parse_reference("hello-world:latest",
	    &registry, &repo, &tag, &digest), 0);
	ATF_CHECK_STREQ(registry, "docker.io");
	ATF_CHECK_STREQ(repo, "library/hello-world");
	ATF_CHECK_STREQ(tag, "latest");
	free(registry);
	free(repo);
	free(tag);
	free(digest);

	ATF_REQUIRE_EQ(parse_reference("docker.io/alpine",
	    &registry, &repo, &tag, &digest), 0);
	ATF_CHECK_STREQ(repo, "library/alpine");
	free(registry);
	free(repo);
	free(tag);
	free(digest);
}

ATF_TC(zfs_paths);
ATF_TC_HEAD(zfs_paths, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "zfs_image/layer/volume_path under /var/lib/ocifbsd");
}
ATF_TC_BODY(zfs_paths, tc)
{
	char *p;

	unsetenv("OCIFBSD_DATA_DIR");
	p = zfs_image_path("ghcr.io", "cloudbsd/ocifbsd", "latest");
	ATF_REQUIRE(p != NULL);
	ATF_CHECK_STREQ(p, "/var/lib/ocifbsd/ghcr.io/cloudbsd/ocifbsd/latest");
	free(p);

	ATF_REQUIRE(setenv("OCIFBSD_DATA_DIR", "/tmp/oci-store", 1) == 0);
	p = zfs_image_path("ghcr.io", "cloudbsd/ocifbsd", "latest");
	ATF_REQUIRE(p != NULL);
	ATF_CHECK_STREQ(p,
	    "/tmp/oci-store/ghcr.io/cloudbsd/ocifbsd/latest");
	free(p);
	unsetenv("OCIFBSD_DATA_DIR");

	p = zfs_layer_path("sha256:deadbeef");
	ATF_REQUIRE(p != NULL);
	ATF_CHECK_STREQ(p, "/var/lib/ocifbsd/layers/sha256:deadbeef");
	free(p);

	p = zfs_volume_path("data");
	ATF_REQUIRE(p != NULL);
	ATF_CHECK_STREQ(p, "/var/lib/ocifbsd/volumes/data");
	free(p);

	ATF_CHECK(zfs_image_path(NULL, "a", "b") == NULL);
	ATF_CHECK(zfs_layer_path(NULL) == NULL);
}

ATF_TC(ref_digest);
ATF_TC_HEAD(ref_digest, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "reference with @sha256 digest parses digest field");
}
ATF_TC_BODY(ref_digest, tc)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;

	ATF_REQUIRE_EQ(parse_reference(
	    "ghcr.io/cloudbsd/ocifbsd@sha256:deadbeef",
	    &registry, &repo, &tag, &digest), 0);
	ATF_CHECK_STREQ(registry, "ghcr.io");
	ATF_CHECK_STREQ(repo, "cloudbsd/ocifbsd");
	ATF_REQUIRE(digest != NULL);
	ATF_CHECK_STREQ(digest, "sha256:deadbeef");
	free(registry);
	free(repo);
	free(tag);
	free(digest);
}

ATF_TC(ref_tag_and_digest);
ATF_TC_HEAD(ref_tag_and_digest, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "tag and @digest together: tag before @");
}
ATF_TC_BODY(ref_tag_and_digest, tc)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;

	ATF_REQUIRE_EQ(parse_reference(
	    "docker.io/library/alpine:3.19@sha256:abc",
	    &registry, &repo, &tag, &digest), 0);
	ATF_CHECK_STREQ(repo, "library/alpine");
	ATF_CHECK_STREQ(tag, "3.19");
	ATF_CHECK_STREQ(digest, "sha256:abc");
	free(registry);
	free(repo);
	free(tag);
	free(digest);
}

ATF_TC(ref_port_registry);
ATF_TC_HEAD(ref_port_registry, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "registry host:port is kept in registry field");
}
ATF_TC_BODY(ref_port_registry, tc)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;

	ATF_REQUIRE_EQ(parse_reference("127.0.0.1:5000/ns/img:v1",
	    &registry, &repo, &tag, &digest), 0);
	ATF_CHECK_STREQ(registry, "127.0.0.1:5000");
	ATF_CHECK_STREQ(repo, "ns/img");
	ATF_CHECK_STREQ(tag, "v1");
	free(registry);
	free(repo);
	free(tag);
	free(digest);
}

ATF_TC(ref_no_library_for_ns);
ATF_TC_HEAD(ref_no_library_for_ns, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "namespaced docker hub repos do not get library/ prefix");
}
ATF_TC_BODY(ref_no_library_for_ns, tc)
{
	char *registry = NULL, *repo = NULL, *tag = NULL, *digest = NULL;

	ATF_REQUIRE_EQ(parse_reference("library/hello-world:latest",
	    &registry, &repo, &tag, &digest), 0);
	ATF_CHECK_STREQ(repo, "library/hello-world");
	free(registry);
	free(repo);
	free(tag);
	free(digest);
}

ATF_TC(ref_stress_many);
ATF_TC_HEAD(ref_stress_many, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "parse_reference survives 1000 iterations without leak crash");
}
ATF_TC_BODY(ref_stress_many, tc)
{
	int i;
	char *registry, *repo, *tag, *digest;

	for (i = 0; i < 1000; i++) {
		registry = repo = tag = digest = NULL;
		ATF_REQUIRE_EQ(parse_reference("hello-world:latest",
		    &registry, &repo, &tag, &digest), 0);
		ATF_CHECK_STREQ(repo, "library/hello-world");
		free(registry);
		free(repo);
		free(tag);
		free(digest);
	}
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, ref_docker_hub_short);
	ATF_TP_ADD_TC(tp, ref_explicit_registry);
	ATF_TP_ADD_TC(tp, ref_localhost);
	ATF_TP_ADD_TC(tp, ref_null);
	ATF_TP_ADD_TC(tp, ref_default_tag);
	ATF_TP_ADD_TC(tp, ref_canonical);
	ATF_TP_ADD_TC(tp, ref_docker_hub_official);
	ATF_TP_ADD_TC(tp, zfs_paths);
	ATF_TP_ADD_TC(tp, ref_digest);
	ATF_TP_ADD_TC(tp, ref_tag_and_digest);
	ATF_TP_ADD_TC(tp, ref_port_registry);
	ATF_TP_ADD_TC(tp, ref_no_library_for_ns);
	ATF_TP_ADD_TC(tp, ref_stress_many);
	return (atf_no_error());
}
