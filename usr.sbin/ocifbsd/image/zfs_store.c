/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 * SPDX-License-Identifier: BSD-2-Clause
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
 * ZFS storage backend for OCI images
 */

#include <sys/param.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/sysctl.h>

#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <libgen.h>
#include <limits.h>
#include <sha256.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <libutil.h>

#include "zfs_store.h"

#include <stdarg.h>

extern int mkdirp(const char *path);
extern int copy_file(const char *from, const char *to, int mode);

/*
 * Configuration
 */
static const char *zfs_pool = OCIFBSD_ZFS_POOL;
static char *mountpoint_base = "/var/lib/ocifbsd";
static char zfs_cmd_buf[1024];

static int
run_zfs(int argc, ...)
{
	va_list ap;
	char **argv;
	pid_t pid;
	int status;
	int i, ret;

	argv = calloc(argc + 1, sizeof(char *));
	if (argv == NULL)
		return (-1);

	va_start(ap, argc);
	for (i = 0; i < argc; i++) {
		argv[i] = va_arg(ap, char *);
	}
	va_end(ap);
	argv[argc] = NULL;

	pid = fork();
	if (pid < 0) {
		free(argv);
		return (-1);
	}

	if (pid == 0) {
		/* Child */
		closefrom(STDERR_FILENO + 1);
		execvp("zfs", argv);
		_exit(127);
	}

	/* Parent */
	free(argv);
	ret = waitpid(pid, &status, 0);
	if (ret < 0)
		return (-1);

	if (WIFEXITED(status))
		return (WEXITSTATUS(status));

	return (-1);
}

static int
run_zfs_str(const char *cmd, char **output)
{
	FILE *fp;
	char buf[1024];
	size_t len;
	char *result = NULL;
	int first = 1;

	fp = popen(cmd, "r");
	if (fp == NULL)
		return (-1);

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		len = strlen(buf);
		char *newp = realloc(result, (result ? strlen(result) : 0) + len + 1);
		if (newp == NULL) {
			free(result);
			pclose(fp);
			return (-1);
		}
		result = newp;
		if (first) {
			result[0] = '\0';
			first = 0;
		}
		strcat(result, buf);
	}

	pclose(fp);

	if (output != NULL && result != NULL)
		*output = result;
	else
		free(result);

	return (0);
}

static int
ensure_dir(const char *path)
{
	if (mkdirp(path, 0755) != 0 && errno != EEXIST)
		return (-1);
	return (0);
}

static int
make_dataset_name(const char *prefix, const char *name, char *buf,
    size_t buflen)
{
	/* Replace slashes with colons in name */
	const char *p;
	char *q;
	size_t plen = strlen(prefix);

	if (buflen < plen + 1 + strlen(name) + 1)
		return (-1);

	snprintf(buf, buflen, "%s/", prefix);
	q = buf + plen + 1;

	for (p = name; *p; p++) {
		if (*p == '/')
			*q++ = ':';
		else
			*q++ = *p;
	}
	*q = '\0';

	return (0);
}

int
zfs_store_init(void)
{
	int ret;

	/* Ensure parent datasets exist */
	ret = run_zfs(4, "zfs", "create", "-p", zfs_get_images_dataset());
	if (ret != 0 && ret != 1)  /* 1 = already exists */
		return (-1);

	ret = run_zfs(4, "zfs", "create", "-p", zfs_get_layers_dataset());
	if (ret != 0 && ret != 1)
		return (-1);

	ret = run_zfs(4, "zfs", "create", "-p", zfs_get_volumes_dataset());
	if (ret != 0 && ret != 1)
		return (-1);

	/* Ensure mountpoint base exists */
	if (ensure_dir(mountpoint_base) != 0)
		return (-1);

	return (0);
}

int
zfs_store_ensure_dataset(const char *dataset, const char *mountpoint)
{
	int ret;

	ret = run_zfs(4, "zfs", "create", "-p", dataset);
	if (ret != 0 && ret != 1)
		return (-1);

	if (mountpoint != NULL) {
		char mnt_arg[PATH_MAX * 2];
		snprintf(mnt_arg, sizeof(mnt_arg), "mountpoint=%s", mountpoint);
		ret = run_zfs(4, "zfs", "set", mnt_arg, dataset);
		if (ret != 0)
			return (-1);
	}

	return (0);
}

const char *
zfs_get_pool(void)
{
	return (zfs_pool);
}

const char *
zfs_get_images_dataset(void)
{
	static char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s/%s", zfs_pool, OCIFBSD_ZFS_IMAGES);
	return (buf);
}

const char *
zfs_get_layers_dataset(void)
{
	static char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s/%s", zfs_pool, OCIFBSD_ZFS_LAYERS);
	return (buf);
}

const char *
zfs_get_volumes_dataset(void)
{
	static char buf[PATH_MAX];
	snprintf(buf, sizeof(buf), "%s/%s", zfs_pool, OCIFBSD_ZFS_VOLUMES);
	return (buf);
}

char *
zfs_image_path(const char *registry, const char *repo, const char *tag)
{
	char *path;
	size_t len;

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

	len = strlen(mountpoint_base) + 1 +
	    strlen("volumes") + 1 +
	    strlen(name) + 1;

	path = malloc(len);
	if (path == NULL)
		return (NULL);

	snprintf(path, len, "%s/volumes/%s", mountpoint_base, name);

	return (path);
}

int
zfs_set_property(const char *dataset, const char *property, const char *value)
{
	char prop_arg[PATH_MAX * 2];
	int ret;

	snprintf(prop_arg, sizeof(prop_arg), "%s=%s", property, value);
	ret = run_zfs(4, "zfs", "set", prop_arg, dataset);

	return (ret == 0 ? 0 : -1);
}

int
zfs_get_property(const char *dataset, const char *property, char **value)
{
	char cmd[PATH_MAX * 2];
	char *output = NULL;
	int ret;

	snprintf(cmd, sizeof(cmd), "zfs get -H -o value %s %s", property, dataset);
	ret = run_zfs_str(cmd, &output);

	if (ret != 0 || output == NULL)
		return (-1);

	/* Trim trailing newline */
	size_t len = strlen(output);
	if (len > 0 && output[len - 1] == '\n')
		output[len - 1] = '\0';

	*value = output;
	return (0);
}

int
zfs_dataset_exists(const char *dataset)
{
	char cmd[PATH_MAX];
	char *output = NULL;
	int ret;

	snprintf(cmd, sizeof(cmd), "zfs list -H -t filesystem,snapshot %s",
	    dataset);
	ret = run_zfs_str(cmd, &output);

	free(output);
	return (ret == 0 ? 1 : 0);
}

int
zfs_destroy_dataset(const char *dataset, bool recursive)
{
	int argc = 4;
	int actual_argc = 3;
	int ret;
	char *argv[6];

	argv[0] = "zfs";
	argv[1] = "destroy";
	argv[2] = (char *)dataset;
	argv[3] = recursive ? "-r" : NULL;
	argv[4] = "-f";
	argv[5] = NULL;

	/* Build actual argc */
	int actual_argc = 3;
	if (argv[3] != NULL) {
		argv[4] = "-f";
		argv[5] = NULL;
		actual_argc = 5;
	}

	ret = run_zfs(actual_argc, argv[0], argv[1], argv[2],
	    argv[3] ? argv[3] : "", argv[4] ? argv[4] : "");
	if (ret != 0 && ret != 1)
		return (-1);

	return (0);
}

/*
 * Layer operations
 */
int
zfs_layer_create(const char *digest, uint64_t size __unused)
{
	char dataset[PATH_MAX];
	int ret;

	make_dataset_name(zfs_get_layers_dataset(), digest, dataset,
	    sizeof(dataset));

	ret = run_zfs(4, "zfs", "create", "-p", dataset);
	if (ret != 0 && ret != 1)
		return (-1);

	/* Set compression */
	ret = zfs_set_property(dataset, "compression", "gzip-9");
	if (ret != 0)
		return (-1);

	/* Mark as shared layer */
	ret = zfs_set_property(dataset, "ocifbsd:layer", "true");
	if (ret != 0)
		return (-1);

	ret = zfs_set_property(dataset, "ocifbsd:digest", digest);
	if (ret != 0)
		return (-1);

	return (0);
}

int
zfs_layer_destroy(const char *digest)
{
	char dataset[PATH_MAX];

	make_dataset_name(zfs_get_layers_dataset(), digest, dataset,
	    sizeof(dataset));

	return (zfs_destroy_dataset(dataset, true));
}

bool
zfs_layer_exists(const char *digest)
{
	char dataset[PATH_MAX];

	make_dataset_name(zfs_get_layers_dataset(), digest, dataset,
	    sizeof(dataset));

	return (zfs_dataset_exists(dataset) > 0);
}

int
zfs_layer_mount(const char *digest, char **mountpoint)
{
	char dataset[PATH_MAX];
	char *mp;
	int ret;

	make_dataset_name(zfs_get_layers_dataset(), digest, dataset,
	    sizeof(dataset));

	mp = zfs_layer_path(digest);
	if (mp == NULL)
		return (-1);

	/* Ensure mountpoint exists */
	if (ensure_dir(mp) != 0) {
		free(mp);
		return (-1);
	}

	/* Mount the dataset */
	ret = zfs_set_property(dataset, "mountpoint", mp);
	if (ret != 0) {
		free(mp);
		return (-1);
	}

	*mountpoint = mp;
	return (0);
}

int
zfs_layer_umount(const char *digest)
{
	char dataset[PATH_MAX];
	int ret;

	make_dataset_name(zfs_get_layers_dataset(), digest, dataset,
	    sizeof(dataset));

	ret = run_zfs(3, "zfs", "unmount", dataset);
	if (ret != 0 && ret != 1)
		return (-1);

	return (0);
}

int
zfs_layer_add_files(const char *digest, const char *srcdir)
{
	char dataset[PATH_MAX];
	char src[PATH_MAX], dst[PATH_MAX];
	FTS *fts;
	FTSENT *ent;
	int ret;

	make_dataset_name(zfs_get_layers_dataset(), digest, dataset,
	    sizeof(dataset));

	snprintf(src, sizeof(src), "%s/", srcdir);
	snprintf(dst, sizeof(dst), "%s/layers/%s", mountpoint_base, digest);

	fts = fts_open((char *const[]){ src, NULL }, FTS_PHYSICAL, NULL);
	if (fts == NULL)
		return (-1);

	while ((ent = fts_read(fts)) != NULL) {
		if (ent->fts_info == FTS_F) {
			char relpath[PATH_MAX];
			char target[PATH_MAX];
			char linkbuf[PATH_MAX];
			ssize_t linklen;

			snprintf(relpath, sizeof(relpath), "%s", ent->fts_path + strlen(src));
			snprintf(target, sizeof(target), "%s%s", dst, relpath);

			/* Ensure directory exists */
			ensure_dir(dirname(target));

			switch (ent->fts_info) {
			case FTS_F:
				ret = copy_file(ent->fts_path, target, 0644);
				if (ret != 0) {
					fts_close(fts);
					return (-1);
				}
				break;
			case FTS_SL:
				linklen = readlink(ent->fts_path, linkbuf, sizeof(linkbuf) - 1);
				if (linklen > 0) {
					linkbuf[linklen] = '\0';
					symlink(linkbuf, target);
				}
				break;
			}
		}
	}

	fts_close(fts);
	return (0);
}

struct zfs_layer *
zfs_layer_get(const char *digest)
{
	struct zfs_layer *layer;
	char dataset[PATH_MAX];
	char *value;
	int ret;

	layer = calloc(1, sizeof(*layer));
	if (layer == NULL)
		return (NULL);

	make_dataset_name(zfs_get_layers_dataset(), digest, dataset,
	    sizeof(dataset));

	layer->digest = strdup(digest);
	layer->dataset = strdup(dataset);
	layer->mountpoint = zfs_layer_path(digest);

	/* Get size */
	ret = zfs_get_property(dataset, "used", &value);
	if (ret == 0) {
		layer->size = strtoull(value, NULL, 10);
		free(value);
	}

	/* Get compression */
	ret = zfs_get_property(dataset, "compression", &value);
	if (ret == 0) {
		layer->compressed = (strcmp(value, "off") != 0);
		free(value);
	}

	/* Check if shared */
	ret = zfs_get_property(dataset, "ocifbsd:shared", &value);
	if (ret == 0) {
		layer->shared = (strcmp(value, "true") == 0);
		free(value);
	}

	return (layer);
}

void
zfs_layer_free(struct zfs_layer *layer)
{
	if (layer == NULL)
		return;

	free(layer->digest);
	free(layer->dataset);
	free(layer->mountpoint);
	free(layer);
}

uint64_t
zfs_layer_get_size(const char *digest)
{
	struct zfs_layer *layer;
	uint64_t size;

	layer = zfs_layer_get(digest);
	if (layer == NULL)
		return (0);

	size = layer->size;
	zfs_layer_free(layer);

	return (size);
}

int
zfs_layer_set_shared(const char *digest, bool shared)
{
	char dataset[PATH_MAX];

	make_dataset_name(zfs_get_layers_dataset(), digest, dataset,
	    sizeof(dataset));

	return (zfs_set_property(dataset, "ocifbsd:shared", shared ? "true" : "false"));
}

/*
 * Image operations
 */
int
zfs_store_create_image(const char *registry, const char *repo,
    const char *tag, const char *digest)
{
	char dataset[PATH_MAX];
	char mp[PATH_MAX];
	int ret;

	make_dataset_name(zfs_get_images_dataset(), digest, dataset,
	    sizeof(dataset));

	ret = run_zfs(4, "zfs", "create", "-p", dataset);
	if (ret != 0 && ret != 1)
		return (-1);

	/* Create mountpoint */
	snprintf(mp, sizeof(mp), "%s/%s/%s/%s", mountpoint_base,
	    registry, repo, tag);
	if (ensure_dir(mp) != 0)
		return (-1);

	ret = zfs_set_property(dataset, "mountpoint", mp);
	if (ret != 0)
		return (-1);

	ret = zfs_set_property(dataset, "ocifbsd:image", "true");
	if (ret != 0)
		return (-1);

	ret = zfs_set_property(dataset, "ocifbsd:registry", registry);
	if (ret != 0)
		return (-1);

	ret = zfs_set_property(dataset, "ocifbsd:repo", repo);
	if (ret != 0)
		return (-1);

	ret = zfs_set_property(dataset, "ocifbsd:tag", tag);
	if (ret != 0)
		return (-1);

	ret = zfs_set_property(dataset, "ocifbsd:digest", digest);
	if (ret != 0)
		return (-1);

	return (0);
}

int
zfs_store_clone_layer(const char *src_digest, const char *dst_digest)
{
	char src_dataset[PATH_MAX];
	char dst_dataset[PATH_MAX];
	char dst_mp[PATH_MAX];
	char *mp;
	int ret;

	make_dataset_name(zfs_get_layers_dataset(), src_digest, src_dataset,
	    sizeof(src_dataset));
	make_dataset_name(zfs_get_layers_dataset(), dst_digest, dst_dataset,
	    sizeof(dst_dataset));

	/* Clone the source dataset */
	ret = run_zfs(5, "zfs", "clone", src_dataset, dst_dataset);
	if (ret != 0)
		return (-1);

	/* Update digest property */
	ret = zfs_set_property(dst_dataset, "ocifbsd:digest", dst_digest);
	if (ret != 0)
		return (-1);

	return (0);
}

int
zfs_store_add_layer(const char *image_dataset, const char *layer_digest)
{
	char *mp;
	int ret;

	/* Get image mountpoint */
	ret = zfs_get_property(image_dataset, "mountpoint", &mp);
	if (ret != 0)
		return (-1);

	/* Layer mountpoint */
	char *layer_mp = zfs_layer_path(layer_digest);
	if (layer_mp == NULL) {
		free(mp);
		return (-1);
	}

	/* Bind mount layer into image */
	char target_path[PATH_MAX * 2];
	snprintf(target_path, sizeof(target_path), "%s/%s", mp, layer_digest);
	pid_t pid = fork();
	if (pid == 0) {
		closefrom(STDERR_FILENO + 1);
		execlp("mount_nullfs", "mount_nullfs", "-o", "ro",
		    layer_mp, target_path, (char *)NULL);
		_exit(127);
	}
	int status;
	waitpid(pid, &status, 0);
	ret = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

	free(mp);
	free(layer_mp);

	return (ret == 0 ? 0 : -1);
}

int
zfs_store_destroy_image(const char *registry, const char *repo,
    const char *tag)
{
	char dataset[PATH_MAX];
	char digest[PATH_MAX];
	char *value;
	int ret;

	make_dataset_name(zfs_get_images_dataset(), digest, dataset,
	    sizeof(dataset));

	/* Get the image digest before destroying */
	ret = zfs_get_property(dataset, "ocifbsd:digest", &value);
	if (ret == 0) {
		strlcpy(digest, value, sizeof(digest));
		free(value);
	}

	ret = zfs_destroy_dataset(dataset, true);
	if (ret != 0)
		return (-1);

	return (0);
}

int
zfs_store_list_images(struct zfs_image ***images, int *nimages)
{
	/*
	 * Listing images via 'zfs list -t filesystem' is not yet
	 * implemented. The implementation needs to:
	 *
	 *   1. Call 'zfs list -t filesystem -H -o name,used,quota,
	 *      mountpoint,creation' to enumerate all ZFS filesystems
	 *   2. Filter to only those under the ocifbsd dataset prefix
	 *      (e.g., tank/ocifbsd/images/<asterisk>)
	 *   3. For each, parse the name into registry/repo/tag
	 *   4. Optionally call 'zfs get -H digest' to get manifest digest
	 *   5. Allocate zfs_image array, populate, return
	 *
	 * Alternative: use libzfs (libzfs.h, in base on FreeBSD) for
	 * a proper C API instead of popen().
	 *
	 * This function runs on FreeBSD hosts only (macOS has no ZFS
	 * in the kernel, and the userland zfs(8) tools are not in base).
	 * See MIGRATION.md for the full plan.
	 */
	*nimages = 0;
	*images = NULL;
	return (0);
}

int
zfs_store_get_image(const char *registry, const char *repo,
    const char *tag, struct zfs_image **image)
{
	/*
	 * Looking up a specific image is not yet implemented. Needs to:
	 *   1. Build dataset path: <prefix>/<registry>/<repo>/<tag>
	 *   2. Call 'zfs list -H <dataset>' to check existence
	 *   3. If exists, call 'zfs get -H used,quota,mountpoint' for details
	 *   4. Allocate and populate zfs_image struct
	 *
	 * This function runs on FreeBSD hosts only.
	 * See MIGRATION.md for the full plan.
	 */
	(void)registry;
	(void)repo;
	(void)tag;
	*image = NULL;
	return (ENOENT);
}

/*
 * Snapshot operations
 */
int
zfs_store_snapshot(const char *dataset, const char *snapshot)
{
	char snap[PATH_MAX * 2];
	int ret;

	snprintf(snap, sizeof(snap), "%s@%s", dataset, snapshot);
	ret = run_zfs(4, "zfs", "snapshot", snap);
	if (ret != 0 && ret != 1)
		return (-1);

	return (0);
}

int
zfs_store_rollback(const char *dataset, const char *snapshot)
{
	char snap[PATH_MAX * 2];

	snprintf(snap, sizeof(snap), "%s@%s", dataset, snapshot);
	return (run_zfs(4, "zfs", "rollback", "-r", snap));
}

int
zfs_store_send(const char *dataset, const char *snapshot, int fd)
{
	char snap[PATH_MAX * 2];
	char cmd[PATH_MAX * 2];
	FILE *fp;
	char buf[8192];
	size_t n;

	snprintf(snap, sizeof(snap), "%s@%s", dataset, snapshot);
	snprintf(cmd, sizeof(cmd), "zfs send %s", snap);

	fp = popen(cmd, "r");
	if (fp == NULL)
		return (-1);

	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		if (write(fd, buf, n) != (ssize_t)n) {
			pclose(fp);
			return (-1);
		}
	}

	pclose(fp);
	return (0);
}

int
zfs_store_recv(const char *dataset, int fd)
{
	char cmd[PATH_MAX * 2];
	FILE *fp;
	char buf[8192];
	size_t n;
	int ret;

	snprintf(cmd, sizeof(cmd), "zfs recv -F %s", dataset);
	fp = popen(cmd, "w");
	if (fp == NULL)
		return (-1);

	while ((n = read(fd, buf, sizeof(buf))) > 0) {
		if (fwrite(buf, 1, n, fp) != n) {
			pclose(fp);
			return (-1);
		}
	}

	ret = pclose(fp);
	return (ret == 0 ? 0 : -1);
}

/*
 * Storage usage
 */
int
zfs_store_get_usage(uint64_t *used, uint64_t *available)
{
	char *output = NULL;
	char *line, *save;
	int ret;

	ret = run_zfs_str("zfs list -H -p -t filesystem,volume ocifbsd", &output);
	if (ret != 0 || output == NULL)
		return (-1);

	*used = 0;
	*available = 0;

	line = strtok_r(output, "\n", &save);
	while (line != NULL) {
		char *tab = strchr(line, '\t');
		if (tab != NULL) {
			*used += strtoull(tab + 1, NULL, 10);
		}
		line = strtok_r(NULL, "\n", &save);
	}

	free(output);

	/* Get available space */
	ret = run_zfs_str("zfs get -H -p available zroot", &output);
	if (ret == 0 && output != NULL) {
		char *tab = strrchr(output, '\t');
		if (tab != NULL)
			*available = strtoull(tab + 1, NULL, 10);
		free(output);
	}

	return (0);
}

int
zfs_store_cleanup_unused(void)
{
	/*
	 * Cleanup of orphaned layers is not yet implemented. Needs to:
	 *   1. List all image datasets (via zfs_store_list_images)
	 *   2. For each layer dataset, check if referenced by any image
	 *   3. If unreferenced for >N days, 'zfs destroy <layer>'
	 *   4. Optionally, snapshot before destroy for grace period
	 *
	 * Garbage collection is a separate concern from this function -
	 * this is just the orphan cleanup, not the policy (when to
	 * cleanup, retention, etc.).
	 *
	 * This function runs on FreeBSD hosts only.
	 * See MIGRATION.md for the full plan.
	 */
	return (0);
}

/*
 * Volume operations
 */
int
zfs_volume_create(const char *name, uint64_t size, bool encrypted)
{
	char dataset[PATH_MAX];
	char volsize[32];
	int ret;

	make_dataset_name(zfs_get_volumes_dataset(), name, dataset,
	    sizeof(dataset));

	ret = run_zfs(5, "zfs", "create", "-V", "1G", "-p", dataset);
	if (ret != 0 && ret != 1)
		return (-1);

	/* Set volume size */
	snprintf(volsize, sizeof(volsize), "%lluM", (unsigned long long)size);
	ret = zfs_set_property(dataset, "volsize", volsize);
	if (ret != 0)
		return (-1);

	/* Set encryption if requested */
	if (encrypted) {
		ret = zfs_set_property(dataset, "encryption", "on");
		if (ret != 0)
			return (-1);
		ret = zfs_set_property(dataset, "keyformat", "passphrase");
		if (ret != 0)
			return (-1);
	}

	/* Mark as ocifbsd volume */
	ret = zfs_set_property(dataset, "ocifbsd:volume", "true");
	if (ret != 0)
		return (-1);

	return (0);
}

int
zfs_volume_destroy(const char *name)
{
	char dataset[PATH_MAX];

	make_dataset_name(zfs_get_volumes_dataset(), name, dataset,
	    sizeof(dataset));

	return (zfs_destroy_dataset(dataset, true));
}

int
zfs_volume_mount(const char *name, char **mountpoint)
{
	char dataset[PATH_MAX];
	char *mp;
	int ret;

	make_dataset_name(zfs_get_volumes_dataset(), name, dataset,
	    sizeof(dataset));

	mp = zfs_volume_path(name);
	if (mp == NULL)
		return (-1);

	if (ensure_dir(mp) != 0) {
		free(mp);
		return (-1);
	}

	ret = zfs_set_property(dataset, "mountpoint", mp);
	if (ret != 0) {
		free(mp);
		return (-1);
	}

	*mountpoint = mp;
	return (0);
}

int
zfs_volume_umount(const char *name)
{
	char dataset[PATH_MAX];

	make_dataset_name(zfs_get_volumes_dataset(), name, dataset,
	    sizeof(dataset));

	return (run_zfs(3, "zfs", "umount", dataset));
}

int
zfs_volume_snapshot(const char *name, const char *snapname)
{
	char dataset[PATH_MAX];

	make_dataset_name(zfs_get_volumes_dataset(), name, dataset,
	    sizeof(dataset));

	return (zfs_store_snapshot(dataset, snapname));
}

int
zfs_volume_rollback(const char *name, const char *snapname)
{
	char dataset[PATH_MAX];

	make_dataset_name(zfs_get_volumes_dataset(), name, dataset,
	    sizeof(dataset));

	return (zfs_store_rollback(dataset, snapname));
}

struct zfs_volume *
zfs_volume_get(const char *name)
{
	struct zfs_volume *vol;
	char dataset[PATH_MAX];
	char *value;
	int ret;

	vol = calloc(1, sizeof(*vol));
	if (vol == NULL)
		return (NULL);

	make_dataset_name(zfs_get_volumes_dataset(), name, dataset,
	    sizeof(dataset));

	vol->name = strdup(name);
	vol->dataset = strdup(dataset);
	vol->mountpoint = zfs_volume_path(name);

	/* Get size */
	ret = zfs_get_property(dataset, "volsize", &value);
	if (ret == 0) {
		vol->size = strtoull(value, NULL, 10);
		free(value);
	}

	/* Get encryption */
	ret = zfs_get_property(dataset, "encryption", &value);
	if (ret == 0) {
		vol->encrypted = (strcmp(value, "off") != 0);
		free(value);
	}

	/* Get readonly */
	ret = zfs_get_property(dataset, "readonly", &value);
	if (ret == 0) {
		vol->readonly = (strcmp(value, "on") == 0);
		free(value);
	}

	return (vol);
}

void
zfs_volume_free(struct zfs_volume *vol)
{
	if (vol == NULL)
		return;

	free(vol->name);
	free(vol->dataset);
	free(vol->mountpoint);
	free(vol);
}

int
zfs_volume_list(struct zfs_volume ***volumes, int *nvolumes)
{
	/*
	 * Listing ZFS volumes is not yet implemented. Needs to:
	 *   1. Call 'zfs list -t volume -H -o name,volsize,encryption,
	 *      mountpoint'
	 *   2. Filter to ocifbsd volume prefix
	 *   3. Parse into zfs_volume structs
	 *
	 * This function runs on FreeBSD hosts only.
	 * See MIGRATION.md for the full plan.
	 */
	*nvolumes = 0;
	*volumes = NULL;
	return (0);
}
