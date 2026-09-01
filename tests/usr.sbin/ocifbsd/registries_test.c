/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Unit tests for the registry alias table (image/registries.c).
 */

#include <atf-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Compile the module under test directly into the test binary. */
#include "registries.c"

/* Built-in defaults must resolve Docker Hub without any config file. */
ATF_TC_WITHOUT_HEAD(default_docker_hub);
ATF_TC_BODY(default_docker_hub, tc)
{
	const struct registry_alias *a;

	unsetenv("OCIFBSD_REGISTRIES_CONF");
	a = registry_alias_lookup("docker.io");
	ATF_REQUIRE_MSG(a != NULL, "docker.io should resolve from built-in defaults");
	ATF_CHECK_STREQ("registry-1.docker.io", a->api_host);
	ATF_CHECK_STREQ("https://auth.docker.io/token", a->auth_realm);
	ATF_CHECK_STREQ("registry.docker.io", a->auth_service);
}

/* index.docker.io is an alias for the same host. */
ATF_TC_WITHOUT_HEAD(default_index_alias);
ATF_TC_BODY(default_index_alias, tc)
{
	const struct registry_alias *a;

	unsetenv("OCIFBSD_REGISTRIES_CONF");
	a = registry_alias_lookup("index.docker.io");
	ATF_REQUIRE(a != NULL);
	ATF_CHECK_STREQ("registry-1.docker.io", a->api_host);
}

/* Reverse lookup by API host feeds the token-realm fallback. */
ATF_TC_WITHOUT_HEAD(lookup_by_host);
ATF_TC_BODY(lookup_by_host, tc)
{
	const struct registry_alias *a;

	unsetenv("OCIFBSD_REGISTRIES_CONF");
	a = registry_alias_by_host("registry-1.docker.io");
	ATF_REQUIRE(a != NULL);
	ATF_CHECK_STREQ("https://auth.docker.io/token", a->auth_realm);
}

/* An unknown registry is simply absent (caller uses the name as the host). */
ATF_TC_WITHOUT_HEAD(unknown_registry);
ATF_TC_BODY(unknown_registry, tc)
{
	unsetenv("OCIFBSD_REGISTRIES_CONF");
	ATF_CHECK(registry_alias_lookup("registry.unknown.example") == NULL);
}

/* A user config file adds/overrides entries. */
ATF_TC_WITHOUT_HEAD(config_file_override);
ATF_TC_BODY(config_file_override, tc)
{
	const struct registry_alias *a;
	FILE *f;

	f = fopen("registries.conf", "w");
	ATF_REQUIRE(f != NULL);
	fputs("# test registries\n"
	      "mirror.example   mirror.internal.example   -   -\n"
	      "docker.io        my-mirror.example         -   -\n",
	    f);
	fclose(f);
	setenv("OCIFBSD_REGISTRIES_CONF", "registries.conf", 1);

	a = registry_alias_lookup("mirror.example");
	ATF_REQUIRE_MSG(a != NULL, "config-file entry should be found");
	ATF_CHECK_STREQ("mirror.internal.example", a->api_host);

	/* A file entry overrides the built-in docker.io default. */
	a = registry_alias_lookup("docker.io");
	ATF_REQUIRE(a != NULL);
	ATF_CHECK_STREQ("my-mirror.example", a->api_host);

	unsetenv("OCIFBSD_REGISTRIES_CONF");
}

/* HTTPS is required by default for any ordinary registry. */
ATF_TC_WITHOUT_HEAD(secure_by_default);
ATF_TC_BODY(secure_by_default, tc)
{
	unsetenv("OCIFBSD_REGISTRIES_CONF");
	ATF_CHECK_EQ(0, registry_alias_insecure("registry.example.com"));
	ATF_CHECK_EQ(0, registry_alias_insecure("docker.io"));
}

/* localhost is allowed over plain HTTP without any configuration. */
ATF_TC_WITHOUT_HEAD(localhost_insecure_default);
ATF_TC_BODY(localhost_insecure_default, tc)
{
	unsetenv("OCIFBSD_REGISTRIES_CONF");
	ATF_CHECK_EQ(1, registry_alias_insecure("localhost"));
	ATF_CHECK_EQ(1, registry_alias_insecure("localhost:5000"));
	ATF_CHECK_EQ(1, registry_alias_insecure("127.0.0.1:5000"));
	ATF_CHECK_EQ(1, registry_alias_insecure("[::1]:5000"));
	ATF_CHECK_EQ(1, registry_alias_insecure("registry.localhost"));
}

/* A registry can be marked insecure (plain HTTP) in the config file. */
ATF_TC_WITHOUT_HEAD(config_insecure_override);
ATF_TC_BODY(config_insecure_override, tc)
{
	FILE *f;

	f = fopen("insecure.conf", "w");
	ATF_REQUIRE(f != NULL);
	fputs("# name            api_host          auth_realm auth_service tls\n"
	      "plain.example     plain.example     -          -            http\n"
	      "secure.example    secure.example    -          -            https\n",
	    f);
	fclose(f);
	setenv("OCIFBSD_REGISTRIES_CONF", "insecure.conf", 1);

	ATF_CHECK_EQ(1, registry_alias_insecure("plain.example"));
	ATF_CHECK_EQ(0, registry_alias_insecure("secure.example"));
	unsetenv("OCIFBSD_REGISTRIES_CONF");
}

/* The default registry for unqualified names is docker.io out of the box. */
ATF_TC_WITHOUT_HEAD(default_registry_builtin);
ATF_TC_BODY(default_registry_builtin, tc)
{
	unsetenv("OCIFBSD_REGISTRIES_CONF");
	ATF_CHECK_STREQ("docker.io", registry_default_name());
}

/* The default registry can be redirected in the config file. */
ATF_TC_WITHOUT_HEAD(default_registry_configured);
ATF_TC_BODY(default_registry_configured, tc)
{
	FILE *f;

	f = fopen("default.conf", "w");
	ATF_REQUIRE(f != NULL);
	fputs("default-registry   registry.internal.example\n"
	      "registry.internal.example   registry.internal.example   -   -   https\n",
	    f);
	fclose(f);
	setenv("OCIFBSD_REGISTRIES_CONF", "default.conf", 1);

	ATF_CHECK_STREQ("registry.internal.example", registry_default_name());
	unsetenv("OCIFBSD_REGISTRIES_CONF");
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, default_docker_hub);
	ATF_TP_ADD_TC(tp, default_registry_builtin);
	ATF_TP_ADD_TC(tp, default_registry_configured);
	ATF_TP_ADD_TC(tp, default_index_alias);
	ATF_TP_ADD_TC(tp, lookup_by_host);
	ATF_TP_ADD_TC(tp, unknown_registry);
	ATF_TP_ADD_TC(tp, config_file_override);
	ATF_TP_ADD_TC(tp, secure_by_default);
	ATF_TP_ADD_TC(tp, localhost_insecure_default);
	ATF_TP_ADD_TC(tp, config_insecure_override);
	return (atf_no_error());
}
