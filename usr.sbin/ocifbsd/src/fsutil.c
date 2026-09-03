/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * Filesystem utilities for ocifbsd. Extracted from the CLI dispatcher so the
 * symlink-safe recursive remove lives with the rest of the runtime's file
 * helpers rather than inside ocifbsd.c.
 */

#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../include/ocifbsd.h"

/*
 * Recursively remove a directory tree without following symlinks in any path
 * component. This is defense-in-depth against a component being swapped to a
 * symlink mid-walk (a TOCTOU the earlier fts(3) + FTS_NOCHDIR walk left open,
 * since it built full paths and then acted on them). Every step is an *at()
 * syscall relative to an open directory fd — openat with O_NOFOLLOW|O_DIRECTORY
 * to descend, fdopendir + unlinkat/fchflags to act — so a raced symlink cannot
 * redirect a removal outside the tree. The walk stays on a single filesystem
 * (like the old FTS_XDEV): a child directory on a different device is not
 * descended into. Best-effort immutable-flag clearing (via fchflags on an
 * O_NOFOLLOW fd) mirrors the behavior needed to remove FreeBSD image rootfses,
 * whose files carry schg/uchg flags; it cannot succeed at securelevel >= 1,
 * which is a system policy the runtime does not override.
 */
static int
rm_rf_at(int parentfd, const char *name, dev_t top_dev)
{
	struct stat st;
	int ret = 0;

	if (fstatat(parentfd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
		return (errno == ENOENT ? 0 : -1);

	if (S_ISDIR(st.st_mode)) {
		if (st.st_dev == top_dev) {
			int fd = openat(parentfd, name,
			    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			DIR *d;
			struct dirent *e;

			if (fd < 0) {
				/* Raced to a symlink/non-dir since fstatat:
				 * fall through and remove it as a leaf. */
				if (errno == ELOOP || errno == ENOTDIR)
					goto leaf;
				fprintf(stderr, "error: cannot open %s: %s\n",
				    name, strerror(errno));
				return (-1);
			}
			/* Clear any immutable flag on the directory itself so
			 * its entries can be removed (a schg/uchg directory
			 * blocks deletion of the files inside it). */
			(void)fchflags(fd, 0);
			d = fdopendir(fd);
			if (d == NULL) {
				close(fd);
				return (-1);
			}
			/*
			 * Unlinking while readdir() walks the same directory
			 * may (per POSIX) cause other entries to be skipped in
			 * that pass. Rescan until a full pass removes nothing,
			 * so no entry is left behind by the iteration itself.
			 */
			for (;;) {
				int removed = 0;

				rewinddir(d);
				while ((e = readdir(d)) != NULL) {
					if (strcmp(e->d_name, ".") == 0 ||
					    strcmp(e->d_name, "..") == 0)
						continue;
					if (rm_rf_at(fd, e->d_name,
					    top_dev) != 0)
						ret = -1;
					else
						removed = 1;
				}
				if (!removed || ret == -1)
					break;
			}
			closedir(d);	/* also closes fd */
		}
		if (unlinkat(parentfd, name, AT_REMOVEDIR) != 0 &&
		    errno != ENOENT) {
			fprintf(stderr, "error: cannot remove dir %s: %s\n",
			    name, strerror(errno));
			ret = -1;
		}
		return (ret);
	}
leaf:
	if (unlinkat(parentfd, name, 0) != 0 && errno != ENOENT) {
		/*
		 * Immutable/append file: clear its flags via an O_NOFOLLOW fd
		 * (so a swapped symlink is not followed) and retry.
		 */
		if (errno == EPERM) {
			int lf = openat(parentfd, name,
			    O_RDONLY | O_NOFOLLOW | O_CLOEXEC);

			if (lf >= 0) {
				(void)fchflags(lf, 0);
				close(lf);
				if (unlinkat(parentfd, name, 0) == 0 ||
				    errno == ENOENT)
					return (0);
			}
		}
		fprintf(stderr, "error: cannot remove %s: %s\n", name,
		    strerror(errno));
		return (-1);
	}
	return (0);
}

/* Recursive remove for rmi (depth-first), symlink-safe (see rm_rf_at). */
int
rm_rf(const char *path)
{
	char *dup, *slash;
	const char *parent, *leaf;
	struct stat st;
	dev_t top_dev;
	size_t n;
	int parentfd, ret;

	if (path == NULL || path[0] == '\0' || strcmp(path, "/") == 0) {
		errno = EINVAL;
		return (-1);
	}
	/* Anchor the walk to one filesystem via the top-level device. */
	if (lstat(path, &st) != 0)
		return (errno == ENOENT ? 0 : -1);
	top_dev = st.st_dev;

	/*
	 * Split into parent directory + leaf name and operate relative to the
	 * parent's fd, so even the leaf is reached through a single *at() call
	 * rather than a re-resolved path.
	 */
	dup = strdup(path);
	if (dup == NULL)
		return (-1);
	n = strlen(dup);
	while (n > 1 && dup[n - 1] == '/')	/* trim trailing slashes */
		dup[--n] = '\0';
	slash = strrchr(dup, '/');
	if (slash == dup) {			/* leaf directly under "/" */
		parent = "/";
		leaf = dup + 1;
	} else if (slash != NULL) {
		*slash = '\0';
		parent = dup;
		leaf = slash + 1;
	} else {				/* bare name, relative to cwd */
		parent = ".";
		leaf = dup;
	}
	/* O_NOFOLLOW so a symlink swapped in as the immediate parent cannot
	 * redirect the subsequent O_NOFOLLOW *at() walk out of the store. */
	parentfd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (parentfd < 0) {
		free(dup);
		return (-1);
	}
	ret = rm_rf_at(parentfd, leaf, top_dev);
	close(parentfd);
	free(dup);
	return (ret);
}
