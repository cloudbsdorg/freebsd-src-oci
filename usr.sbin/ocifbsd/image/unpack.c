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
 * OCI image layer unpacking
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/extattr.h>
#include <sys/wait.h>

#include <archive.h>
#include <archive_entry.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int mkdirp_local(const char *path, mode_t mode);
static int
mkdirp_local(const char *path, mode_t mode)
{
	char buf[PATH_MAX];
	char *p;
	size_t len;

	if (path == NULL || *path == '\0')
		return (-1);

	len = strlcpy(buf, path, sizeof(buf));
	if (len >= sizeof(buf))
		return (-1);

	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, mode) != 0 && errno != EEXIST)
			return (-1);
		*p = '/';
	}

	if (mkdir(buf, mode) != 0 && errno != EEXIST)
		return (-1);
	return (0);
}

#define	mkdirp(path, mode)	mkdirp_local((path), (mode))
#include <zlib.h>

#include "unpack.h"
#include "zfs_store.h"

#define WHITEOUT_PREFIX		".wh."
#define WHITEOUT_PREFIX_LEN	4

/* is_whiteout / get_whiteout_target live in whiteout.c */

/*
 * Default unpack options
 */
static struct unpack_options default_opts = {
	.keep_permissions = true,
	.expand_whiteouts = true,
	.strip_whiteouts = false,
	.preserve_xattrs = true,
	.root = NULL
};

/*
 * Get default options
 */
static struct unpack_options *
unpack_default_options(void)
{
	return (&default_opts);
}

/*
 * Detect compression type from file magic
 */
compression_type_t
detect_compression(const char *path)
{
	unsigned char buf[6];
	int fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (COMPRESSION_NONE);

	ssize_t n = read(fd, buf, sizeof(buf));
	close(fd);

	if (n < 2)
		return (COMPRESSION_NONE);

	/* Check magic bytes */
	if (buf[0] == 0x1f && buf[1] == 0x8b)
		return (COMPRESSION_GZIP);

	if (buf[0] == 'B' && buf[1] == 'Z' && buf[2] == 'h')
		return (COMPRESSION_BZIP2);

	if (buf[0] == 0xfd && buf[1] == '7' && buf[2] == 'z' && buf[3] == 'X' && buf[4] == 'Z')
		return (COMPRESSION_XZ);

	if (buf[0] == 0x28 && buf[1] == 0xb5 && buf[2] == 0x2f)
		return (COMPRESSION_ZSTD);

	return (COMPRESSION_NONE);
}

const char *
compression_extension(compression_type_t type)
{
	switch (type) {
	case COMPRESSION_GZIP:
		return (".gz");
	case COMPRESSION_BZIP2:
		return (".bz2");
	case COMPRESSION_XZ:
		return (".xz");
	case COMPRESSION_ZSTD:
		return (".zst");
	default:
		return ("");
	}
}

/*
 * Check if directory has opaque whiteout marker
 */
bool
is_opaque(const char *dirname)
{
	char path[PATH_MAX];
	struct stat st;

	snprintf(path, sizeof(path), "%s/.wh..wh..opq", dirname);

	if (stat(path, &st) == 0 && S_ISREG(st.st_mode))
		return (true);

	return (false);
}

/*
 * Recursively delete files marked by whiteouts
 */
static int
delete_whiteout_files(const char *dir, const char *wh_prefix)
{
	FTS *fts;
	FTSENT *ent;
	char *paths[2];
	char target[PATH_MAX];
	int ret = 0;

	paths[0] = (char *)dir;
	paths[1] = NULL;

	fts = fts_open(paths, FTS_PHYSICAL | FTS_NOSTAT, NULL);
	if (fts == NULL)
		return (-1);

	while ((ent = fts_read(fts)) != NULL) {
		if (is_whiteout(ent->fts_name)) {
			char *target_file = get_whiteout_target(ent->fts_name);
			if (target_file == NULL)
				continue;  /* Opaque marker */

			/* Build path to target */
			snprintf(target, sizeof(target), "%s/%s",
			    ent->fts_path, target_file);

			/* Delete target file/directory */
			if (unlink(target) == 0 || rmdir(target) == 0) {
				fprintf(stderr, "deleted: %s\n", target);
			}

			/* Delete whiteout marker itself */
			unlink(ent->fts_path);

			free(target_file);
		}
	}

	fts_close(fts);
	return (ret);
}

/*
 * Find all whiteout files in a directory tree
 */
int
find_whiteouts(const char *dir, struct whiteout_info **info)
{
	struct whiteout_info *wi;
	FTS *fts;
	FTSENT *ent;
	char *paths[2];
	char *target;

	wi = calloc(1, sizeof(*wi));
	if (wi == NULL)
		return (-1);

	paths[0] = (char *)dir;
	paths[1] = NULL;

	fts = fts_open(paths, FTS_PHYSICAL | FTS_NOSTAT, NULL);
	if (fts == NULL) {
		free(wi);
		return (-1);
	}

	while ((ent = fts_read(fts)) != NULL) {
		if (is_whiteout(ent->fts_name)) {
			target = get_whiteout_target(ent->fts_name);
			if (target != NULL) {
				wi->files = realloc(wi->files,
				    (wi->nfiles + 1) * sizeof(char *));
				wi->files[wi->nfiles++] = target;
			}
		}
	}

	fts_close(fts);

	*info = wi;
	return (0);
}

void
free_whiteout_info(struct whiteout_info *info)
{
	int i;

	if (info == NULL)
		return;

	for (i = 0; i < info->nfiles; i++)
		free(info->files[i]);

	free(info->files);
	free(info);
}

/*
 * Expand whiteouts by creating .wh.<filename> markers
 */
int
expand_whiteouts(const char *dir, const struct whiteout_info *info)
{
	int i;
	char path[PATH_MAX];

	for (i = 0; i < info->nfiles; i++) {
		snprintf(path, sizeof(path), "%s/.wh.%s", dir, info->files[i]);

		int fd = open(path, O_CREAT | O_WRONLY, 0644);
		if (fd >= 0)
			close(fd);
	}

	return (0);
}

/*
 * Apply whiteouts: delete files marked by whiteout markers
 */
int
apply_whiteouts(const char *dir)
{
	return (delete_whiteout_files(dir, WHITEOUT_PREFIX));
}

/*
 * Extract a single tar entry to disk
 */
static int
extract_entry(struct archive *ar, struct archive_entry *entry,
    const char *dest, struct unpack_options *opts)
{
	const char *pathname;
	char path[PATH_MAX];
	int fd;
	int ret = 0;

	pathname = archive_entry_pathname(entry);
	if (pathname == NULL || pathname[0] == '\0')
		return (0);

	/* Skip whiteout files - they'll be handled separately */
	if (is_whiteout(pathname))
		return (0);

	/* Build destination path */
	snprintf(path, sizeof(path), "%s/%s", dest, pathname);

	archive_entry_stat(entry);

	switch (archive_entry_filetype(entry)) {
	case AE_IFREG: {
		/*
		 * dirname(3) may modify its argument in place — copy first
		 * so we do not clobber the full destination path.
		 */
		char path_copy[PATH_MAX];
		char *parent;

		strlcpy(path_copy, path, sizeof(path_copy));
		parent = dirname(path_copy);
		if (mkdirp(parent, 0755) != 0 && errno != EEXIST) {
			fprintf(stderr, "error: cannot create directory: %s\n",
			    parent);
			return (-1);
		}

		fd = open(path, O_CREAT | O_WRONLY | O_TRUNC,
		    archive_entry_mode(entry));
		if (fd < 0) {
			fprintf(stderr, "error: cannot create file: %s: %s\n",
			    path, strerror(errno));
			return (-1);
		}

		/* Copy data */
		{
		char buf[8192];
		ssize_t n;
		while ((n = archive_read_data(ar, buf, sizeof(buf))) > 0) {
			if (write(fd, buf, n) != n) {
				fprintf(stderr, "error: write failed: %s\n",
				    strerror(errno));
				ret = -1;
			}
		}
		}

		close(fd);

		/* Preserve permissions if requested */
		if (opts->keep_permissions) {
			chmod(path, archive_entry_mode(entry));
			chown(path, archive_entry_uid(entry),
			    archive_entry_gid(entry));
		}
		}
		break;

	case AE_IFDIR:
		if (mkdirp(path, archive_entry_mode(entry)) != 0 && errno != EEXIST) {
			fprintf(stderr, "error: cannot create directory: %s\n",
			    path);
			ret = -1;
		}
		if (opts->keep_permissions) {
			chmod(path, archive_entry_mode(entry));
			chown(path, archive_entry_uid(entry),
			    archive_entry_gid(entry));
		}
		break;

	case AE_IFLNK:
		{
		const char *link = archive_entry_symlink(entry);
		char path_copy[PATH_MAX];
		char *parent;

		if (link == NULL)
			break;

		/* dirname(3) mutates its argument — copy first */
		strlcpy(path_copy, path, sizeof(path_copy));
		parent = dirname(path_copy);
		if (mkdirp(parent, 0755) != 0 && errno != EEXIST) {
			fprintf(stderr, "error: cannot create directory: %s\n",
			    parent);
			break;
		}

		unlink(path);  /* Remove if exists */
		if (symlink(link, path) != 0) {
			fprintf(stderr, "error: cannot create symlink: %s\n",
			    path);
			ret = -1;
		}
		}
		break;
	}

	return (ret);
}

/*
 * Unpack a single tarball to destination
 */
int
unpack_layer(const char *tarball, const char *dest,
    struct unpack_options *opts)
{
	struct archive *a;
	struct archive_entry *entry;
	compression_type_t comp;
	int ret = 0;

	if (opts == NULL)
		opts = &default_opts;

	/* Detect compression */
	comp = detect_compression(tarball);

	/* Open archive */
	a = archive_read_new();
	if (a == NULL) {
		fprintf(stderr, "error: archive_read_new failed\n");
		return (-1);
	}

	/* Add filters based on compression type */
	switch (comp) {
	case COMPRESSION_GZIP:
		archive_read_support_filter_gzip(a);
		break;
	case COMPRESSION_BZIP2:
		archive_read_support_filter_bzip2(a);
		break;
	case COMPRESSION_XZ:
		archive_read_support_filter_xz(a);
		break;
	case COMPRESSION_ZSTD:
		archive_read_support_filter_zstd(a);
		break;
	default:
		break;
	}

	archive_read_support_format_tar(a);

	if (archive_read_open_filename(a, tarball, 10240) != ARCHIVE_OK) {
		fprintf(stderr, "error: cannot open archive: %s\n",
		    archive_error_string(a));
		archive_read_free(a);
		return (-1);
	}

	/* Create destination if needed */
	if (mkdirp(dest, 0755) != 0 && errno != EEXIST) {
		fprintf(stderr, "error: cannot create directory: %s\n", dest);
		archive_read_free(a);
		return (-1);
	}

	/* Extract entries */
	while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
		if (extract_entry(a, entry, dest, opts) != 0) {
			ret = -1;
			break;
		}
		archive_read_data_skip(a);
	}

	archive_read_free(a);

	/* Apply whiteouts if requested */
	if (ret == 0 && opts->strip_whiteouts) {
		if (apply_whiteouts(dest) != 0) {
			fprintf(stderr, "warning: failed to apply whiteouts\n");
		}
	}

	return (ret);
}

/*
 * Unpack multiple layers in order (for layered images)
 */
int
unpack_layers(const char **tarballs, int ntarballs, const char *dest,
    struct unpack_options *opts)
{
	int i, ret = 0;
	char layer_dir[PATH_MAX];

	for (i = 0; i < ntarballs; i++) {
		fprintf(stderr, "unpacking layer %d/%d: %s\n",
		    i + 1, ntarballs, tarballs[i]);

		snprintf(layer_dir, sizeof(layer_dir), "%s/layer.%d",
		    dest, i);

		ret = unpack_layer(tarballs[i], layer_dir, opts);
		if (ret != 0) {
			fprintf(stderr, "error: failed to unpack layer %d\n", i);
			break;
		}
	}

	return (ret);
}

/*
 * Unpack a complete OCI image from a directory
 */
int
unpack_image(const char *imagedir, const char *dest,
    struct unpack_options *opts)
{
	char layers_dir[PATH_MAX];
	char **tarballs = NULL;
	int ntarballs = 0;
	DIR *dir;
	struct dirent *ent;
	int ret = 0;
	int i;

	if (opts == NULL)
		opts = &default_opts;

	/* Find layer tarballs */
	snprintf(layers_dir, sizeof(layers_dir), "%s/layers", imagedir);
	dir = opendir(layers_dir);
	if (dir == NULL) {
		fprintf(stderr, "error: cannot open layers directory: %s\n",
		    layers_dir);
		return (-1);
	}

	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_type == DT_REG) {
			char path[PATH_MAX];
			snprintf(path, sizeof(path), "%s/%s",
			    layers_dir, ent->d_name);

			tarballs = realloc(tarballs,
			    (ntarballs + 1) * sizeof(char *));
			tarballs[ntarballs++] = strdup(path);
		}
	}
	closedir(dir);

	/* Unpack each layer */
	for (i = 0; i < ntarballs; i++) {
		char layer_dest[PATH_MAX];

		snprintf(layer_dest, sizeof(layer_dest), "%s/%d",
		    dest, i);

		fprintf(stderr, "unpacking layer %d/%d\n", i + 1, ntarballs);
		ret = unpack_layer(tarballs[i], layer_dest, opts);
		if (ret != 0) {
			fprintf(stderr, "error: failed to unpack layer %d\n", i);
			break;
		}
	}

	/* Clean up */
	for (i = 0; i < ntarballs; i++)
		free(tarballs[i]);
	free(tarballs);

	return (ret);
}
