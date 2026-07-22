/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 CloudBSD
 *
 * Pure path helpers for the ocifbsd image store (no ZFS CLI).
 * Override base with OCIFBSD_DATA_DIR for non-root / test installs.
 */

#include <sys/param.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "zfs_store.h"

#define DEFAULT_MOUNTPOINT_BASE	"/var/lib/ocifbsd"

static const char *
mountpoint_base(void)
{
	const char *e;

	e = getenv("OCIFBSD_DATA_DIR");
	if (e != NULL && e[0] != '\0')
		return (e);
	return (DEFAULT_MOUNTPOINT_BASE);
}

char *
zfs_image_path(const char *registry, const char *repo, const char *tag)
{
	char *path;
	size_t len;
	const char *base;

	if (registry == NULL || repo == NULL || tag == NULL)
		return (NULL);

	base = mountpoint_base();
	len = strlen(base) + 1 +
	    strlen(registry) + 1 +
	    strlen(repo) + 1 +
	    strlen(tag) + 1;

	path = malloc(len);
	if (path == NULL)
		return (NULL);

	snprintf(path, len, "%s/%s/%s/%s", base, registry, repo, tag);
	return (path);
}

char *
zfs_layer_path(const char *digest)
{
	char *path;
	size_t len;
	const char *base;

	if (digest == NULL)
		return (NULL);

	base = mountpoint_base();
	len = strlen(base) + 1 + strlen("layers") + 1 + strlen(digest) + 1;

	path = malloc(len);
	if (path == NULL)
		return (NULL);

	snprintf(path, len, "%s/layers/%s", base, digest);
	return (path);
}

char *
zfs_volume_path(const char *name)
{
	char *path;
	size_t len;
	const char *base;

	if (name == NULL)
		return (NULL);

	base = mountpoint_base();
	len = strlen(base) + 1 + strlen("volumes") + 1 + strlen(name) + 1;

	path = malloc(len);
	if (path == NULL)
		return (NULL);

	snprintf(path, len, "%s/volumes/%s", base, name);
	return (path);
}
