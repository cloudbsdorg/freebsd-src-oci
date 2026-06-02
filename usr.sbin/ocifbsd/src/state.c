/*-
 * Copyright (c) 2024 The FreeBSD Foundation
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
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static int state_fd = -1;

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
{
	pthread_mutex_lock(&state_lock);
	return (0);
}

/*
 * Unlock the state file
 */
void
state_unlock(void)
{
	pthread_mutex_unlock(&state_lock);
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
	int ret = -1;
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
	json_object_object_add(obj, "created_at", json_object_new_int64((json_int_t)c->created_at));
	json_object_object_add(obj, "started_at", json_object_new_int64((json_int_t)c->started_at));
	json_object_object_add(obj, "finished_at", json_object_new_int64((json_int_t)c->finished_at));
	json_object_object_add(obj, "exit_code", json_object_new_int(c->exit_code));
	json_object_object_add(obj, "bundle_path", json_object_new_string(
	    c->bundle_path ? c->bundle_path : ""));
	json_object_object_add(obj, "rootfs", json_object_new_string(
	    c->rootfs ? c->rootfs : ""));
	json_object_object_add(obj, "config_path", json_object_new_string(
	    c->config_path ? c->config_path : ""));

	json_str = strdup(json_object_to_json_string_ext(obj,
	    JSON_C_OBJECT_TO_STRING_PRETTY));
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
	struct json_value *root;
	struct json_object *obj;
	struct ocifbsd_container *c;

	if (id == NULL)
		return (NULL);

	get_state_path(id, path, sizeof(path));

	/* Read file */
	json_str = read_file(path, &json_len);
	if (json_str == NULL)
		return (NULL);

	/* Parse JSON */
	root = json_parse_string(json_str);
	free(json_str);

	if (root == NULL || root->type != JSON_TYPE_OBJECT) {
		if (root)
			json_value_free(root);
		errno = EINVAL;
		return (NULL);
	}

	obj = json_value_object(root);

	/* Allocate container */
	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		json_value_free(root);
		errno = ENOMEM;
		return (NULL);
	}

	/* Parse fields */
#define GET_STRING(field, json_key) do { \
	char *val = json_get_string(root, json_key); \
	if (val) c->field = val; \
} while (0)

#define GET_INT(field, json_key) do { \
	struct json_object *o = json_value_object(root); \
	struct json_value *v = json_object_property_value(o, json_key); \
	if (v && v->type == JSON_TYPE_NUMBER) { \
		struct json_number *n = json_value_number(v); \
		c->field = (int)n->number; \
	} \
} while (0)

#define GET_INT64(field, json_key) do { \
	struct json_object *o = json_value_object(root); \
	struct json_value *v = json_object_property_value(o, json_key); \
	if (v && v->type == JSON_TYPE_NUMBER) { \
		struct json_number *n = json_value_number(v); \
		c->field = (int64_t)n->number; \
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

	/* Parse state string */
	GET_STRING(state_str, "state");

	json_value_free(root);

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
		const char *ext;

		/* Skip . and .. */
		if (entry->d_name[0] == '.')
			continue;

		/* Only process .json files */
		ext = strrchr(entry->d_name, '.');
		if (ext == NULL || strcmp(ext, ".json") != 0)
			continue;

		/* Load container state */
		*entry->d_name = '\0';  /* Remove .json extension */
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
