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

static const char *state_dir = OCIFBSD_STATE_DIR;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Initialize state directory
 */
int
state_init(void)
{
	if (ensure_directory(state_dir, 0755) != 0) {
		fprintf(stderr, "error: failed to create state directory: %s\n",
		    strerror(errno));
		return (-1);
	}

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
 * Get state file path for a container
 */
static void
get_state_path(const char *id, char *path, size_t path_len)
{
	snprintf(path, path_len, "%s/%s.json", state_dir, id);
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

	json_str = strdup(json_object_to_json_string_ext(obj,
	    JSON_C_TO_STRING_PRETTY));
	json_object_put(obj);

	if (json_str == NULL) {
		errno = ENOMEM;
		return (-1);
	}

	/* Write to file */
	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		free(json_str);
		return (-1);
	}

	if (safe_write(fd, json_str, strlen(json_str)) != 0) {
		close(fd);
		free(json_str);
		return (-1);
	}

	close(fd);
	free(json_str);

	return (0);
}

/*
 * Load container state from disk
 */
struct ocifbsd_container *
state_load(const char *id)
{
	char path[PATH_MAX];
	char *json_str;
	size_t json_len;
	struct json_object *root;
	struct ocifbsd_container *c;

	if (id == NULL)
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

	/* Parse fields */
#define GET_STRING(field, json_key) do { \
	struct json_object *_v = json_object_object_get(root, json_key); \
	if (_v && json_object_get_type(_v) == json_type_string) { \
		c->field = strdup(json_object_get_string(_v)); \
	} \
} while (0)

#define GET_INT(field, json_key) do { \
	struct json_object *_v = json_object_object_get(root, json_key); \
	if (_v && json_object_get_type(_v) == json_type_int) { \
		c->field = json_object_get_int(_v); \
	} \
} while (0)

#define GET_INT64(field, json_key) do { \
	struct json_object *_v = json_object_object_get(root, json_key); \
	if (_v && json_object_get_type(_v) == json_type_int) { \
		c->field = (int64_t)json_object_get_int(_v); \
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
	 * State files store lifecycle metadata only. Reload the full OCI
	 * runtime spec from config.json so start/exec work after process
	 * restart (CLI is multi-invocation).
	 */
	if (c->config_path != NULL) {
		c->spec = oci_parse_config(c->config_path);
		if (c->spec != NULL && c->rootfs != NULL &&
		    c->spec->root.path != NULL &&
		    c->spec->root.path[0] != '/' &&
		    c->bundle_path != NULL) {
			char abspath[PATH_MAX];
			char *resolved;

			snprintf(abspath, sizeof(abspath), "%s/%s",
			    c->bundle_path, c->spec->root.path);
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

	if (id == NULL) {
		errno = EINVAL;
		return (-1);
	}

	get_state_path(id, path, sizeof(path));

	if (unlink(path) != 0 && errno != ENOENT) {
		return (-1);
	}

	return (0);
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

	dir = opendir(state_dir);
	if (dir == NULL)
		return (NULL);

	while ((entry = readdir(dir)) != NULL) {
		char *ext;

		/* Skip . and .. */
		if (entry->d_name[0] == '.')
			continue;

		/* Only process .json files */
		ext = strrchr(entry->d_name, '.');
		if (ext == NULL || strcmp(ext, ".json") != 0)
			continue;

		/* Load container state (strip the ".json" extension) */
		*ext = '\0';
		c = state_load(entry->d_name);
		if (c == NULL)
			continue;

		/* Expand list if needed */
		if (count >= capacity) {
			capacity = capacity ? capacity * 2 : 16;
			list = realloc(list, capacity * sizeof(*list));
			if (list == NULL) {
				closedir(dir);
				*n = count;
				return (NULL);
			}
		}

		list[count++] = c;
	}

	closedir(dir);

	/* Null-terminate list */
	if (count > 0) {
		list = realloc(list, (count + 1) * sizeof(*list));
		if (list == NULL) {
			*n = 0;
			return (NULL);
		}
		list[count] = NULL;
	}

	*n = count;
	return (list);
}
