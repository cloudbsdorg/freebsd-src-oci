/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Pure OCI/Docker reference parsing (no network).
 */

#include <sys/param.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pull.h"
#include "registries.h"

/*
 * True if the first path component looks like a registry host
 * (has a dot, is localhost, or host:port).
 */
static int
looks_like_registry(const char *p, const char *slash)
{
	const char *c;

	if (slash == NULL || slash <= p)
		return (0);
	if (strncmp(p, "localhost", 9) == 0 &&
	    (p[9] == '\0' || p[9] == ':' || p[9] == '/'))
		return (1);
	for (c = p; c < slash; c++) {
		if (*c == '.' || *c == ':')
			return (1);
	}
	return (0);
}

/*
 * Parse a Docker/OCI reference
 * Format: [registry/][namespace/]repository[:tag][@digest]
 */
int
parse_reference(const char *ref, char **registry, char **repo,
    char **tag, char **digest)
{
	const char *p = ref;
	const char *slash, *at;
	const char *repo_start, *repo_end;
	const char *colon = NULL;
	const char *scan;

	if (registry == NULL || repo == NULL || tag == NULL || digest == NULL) {
		errno = EINVAL;
		return (-1);
	}
	*registry = NULL;
	*repo = NULL;
	*tag = NULL;
	*digest = NULL;

	if (ref == NULL || ref[0] == '\0') {
		errno = EINVAL;
		return (-1);
	}

	slash = strchr(p, '/');
	at = strchr(p, '@');

	if (slash != NULL && looks_like_registry(p, slash)) {
		*registry = strndup(p, slash - p);
		repo_start = slash + 1;
	} else {
		/* Unqualified name: use the configured default registry. */
		*registry = strdup(registry_default_name());
		repo_start = p;
	}
	if (*registry == NULL)
		return (-1);

	/* Tag colon is the last ':' after repo_start, before '@' if any */
	for (scan = repo_start; *scan != '\0' && scan != at; scan++) {
		if (*scan == ':')
			colon = scan;
	}

	if (colon != NULL)
		repo_end = colon;
	else if (at != NULL)
		repo_end = at;
	else
		repo_end = ref + strlen(ref);

	if (repo_end < repo_start) {
		free(*registry);
		*registry = NULL;
		errno = EINVAL;
		return (-1);
	}

	*repo = strndup(repo_start, repo_end - repo_start);
	if (*repo == NULL) {
		free(*registry);
		*registry = NULL;
		return (-1);
	}

	if (colon != NULL) {
		const char *tag_end = at != NULL ? at : ref + strlen(ref);

		*tag = strndup(colon + 1, tag_end - (colon + 1));
	} else {
		*tag = strdup("latest");
	}
	if (*tag == NULL) {
		free(*registry);
		free(*repo);
		*registry = NULL;
		*repo = NULL;
		return (-1);
	}

	if (at != NULL) {
		*digest = strdup(at + 1);
		if (*digest == NULL) {
			free(*registry);
			free(*repo);
			free(*tag);
			*registry = NULL;
			*repo = NULL;
			*tag = NULL;
			return (-1);
		}
	}

	/*
	 * Docker Hub official images: single path component maps to
	 * library/<name> (e.g. hello-world -> library/hello-world).
	 */
	if (*repo != NULL && strchr(*repo, '/') == NULL &&
	    (strcmp(*registry, "docker.io") == 0 ||
	    strcmp(*registry, "index.docker.io") == 0 ||
	    strcmp(*registry, "registry-1.docker.io") == 0)) {
		char *librepo;

		if (asprintf(&librepo, "library/%s", *repo) < 0)
			return (-1);
		free(*repo);
		*repo = librepo;
	}

	return (0);
}

int
canonicalize_reference(const char *ref, char **canonical)
{
	char *registry, *repo, *tag, *digest;
	size_t len;
	int ret;

	if (canonical == NULL) {
		errno = EINVAL;
		return (-1);
	}
	*canonical = NULL;

	ret = parse_reference(ref, &registry, &repo, &tag, &digest);
	if (ret != 0)
		return (-1);

	len = strlen(repo) + 1 + strlen(tag) + 1;
	*canonical = malloc(len);
	if (*canonical == NULL) {
		ret = -1;
		goto cleanup;
	}

	snprintf(*canonical, len, "%s:%s", repo, tag);

cleanup:
	free(registry);
	free(repo);
	free(tag);
	free(digest);

	return (ret);
}
