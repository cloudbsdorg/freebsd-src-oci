/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 CloudBSD
 *
 * Pure path helpers for the ocifbsd image store (no ZFS CLI).
 */

#include <sys/param.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zfs_store.h"

static const char *mountpoint_base = "/var/lib/ocifbsd";

const char *
zfs_paths_mountpoint_base(void)
{
	return (mountpoint_base);
}

char *
zfs_image_path(const char *registry, const char *repo, const char *tag)
{
	char *path;
	size_t len;

	if (registry == NULL || repo == NULL || tag == NULL)
		return (NULL);

	len = strlen(mountpoint_base) + 1 +
	    strlen(registry) + 1 +
	    strlen(repo) + 1 +
	    strlen(tag) + 1;

	path = malloc(len);
	if (path == NULL)
		return (NULL);

	snprintf(path, len, "%s/%s/%s/%s", mountpoint_base,
	    registry, repo, tag);

	return (path);
}

char *
zfs_layer_path(const char *digest)
{
	char *path;
	size_t len;

	if (digest == NULL)
		return (NULL);

	len = strlen(mountpoint_base) + 1 +
	    strlen("layers") + 1 +
	    strlen(digest) + 1;

	path = malloc(len);
	if (path == NULL)
		return (NULL);

	snprintf(path, len, "%s/layers/%s", mountpoint_base, digest);

	return (path);
}

char *
zfs_volume_path(const char *name)
{
	char *path;
	size_t len;

	if (name == NULL)
		return (NULL);

	len = strlen(mountpoint_base) + 1 +
	    strlen("volumes") + 1 +
	    strlen(name) + 1;

	path = malloc(len);
	if (path == NULL)
		return (NULL);

	snprintf(path, len, "%s/volumes/%s", mountpoint_base, name);

	return (path);
}
