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
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/syslimits.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <grp.h>
#include <limits.h>
#include <paths.h>
#include <sha256.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ocifbsd.h"

/* Verbosity level */
static bool verbose = false;

void
ocifbsd_set_verbose(bool v)
{
	verbose = v;
}

/*
 * Generate a unique container ID.
 * Format: 64-character hex string (256 bits from SHA-256 of random data)
 */
char *
generate_container_id(void)
{
	char *id;
	char hex[SHA256_DIGEST_STRING_LENGTH];
	uint8_t random_bytes[32];
	FILE *urandom;
	size_t nread;

	id = malloc(OCIFBSD_MAX_CONTAINER_ID_LENGTH);
	if (id == NULL)
		return (NULL);

	/*
	 * SHA256_Data(3) writes a NUL-terminated hex digest
	 * (SHA256_DIGEST_LENGTH * 2 chars) into the output buffer.
	 * Do not treat that buffer as binary and re-encode it — the
	 * old code overflowed a 32-byte array (SIGABRT under ATF).
	 */
	urandom = fopen("/dev/urandom", "r");
	if (urandom == NULL) {
		free(id);
		return (NULL);
	}
	nread = fread(random_bytes, 1, sizeof(random_bytes), urandom);
	fclose(urandom);
	if (nread != sizeof(random_bytes)) {
		free(id);
		return (NULL);
	}
	if (SHA256_Data(random_bytes, sizeof(random_bytes), hex) == NULL) {
		free(id);
		return (NULL);
	}
	memcpy(id, hex, OCIFBSD_MAX_CONTAINER_ID_LENGTH - 1);
	id[OCIFBSD_MAX_CONTAINER_ID_LENGTH - 1] = '\0';
	return (id);
}

/*
 * Convert a name to a canonical form suitable for use as a container name.
 * - Lowercase
 * - Replace invalid characters with hyphens
 * - Trim leading/trailing hyphens
 */
char *
canonical_name(const char *name)
{
	char *canonical;
	size_t i, j;

	if (name == NULL)
		return (NULL);

	canonical = strdup(name);
	if (canonical == NULL)
		return (NULL);

	/* Convert to lowercase */
	for (i = 0; canonical[i]; i++)
		canonical[i] = tolower((unsigned char)canonical[i]);

	/* Replace invalid characters */
	for (i = 0, j = 0; canonical[i]; i++) {
		if (isalnum((unsigned char)canonical[i]) ||
		    canonical[i] == '-' || canonical[i] == '_') {
			canonical[j++] = canonical[i];
		} else {
			canonical[j++] = '-';
		}
	}
	canonical[j] = '\0';

	/* Trim leading/trailing hyphens */
	while (j > 0 && canonical[j - 1] == '-')
		canonical[--j] = '\0';

	i = 0;
	while (canonical[i] == '-')
		i++;

	if (i > 0) {
		memmove(canonical, canonical + i, j - i + 1);
	}

	/* Ensure non-empty */
	if (canonical[0] == '\0') {
		free(canonical);
		return (NULL);
	}

	return (canonical);
}

/*
 * Resolve bundle path. If absolute, return as-is.
 * If relative, prepend cwd or use default.
 */
char *
resolve_bundle_path(const char *bundle)
{
	char *resolved;
	char cwd[PATH_MAX];
	char *path;

	if (bundle == NULL)
		return (NULL);

	if (bundle[0] == '/') {
		/* Absolute path */
		resolved = strdup(bundle);
	} else if (bundle[0] == '~') {
		/* Home directory */
		char root[] = "/";
		path = getenv("HOME");
		if (path == NULL)
			path = root;
		resolved = malloc(PATH_MAX);
		if (resolved == NULL)
			return (NULL);
		snprintf(resolved, PATH_MAX, "%s%s", path, bundle + 1);
	} else {
		/* Relative path */
		if (getcwd(cwd, sizeof(cwd)) == NULL)
			return (NULL);
		resolved = malloc(PATH_MAX);
		if (resolved == NULL)
			return (NULL);
		snprintf(resolved, PATH_MAX, "%s/%s", cwd, bundle);
	}

	return (resolved);
}

/*
 * Ensure a directory exists, creating it if necessary.
 * Returns 0 on success, -1 on error.
 */
int
ensure_directory(const char *path, mode_t mode)
{
	struct stat sb;
	char *copy, *p;

	if (path == NULL)
		return (-1);

	if (stat(path, &sb) == 0) {
		if (S_ISDIR(sb.st_mode))
			return (0);
		errno = ENOTDIR;
		return (-1);
	}

	if (errno != ENOENT)
		return (-1);

	/* Create missing parents then the leaf (mkdir -p semantics). */
	copy = strdup(path);
	if (copy == NULL)
		return (-1);
	for (p = copy + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(copy, mode) != 0 && errno != EEXIST) {
			free(copy);
			return (-1);
		}
		*p = '/';
	}
	free(copy);

	if (mkdir(path, mode) != 0) {
		if (errno == EEXIST)
			return (0);
		return (-1);
	}

	return (0);
}

/*
 * Safely write all data to a file descriptor.
 */
int
safe_write(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	size_t written = 0;
	ssize_t ret;

	while (written < n) {
		ret = write(fd, p + written, n - written);
		if (ret < 0) {
			if (errno == EINTR)
				continue;
			return (-1);
		}
		if (ret == 0) {
			errno = EPIPE;
			return (-1);
		}
		written += ret;
	}

	return (0);
}

/*
 * Read entire file into memory.
 * Caller must free the returned buffer.
 */
char *
read_file(const char *path, size_t *len)
{
	struct stat sb;
	char *buf;
	int fd;

	if (path == NULL || len == NULL)
		return (NULL);

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (NULL);

	if (fstat(fd, &sb) != 0) {
		close(fd);
		return (NULL);
	}

	if (!S_ISREG(sb.st_mode)) {
		close(fd);
		errno = EINVAL;
		return (NULL);
	}

	buf = malloc(sb.st_size + 1);
	if (buf == NULL) {
		close(fd);
		return (NULL);
	}

	ssize_t nread = read(fd, buf, sb.st_size);
	close(fd);

	if (nread != sb.st_size) {
		free(buf);
		return (NULL);
	}

	buf[sb.st_size] = '\0';
	*len = sb.st_size;

	return (buf);
}

/*
 * Write data to file.
 */
int
write_file(const char *path, const void *data, size_t len)
{
	int fd;

	if (path == NULL || data == NULL)
		return (-1);

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return (-1);

	if (safe_write(fd, data, len) != 0) {
		close(fd);
		return (-1);
	}

	close(fd);
	return (0);
}

/*
 * Convert container state enum to string.
 */
const char *
ocifbsd_state_to_string(ocifbsd_state_t state)
{
	switch (state) {
	case OCIFBSD_STATE_UNKNOWN:
		return "unknown";
	case OCIFBSD_STATE_CREATED:
		return "created";
	case OCIFBSD_STATE_RUNNING:
		return "running";
	case OCIFBSD_STATE_STOPPED:
		return "stopped";
	case OCIFBSD_STATE_PAUSED:
		return "paused";
	case OCIFBSD_STATE_PAUSED_HIGH:
		return "paused (high priority)";
	default:
		return "unknown";
	}
}

/*
 * Pure access-control decision: may a caller with effective uid `euid` and
 * supplementary group set `groups` (of length `ngroups`) act on ocifbsd
 * state, given the runtime's admin group id `allowed_gid`?
 *
 * The rule is deliberately narrow so that "only root or the admin group"
 * is provably the whole set of principals:
 *   - root (uid 0) is always allowed;
 *   - otherwise the caller must be a member of the configured admin group;
 *   - if no admin group is configured (allowed_gid == (gid_t)-1) or the
 *     group list is empty, only root qualifies.
 * No syscalls here so the policy is unit-testable in isolation; the
 * process's real credentials are gathered by ocifbsd_require_access().
 */
bool
ocifbsd_access_allowed(uid_t euid, gid_t allowed_gid, const gid_t *groups,
    int ngroups)
{
	int i;

	if (euid == 0)
		return (true);
	if (allowed_gid == (gid_t)-1 || groups == NULL)
		return (false);
	for (i = 0; i < ngroups; i++) {
		if (groups[i] == allowed_gid)
			return (true);
	}
	return (false);
}

/*
 * Resolve the configured admin group to a gid, or (gid_t)-1 if the group
 * does not exist on this host (in which case access is root-only).
 */
static gid_t
ocifbsd_admin_gid(void)
{
	struct group *gr;

	gr = getgrnam(OCIFBSD_ADMIN_GROUP);
	return (gr != NULL ? gr->gr_gid : (gid_t)-1);
}

/*
 * Enforce the access policy against the calling process's real credentials.
 * Returns 0 if permitted, or -1 with errno set to EPERM if denied. Both the
 * view and modify operations are currently gated on the same admin group;
 * the operation argument lets that policy diverge later without touching
 * callers.
 */
int
ocifbsd_require_access(enum ocifbsd_access_op op)
{
	gid_t groups[NGROUPS_MAX];
	uid_t euid;
	gid_t admin;
	int ng;

	(void)op;
	euid = geteuid();
	admin = ocifbsd_admin_gid();
	ng = getgroups(NGROUPS_MAX, groups);
	if (ng < 0)
		ng = 0;
	if (ocifbsd_access_allowed(euid, admin, groups, ng))
		return (0);
	errno = EPERM;
	return (-1);
}

/*
 * Restrict a state/config path so that only root and the admin group may
 * reach it: set the requested mode and, when the admin group exists, set the
 * path's group owner to it (leaving the user owner unchanged). Both steps
 * are best-effort — a non-privileged caller cannot chown, and that is fine
 * because such a caller could not have created the file either; the point is
 * that the privileged writer leaves world-inaccessible artifacts behind.
 */
void
ocifbsd_secure_path(const char *path, mode_t mode)
{
	gid_t admin;

	if (path == NULL)
		return;
	(void)chmod(path, mode);
	admin = ocifbsd_admin_gid();
	if (admin != (gid_t)-1)
		(void)chown(path, (uid_t)-1, admin);
}

/*
 * Reconcile a persisted lifecycle state against observed jail liveness.
 *
 * State files record the last state the CLI wrote, but a jail can vanish
 * out from under that record: a host reboot clears every jail, and an
 * operator (or the kernel) can remove a jail without ocifbsd noticing.
 * A container recorded as running or paused whose jail no longer exists
 * is really stopped; every other state is authoritative on its own
 * (a created container has no jail yet, and stopped/unknown never depend
 * on liveness), so those are returned unchanged.
 */
ocifbsd_state_t
ocifbsd_reconcile_state(ocifbsd_state_t stored, bool jail_alive)
{
	switch (stored) {
	case OCIFBSD_STATE_RUNNING:
	case OCIFBSD_STATE_PAUSED:
	case OCIFBSD_STATE_PAUSED_HIGH:
		return (jail_alive ? stored : OCIFBSD_STATE_STOPPED);
	default:
		return (stored);
	}
}

/*
 * Get error string for ocifbsd errors.
 */
const char *
ocifbsd_strerror(int error)
{
	switch (error) {
	case 0:
		return "Success";
	case ENOENT:
		return "Container not found";
	case EINVAL:
		return "Invalid argument";
	case EEXIST:
		return "Container already exists";
	case EBUSY:
		return "Container is busy";
	case ENOTEMPTY:
		return "Container is not empty";
	case EPERM:
		return "Permission denied";
	case EALREADY:
		return "Container is already running";
	default:
		return strerror(error);
	}
}

/*
 * Logging function using syslog-style priorities.
 */
void
ocifbsd_log(int priority, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);

	if (verbose) {
		fprintf(stderr, "ocifbsd[%d]: ", priority);
		vfprintf(stderr, fmt, ap);
		fprintf(stderr, "\n");
	}

	va_end(ap);
}
