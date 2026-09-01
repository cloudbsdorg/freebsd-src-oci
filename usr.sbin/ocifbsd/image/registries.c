/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Registry alias table: maps a registry reference name to its API host and
 * token endpoint. Built-in defaults cover Docker Hub so pulls work out of the
 * box; a user-editable configuration file overrides and extends them. See
 * registries.h for the file format and search order.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "registries.h"

struct alias_entry {
	struct registry_alias	 a;
	struct alias_entry	*next;
};

static struct alias_entry	*g_head;
static int			 g_loaded;

/*
 * Built-in defaults, applied before the configuration file so a file entry of
 * the same name overrides them.
 */
static const struct registry_alias g_defaults[] = {
	{ (char *)"docker.io", (char *)"registry-1.docker.io",
	  (char *)"https://auth.docker.io/token", (char *)"registry.docker.io",
	  0 },
	{ (char *)"index.docker.io", (char *)"registry-1.docker.io",
	  (char *)"https://auth.docker.io/token", (char *)"registry.docker.io",
	  0 },
};

/* strdup that maps "-" and empty to NULL (absent optional field). */
static char *
dup_field(const char *s)
{
	if (s == NULL || s[0] == '\0' || (s[0] == '-' && s[1] == '\0'))
		return (NULL);
	return (strdup(s));
}

/* Find an existing entry by name, or NULL. */
static struct alias_entry *
find_entry(const char *name)
{
	struct alias_entry *e;

	for (e = g_head; e != NULL; e = e->next)
		if (e->a.name != NULL && strcmp(e->a.name, name) == 0)
			return (e);
	return (NULL);
}

/*
 * Insert or update an alias. On update, existing strings are freed and
 * replaced. Silently ignores allocation failure (the entry is simply absent).
 */
/* Interpret the optional "tls" column. Returns 1 (insecure) or 0 (secure). */
static int
parse_insecure(const char *tls)
{
	if (tls == NULL)
		return (0);
	if (strcasecmp(tls, "http") == 0 || strcasecmp(tls, "insecure") == 0 ||
	    strcasecmp(tls, "true") == 0 || strcmp(tls, "1") == 0)
		return (1);
	return (0);
}

static void
upsert(const char *name, const char *api_host, const char *realm,
    const char *service, int insecure)
{
	struct alias_entry *e;

	if (name == NULL || name[0] == '\0')
		return;

	e = find_entry(name);
	if (e == NULL) {
		e = calloc(1, sizeof(*e));
		if (e == NULL)
			return;
		e->a.name = strdup(name);
		if (e->a.name == NULL) {
			free(e);
			return;
		}
		e->next = g_head;
		g_head = e;
	} else {
		free(e->a.api_host);
		free(e->a.auth_realm);
		free(e->a.auth_service);
	}
	e->a.api_host = dup_field(api_host);
	e->a.auth_realm = dup_field(realm);
	e->a.auth_service = dup_field(service);
	e->a.insecure = insecure;
}

/*
 * Extract the host portion of a registry reference (strip any :port and the
 * brackets around an IPv6 literal) into buf.
 */
static void
host_only(const char *name, char *buf, size_t buflen)
{
	const char *start = name;
	const char *end;

	if (name[0] == '[') {			/* [::1]:5000 */
		start = name + 1;
		end = strchr(start, ']');
		if (end == NULL)
			end = start + strlen(start);
	} else {
		end = strrchr(name, ':');	/* host:port */
		if (end == NULL)
			end = name + strlen(name);
	}
	{
		size_t n = (size_t)(end - start);
		if (n >= buflen)
			n = buflen - 1;
		memcpy(buf, start, n);
		buf[n] = '\0';
	}
}

/* Loopback / localhost hosts are insecure by default (Podman behavior). */
static int
is_localhost(const char *name)
{
	char host[256];
	size_t n;

	host_only(name, host, sizeof(host));
	if (strcasecmp(host, "localhost") == 0 ||
	    strcmp(host, "::1") == 0 ||
	    strncmp(host, "127.", 4) == 0)
		return (1);
	n = strlen(host);
	if (n >= 10 && strcasecmp(host + n - 10, ".localhost") == 0)
		return (1);
	return (0);
}

/* Return the next whitespace-delimited token in *sp, advancing *sp. */
static char *
next_token(char **sp)
{
	char *s = *sp;
	char *start;

	while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
		s++;
	if (*s == '\0') {
		*sp = s;
		return (NULL);
	}
	start = s;
	while (*s != '\0' && *s != ' ' && *s != '\t' && *s != '\r' &&
	    *s != '\n')
		s++;
	if (*s != '\0')
		*s++ = '\0';
	*sp = s;
	return (start);
}

/* Parse one config line into the table. */
static void
parse_line(char *line)
{
	char *p = line;
	char *name, *host, *realm, *service, *tls;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '#' || *p == '\0' || *p == '\n')
		return;

	name = next_token(&p);
	host = next_token(&p);
	realm = next_token(&p);
	service = next_token(&p);
	tls = next_token(&p);
	if (name == NULL || host == NULL)
		return;
	upsert(name, host, realm, service, parse_insecure(tls));
}

/* Resolve the configuration file path per the documented search order. */
static const char *
config_path(void)
{
	const char *env;

	env = getenv("OCIFBSD_REGISTRIES_CONF");
	if (env != NULL && env[0] != '\0')
		return (env);
	if (access("/usr/local/etc/ocifbsd/registries.conf", R_OK) == 0)
		return ("/usr/local/etc/ocifbsd/registries.conf");
	if (access("/etc/ocifbsd/registries.conf", R_OK) == 0)
		return ("/etc/ocifbsd/registries.conf");
	return (NULL);
}

static void
load_config_file(void)
{
	const char *path;
	FILE *f;
	char buf[1024];

	path = config_path();
	if (path == NULL)
		return;
	f = fopen(path, "r");
	if (f == NULL)
		return;
	while (fgets(buf, sizeof(buf), f) != NULL)
		parse_line(buf);
	fclose(f);
}

static void
load_once(void)
{
	size_t i;

	if (g_loaded)
		return;
	g_loaded = 1;

	for (i = 0; i < sizeof(g_defaults) / sizeof(g_defaults[0]); i++)
		upsert(g_defaults[i].name, g_defaults[i].api_host,
		    g_defaults[i].auth_realm, g_defaults[i].auth_service,
		    g_defaults[i].insecure);

	load_config_file();
}

const struct registry_alias *
registry_alias_lookup(const char *name)
{
	struct alias_entry *e;

	if (name == NULL)
		return (NULL);
	load_once();
	e = find_entry(name);
	return (e != NULL ? &e->a : NULL);
}

const struct registry_alias *
registry_alias_by_host(const char *host)
{
	struct alias_entry *e;

	if (host == NULL)
		return (NULL);
	load_once();
	for (e = g_head; e != NULL; e = e->next)
		if (e->a.api_host != NULL && strcmp(e->a.api_host, host) == 0)
			return (&e->a);
	return (NULL);
}

int
registry_alias_insecure(const char *name)
{
	struct alias_entry *e;

	if (name == NULL)
		return (0);
	load_once();

	/* An explicit config/default entry is authoritative. */
	e = find_entry(name);
	if (e != NULL)
		return (e->a.insecure);

	/* Otherwise localhost/loopback is insecure by default. */
	return (is_localhost(name));
}
