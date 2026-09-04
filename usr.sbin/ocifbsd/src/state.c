/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
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
 * Container state persistence
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/syslimits.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "ocifbsd.h"

/* Process/jail liveness probe (src/procutil.c). */
extern bool pid_in_jail(pid_t pid, int jid);

/*
 * Runtime state directory. Defaults to OCIFBSD_STATE_DIR but may be
 * redirected with the OCIFBSD_STATE_DIR environment variable (mirroring
 * OCIFBSD_DATA_DIR for the image store), which is useful for tests and for
 * running an unprivileged, self-contained instance.
 */
const char *
state_base_dir(void)
{
	const char *e = getenv("OCIFBSD_STATE_DIR");

	return (e != NULL && e[0] != '\0') ? e : OCIFBSD_STATE_DIR;
}
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Initialize state directory
 */
int
state_init(void)
{
	if (ensure_directory(state_base_dir(), OCIFBSD_STATE_DIR_MODE) != 0) {
		fprintf(stderr, "error: failed to create state directory: %s\n",
		    strerror(errno));
		return (-1);
	}
	/*
	 * Enforce the restrictive mode/group even if the directory already
	 * existed (e.g. created world-readable by an older build): container
	 * state must not be readable or modifiable by unprivileged users.
	 */
	ocifbsd_secure_path(state_base_dir(), OCIFBSD_STATE_DIR_MODE);

	return (0);
}

/*
 * Lock the state file
 */
int
state_lock(void)
__attribute__((no_thread_safety_analysis));
int
state_lock(void)
{
	pthread_mutex_lock(&state_mutex);
	return (0);
}

/*
 * Unlock the state file
 */
void
state_unlock(void)
__attribute__((no_thread_safety_analysis));
void
state_unlock(void)
{
	pthread_mutex_unlock(&state_mutex);
}

/*
 * A container id/name is used as a single path component of files created and
 * opened as root under the state dir. resolve_cid() passes an unmatched CLI
 * argument through verbatim, so reject anything that could escape the dir — a
 * slash, or a lone "." / "..". Real ids are hex and resolved names are single
 * components, so this rejects only hostile input.
 */
static bool
state_id_is_safe(const char *id)
{
	return (id != NULL && id[0] != '\0' &&
	    strchr(id, '/') == NULL &&
	    strcmp(id, ".") != 0 && strcmp(id, "..") != 0);
}

/*
 * Get state file path for a container
 */
static void
get_state_path(const char *id, char *path, size_t path_len)
{
	snprintf(path, path_len, "%s/%s.json", state_base_dir(), id);
}

/*
 * Name -> id index.
 *
 * Resolving a human-readable name used to be a full scan of the state
 * directory that loaded every container completely -- including parsing each
 * one's OCI runtime spec off disk and realpath()ing its root -- purely to
 * compare a name string. That is O(containers * spec size) work for an O(1)
 * question, paid by every subcommand a user invokes by name.
 *
 * The index is one small file per name under <state>/by-name/ holding the
 * container id. It is a CACHE, not the source of truth: a lookup verifies the
 * id it read still exists and still carries that name, and any miss falls back
 * to the directory scan. So a stale, missing, or hand-deleted index is a
 * performance question only, never a correctness one, and an older state
 * directory with no index keeps working unchanged.
 */
static int
name_index_path(const char *name, char *path, size_t path_len)
{
	int n;

	/*
	 * The name becomes a single path component under a root-owned
	 * directory, so it must not contain '/' or be "."/"..". Names that
	 * cannot be indexed safely simply are not indexed; the scan still
	 * finds them.
	 */
	if (!state_id_is_safe(name) || name[0] == '.')
		return (-1);
	n = snprintf(path, path_len, "%s/by-name/%s", state_base_dir(), name);
	if (n < 0 || (size_t)n >= path_len)
		return (-1);
	return (0);
}

static void
name_index_put(const char *name, const char *id)
{
	char dir[PATH_MAX], path[PATH_MAX], tmp[PATH_MAX];
	int fd, n;

	if (name == NULL || id == NULL || name_index_path(name, path,
	    sizeof(path)) != 0)
		return;
	n = snprintf(dir, sizeof(dir), "%s/by-name", state_base_dir());
	if (n < 0 || (size_t)n >= sizeof(dir))
		return;
	if (mkdir(dir, OCIFBSD_STATE_DIR_MODE) != 0 && errno != EEXIST)
		return;
	ocifbsd_secure_path(dir, OCIFBSD_STATE_DIR_MODE);

	n = snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
	if (n < 0 || (size_t)n >= sizeof(tmp))
		return;
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, OCIFBSD_STATE_FILE_MODE);
	if (fd < 0)
		return;
	if (safe_write(fd, id, strlen(id)) != 0) {
		close(fd);
		unlink(tmp);
		return;
	}
	close(fd);
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return;
	}
	ocifbsd_secure_path(path, OCIFBSD_STATE_FILE_MODE);
}

static void
name_index_remove(const char *name)
{
	char path[PATH_MAX];

	if (name == NULL || name_index_path(name, path, sizeof(path)) != 0)
		return;
	(void)unlink(path);
}

/*
 * Cross-process advisory lock for a single container's lifecycle.
 *
 * The in-process state_mutex above only serializes threads within one
 * process; two concurrent CLI invocations on the same container (e.g.
 * `start` racing `network set`) are separate processes and share no such
 * mutex. state_lock_container takes an exclusive flock(2) on a per-container
 * lock file so that the whole read-check-act sequence of a lifecycle op is
 * mutually excluded across processes. The returned fd is passed to
 * state_unlock_container, which releases the lock and closes it.
 *
 * The lock file lives alongside the state JSON, is created with the same
 * restrictive owner/mode as other state (root / ocifbsd group only), and is
 * intentionally left in place after unlock: an empty marker is cheap and
 * removing it would reintroduce a create/open race between contenders.
 *
 * Returns a non-negative fd on success, or -1 on error (errno set). A -1
 * return from a failed lock must be treated as "do not proceed": callers
 * fail closed rather than act without exclusion.
 */
int
state_lock_container(const char *id)
{
	char path[PATH_MAX];
	int fd;

	if (!state_id_is_safe(id)) {
		errno = EINVAL;
		return (-1);
	}
	/* Ensure the state directory exists and is secured first. */
	if (state_init() != 0)
		return (-1);
	if (snprintf(path, sizeof(path), "%s/%s.lock", state_base_dir(), id) >=
	    (int)sizeof(path)) {
		errno = ENAMETOOLONG;
		return (-1);
	}
	fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, OCIFBSD_STATE_FILE_MODE);
	if (fd < 0)
		return (-1);
	/* Best-effort tighten of owner/mode in case of a pre-existing file. */
	ocifbsd_secure_path(path, OCIFBSD_STATE_FILE_MODE);
	if (flock(fd, LOCK_EX) != 0) {
		int saved = errno;

		close(fd);
		errno = saved;
		return (-1);
	}
	return (fd);
}

/*
 * Release a lock taken by state_lock_container. A negative fd is a no-op so
 * callers can unconditionally unlock in a cleanup path.
 */
void
state_unlock_container(int fd)
{
	if (fd < 0)
		return;
	(void)flock(fd, LOCK_UN);
	(void)close(fd);
}

/*
 * Save container state to disk
 */
int
state_save(const struct ocifbsd_container *c)
{
	char path[PATH_MAX];
	char *json_str;
	int fd;
	struct json_object *obj;

	if (c == NULL || c->id == NULL) {
		errno = EINVAL;
		return (-1);
	}

	get_state_path(c->id, path, sizeof(path));

	/* Create JSON representation */
	obj = json_object_new_object();
	if (obj == NULL) {
		errno = ENOMEM;
		return (-1);
	}

	json_object_object_add(obj, "id", json_object_new_string(c->id));
	json_object_object_add(obj, "name", json_object_new_string(c->name ? c->name : ""));
	json_object_object_add(obj, "state", json_object_new_string(
	    ocifbsd_state_to_string(c->state)));
	json_object_object_add(obj, "jid", json_object_new_int(c->jid));
	json_object_object_add(obj, "init_pid", json_object_new_int((int)c->init_pid));
	json_object_object_add(obj, "created_at", json_object_new_int64((int64_t)c->created_at));
	json_object_object_add(obj, "started_at", json_object_new_int64((int64_t)c->started_at));
	json_object_object_add(obj, "finished_at", json_object_new_int64((int64_t)c->finished_at));
	json_object_object_add(obj, "exit_code", json_object_new_int(c->exit_code));
	json_object_object_add(obj, "bundle_path", json_object_new_string(
	    c->bundle_path ? c->bundle_path : ""));
	json_object_object_add(obj, "rootfs", json_object_new_string(
	    c->rootfs ? c->rootfs : ""));
	json_object_object_add(obj, "config_path", json_object_new_string(
	    c->config_path ? c->config_path : ""));

	/*
	 * json_object_to_json_string_ext() returns NULL if it cannot allocate
	 * its print buffer. Passing that straight to strdup() dereferences
	 * NULL; capture and check it first.
	 */
	{
		const char *rendered = json_object_to_json_string_ext(obj,
		    JSON_C_TO_STRING_PRETTY);

		json_str = (rendered != NULL) ? strdup(rendered) : NULL;
	}
	json_object_put(obj);

	if (json_str == NULL) {
		errno = ENOMEM;
		return (-1);
	}

	/*
	 * Write atomically: write to a temp file, fsync, then rename over the
	 * target. A crash or a concurrent reader never sees a truncated or
	 * half-written state file (the previous O_TRUNC write did).
	 */
	{
		char tmp[PATH_MAX];
		int n;

		n = snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());
		if (n < 0 || (size_t)n >= sizeof(tmp)) {
			free(json_str);
			errno = ENAMETOOLONG;
			return (-1);
		}

		fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC,
		    OCIFBSD_STATE_FILE_MODE);
		if (fd < 0) {
			free(json_str);
			return (-1);
		}

		if (safe_write(fd, json_str, strlen(json_str)) != 0) {
			close(fd);
			unlink(tmp);
			free(json_str);
			return (-1);
		}

		(void)fsync(fd);
		close(fd);
		free(json_str);

		if (rename(tmp, path) != 0) {
			unlink(tmp);
			return (-1);
		}
		/*
		 * The umask can widen the create mode, and the file may carry
		 * a stale mode after rename over an older world-readable file;
		 * pin the restrictive mode/group on the final path.
		 */
		ocifbsd_secure_path(path, OCIFBSD_STATE_FILE_MODE);
	}

	/*
	 * Refresh the name index. Best effort by design: the index is a cache
	 * and every lookup falls back to the scan, so a failure here must not
	 * fail the save of the authoritative state file.
	 */
	if (c->name != NULL && c->name[0] != '\0' &&
	    strcmp(c->name, c->id) != 0)
		name_index_put(c->name, c->id);

	return (0);
}

/*
 * Load a container's lifecycle METADATA from disk: everything the state file
 * itself records (id, name, state, jid, pids, paths), with the persisted state
 * reconciled against jail liveness -- but WITHOUT reloading the OCI runtime
 * spec from config.json.
 *
 * Callers that only ask about identity or status (list, ps, stats, name
 * resolution) pay a single small JSON parse per container instead of also
 * parsing a full runtime spec and realpath()ing its root. state_load() is this
 * plus the spec, for the lifecycle paths that genuinely need it.
 */
struct ocifbsd_container *
state_load_meta(const char *id)
{
	char path[PATH_MAX];
	char *json_str;
	size_t json_len;
	struct json_object *root;
	struct ocifbsd_container *c;
	bool oom;

	/*
	 * Reject an unsafe id before it becomes a path opened as root: without
	 * this, `ocifbsd inspect '../../etc/x'` would state_load + parse
	 * attacker-controlled JSON from outside the state dir (the lifecycle
	 * commands are protected by state_lock_container, but read paths are not).
	 */
	if (!state_id_is_safe(id))
		return (NULL);

	get_state_path(id, path, sizeof(path));

	/* Read file */
	json_str = read_file(path, &json_len);
	if (json_str == NULL)
		return (NULL);

	/* Parse JSON */
	root = json_tokener_parse(json_str);
	free(json_str);

	if (root == NULL || json_object_get_type(root) != json_type_object) {
		if (root)
			json_object_put(root);
		errno = EINVAL;
		return (NULL);
	}

	/* Allocate container */
	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		json_object_put(root);
		errno = ENOMEM;
		return (NULL);
	}

	/*
	 * Parse fields.
	 *
	 * A strdup() failure on a field that IS present must fail the whole
	 * load: returning a container whose id or config_path is NULL looks
	 * like a successful parse to every caller, and c->id then reaches
	 * snprintf("%.12s")/strcmp as a NULL pointer. Track it and bail.
	 */
	oom = false;
#define GET_STRING(field, json_key) do { \
	struct json_object *_v = json_object_object_get(root, json_key); \
	if (_v && json_object_get_type(_v) == json_type_string) { \
		c->field = strdup(json_object_get_string(_v)); \
		if (c->field == NULL) \
			oom = true; \
	} \
} while (0)

#define GET_INT(field, json_key) do { \
	struct json_object *_v = json_object_object_get(root, json_key); \
	if (_v && json_object_get_type(_v) == json_type_int) { \
		c->field = json_object_get_int(_v); \
	} \
} while (0)

/*
 * Timestamps are written with json_object_new_int64 and time_t is 64-bit on
 * FreeBSD, so they must be read back with json_object_get_int64: the 32-bit
 * accessor saturates at INT32_MAX, which silently corrupts every timestamp
 * past 2038-01-19 into the same value and breaks any age/uptime arithmetic.
 */
#define GET_INT64(field, json_key) do { \
	struct json_object *_v = json_object_object_get(root, json_key); \
	if (_v && json_object_get_type(_v) == json_type_int) { \
		c->field = (int64_t)json_object_get_int64(_v); \
	} \
} while (0)

	GET_STRING(id, "id");
	GET_STRING(name, "name");
	GET_STRING(bundle_path, "bundle_path");
	GET_STRING(rootfs, "rootfs");
	GET_STRING(config_path, "config_path");
	GET_INT(jid, "jid");
	GET_INT(init_pid, "init_pid");
	GET_INT64(created_at, "created_at");
	GET_INT64(started_at, "started_at");
	GET_INT64(finished_at, "finished_at");
	GET_INT(exit_code, "exit_code");

	if (oom) {
		json_object_put(root);
		container_free(c);
		errno = ENOMEM;
		return (NULL);
	}

	/* Parse state string -> enum */
	{
		struct json_object *_state_v = json_object_object_get(root, "state");
		if (_state_v && json_object_get_type(_state_v) == json_type_string) {
			const char *_s = json_object_get_string(_state_v);
			if (strcmp(_s, "created") == 0)
				c->state = OCIFBSD_STATE_CREATED;
			else if (strcmp(_s, "running") == 0)
				c->state = OCIFBSD_STATE_RUNNING;
			else if (strcmp(_s, "stopped") == 0)
				c->state = OCIFBSD_STATE_STOPPED;
			else if (strcmp(_s, "paused") == 0)
				c->state = OCIFBSD_STATE_PAUSED;
		}
	}

	json_object_put(root);

	/*
	 * Reconcile the persisted state against reality. The state file records
	 * the last status the CLI wrote, but a jail can disappear without our
	 * knowledge (host reboot, an operator running jail -r, init exiting
	 * while no ocifbsd process was watching). A container recorded as
	 * running or paused whose init PID no longer lives in its jail is
	 * really stopped; report it that way instead of trusting a stale file.
	 * This mirrors the liveness signal the kill/stop paths already use.
	 */
	c->state = ocifbsd_reconcile_state(c->state,
	    pid_in_jail(c->init_pid, c->jid));

	return (c);
}

/*
 * Load container state from disk, including the OCI runtime spec.
 *
 * State files store lifecycle metadata only, so the full spec is reloaded from
 * config.json here -- start/exec need it and the CLI is multi-invocation, so
 * nothing carries it over in memory.
 */
struct ocifbsd_container *
state_load(const char *id)
{
	struct ocifbsd_container *c;

	c = state_load_meta(id);
	if (c == NULL)
		return (NULL);

	if (c->config_path != NULL) {
		c->spec = oci_parse_config(c->config_path);
		if (c->spec != NULL && c->rootfs != NULL &&
		    c->spec->root.path != NULL &&
		    c->spec->root.path[0] != '/' &&
		    c->bundle_path != NULL) {
			char abspath[PATH_MAX];
			char *resolved;
			int n;

			/*
			 * A truncated join must NOT be resolved: the prefix
			 * that survives truncation can name a completely
			 * different, existing directory, and this value
			 * becomes the jail root that mount/jail_set later use
			 * as root. Leave root.path as-is on overflow rather
			 * than pointing the jail somewhere the spec never
			 * named. (container_create makes the same check at
			 * create time; this is the reload path.)
			 */
			n = snprintf(abspath, sizeof(abspath), "%s/%s",
			    c->bundle_path, c->spec->root.path);
			if (n < 0 || (size_t)n >= sizeof(abspath))
				return (c);
			resolved = realpath(abspath, NULL);
			if (resolved == NULL)
				resolved = strdup(abspath);
			if (resolved != NULL) {
				free(c->spec->root.path);
				c->spec->root.path = resolved;
			}
		}
	}

	return (c);
}

/*
 * Delete container state from disk
 */
int
state_delete(const char *id)
{
	char path[PATH_MAX];
	struct ocifbsd_container *c;

	if (!state_id_is_safe(id)) {
		errno = EINVAL;
		return (-1);
	}

	/*
	 * Drop this container's name-index entry before removing the state
	 * file. The name lives in the state file, so it has to be read first;
	 * a metadata-only load is enough (no spec parse needed to learn a
	 * name). A stale entry left behind would still resolve correctly --
	 * lookups verify the target -- but removing it keeps the index tidy
	 * and lets a later container reuse the name.
	 */
	c = state_load_meta(id);
	if (c != NULL) {
		if (c->name != NULL && c->id != NULL &&
		    strcmp(c->name, c->id) != 0)
			name_index_remove(c->name);
		container_free(c);
	}

	get_state_path(id, path, sizeof(path));

	if (unlink(path) != 0 && errno != ENOENT) {
		return (-1);
	}

	return (0);
}

/*
 * Resolve a container name to its id via the index.
 *
 * Returns a newly allocated id string (caller frees) on a verified hit, or
 * NULL when the name is not indexed, the index is stale, or the name is not a
 * safe path component. NULL is never conclusive: the caller falls back to the
 * directory scan, which remains the source of truth.
 */
char *
state_lookup_name(const char *name)
{
	char path[PATH_MAX];
	char *id;
	size_t len;
	struct ocifbsd_container *c;
	bool ok;

	if (name == NULL || name_index_path(name, path, sizeof(path)) != 0)
		return (NULL);

	id = read_file(path, &len);
	if (id == NULL)
		return (NULL);
	/* Trim any trailing newline a human may have added. */
	while (len > 0 && (id[len - 1] == '\n' || id[len - 1] == '\r'))
		id[--len] = '\0';
	if (len == 0 || !state_id_is_safe(id)) {
		free(id);
		return (NULL);
	}

	/*
	 * Verify: the indexed container must still exist AND still carry this
	 * name. Without this check a stale entry (state file removed behind
	 * our back, or the name reused) would silently resolve to the wrong
	 * container, which is far worse than a slow lookup.
	 */
	c = state_load_meta(id);
	if (c == NULL) {
		free(id);
		return (NULL);
	}
	ok = (c->name != NULL && strcmp(c->name, name) == 0);
	container_free(c);
	if (!ok) {
		free(id);
		return (NULL);
	}
	return (id);
}

/*
 * List all containers in state directory
 */
struct ocifbsd_container **
state_list(int *n)
{
	struct ocifbsd_container **list = NULL;
	struct ocifbsd_container *c;
	DIR *dir;
	struct dirent *entry;
	int count = 0;
	int capacity = 0;

	*n = 0;

	dir = opendir(state_base_dir());
	if (dir == NULL) {
		/*
		 * A missing state directory means no containers exist yet, not
		 * a failure: return an allocated, NULL-terminated empty list.
		 * A genuine error (e.g. EACCES) returns NULL so callers can
		 * distinguish "no containers" from "could not read state" and
		 * fail closed on the latter.
		 */
		if (errno == ENOENT) {
			list = calloc(1, sizeof(*list));  /* [NULL] */
			return (list);
		}
		return (NULL);
	}

	while ((entry = readdir(dir)) != NULL) {
		char *ext;

		/* Skip . and .. */
		if (entry->d_name[0] == '.')
			continue;

		/* Only process .json files */
		ext = strrchr(entry->d_name, '.');
		if (ext == NULL || strcmp(ext, ".json") != 0)
			continue;

		/*
		 * Load container state (strip the ".json" extension).
		 * Metadata only: no caller of state_list() reads ->spec, and
		 * parsing every container's runtime spec here made listing
		 * O(containers * spec size) for information the state file
		 * already holds.
		 */
		*ext = '\0';
		c = state_load_meta(entry->d_name);
		if (c == NULL)
			continue;

		/* Expand list if needed */
		if (count >= capacity) {
			capacity = capacity ? capacity * 2 : 16;
			void *_t = realloc(list, capacity * sizeof(*list));
			if (_t == NULL) {
				/* Free the entries loaded so far and the array
				 * (realloc into a temp so we don't lose it). */
				for (int k = 0; k < count; k++)
					container_free(list[k]);
				free(list);
				closedir(dir);
				*n = 0;
				return (NULL);
			}
			list = _t;
		}

		list[count++] = c;
	}

	closedir(dir);

	/*
	 * Return a NULL-terminated array. For an existing-but-empty store
	 * (count == 0) allocate a [NULL] list rather than returning NULL, so
	 * NULL is reserved for genuine errors.
	 */
	{
		void *_t = realloc(list, (count + 1) * sizeof(*list));
		if (_t == NULL) {
			for (int k = 0; k < count; k++)
				container_free(list[k]);
			free(list);
			*n = 0;
			return (NULL);
		}
		list = _t;
	}
	list[count] = NULL;

	*n = count;
	return (list);
}
