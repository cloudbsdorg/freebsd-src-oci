/*-
 * Copyright (c) 2026 REVYTECH, Inc.
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
 * Pod management implementation
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <dirent.h>
#include <json-c/json.h>
#include <errno.h>
#include <fcntl.h>
#include <libutil.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sha256.h>

extern int mkdirp(const char *path, mode_t mode);

#include "orchestration.h"
#include "../include/ocifbsd.h"

/*
 * Global pod registry
 */
static struct pod **pod_registry = NULL;
static int pod_registry_size = 0;
static int pod_registry_count = 0;
static pthread_mutex_t pod_registry_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Generate a unique pod UID
 */
static void
generate_pod_uid(char *uid, size_t len)
{
	SHA256_CTX ctx;
	uint8_t hash[SHA256_DIGEST_LENGTH];
	uint64_t ts;
	
	arc4random_buf(&ts, sizeof(ts));
	
	SHA256_Init(&ctx);
	SHA256_Update(&ctx, &ts, sizeof(ts));
	pthread_t tid = pthread_self();
	SHA256_Update(&ctx, &tid, sizeof(pthread_t));
	SHA256_Final(hash, &ctx);
	
	snprintf(uid, len, "pod-%08x%08x", 
	    (uint32_t)(hash[0] << 24 | hash[1] << 16 | hash[2] << 8 | hash[3]),
	    (uint32_t)(hash[4] << 24 | hash[5] << 16 | hash[6] << 8 | hash[7]));
}

/*
 * Base directory for orchestration state. Defaults to OCIFBSD_ORCH_VAR_DIR but
 * may be redirected with OCIFBSD_ORCH_DIR (mirroring OCIFBSD_STATE_DIR for the
 * runtime) so tests and unprivileged instances can use a writable location.
 */
static const char *
orch_var_dir(void)
{
	const char *e = getenv("OCIFBSD_ORCH_DIR");

	return (e != NULL && e[0] != '\0') ? e : OCIFBSD_ORCH_VAR_DIR;
}

/*
 * Get pod state file path
 */
static char *
get_pod_state_path(const char *name, const char *namespace)
{
	char *path;

	asprintf(&path, "%s/pods/%s/%s.json",
	    orch_var_dir(), namespace, name);
	return (path);
}

/*
 * Save pod state to disk
 */
static int
save_pod_state(struct pod *pod)
{
	FILE *fp;
	char *path;
	
	path = get_pod_state_path(pod->name, pod->namespace);
	if (path == NULL)
		return (-1);

	/*
	 * Ensure the PARENT directory exists. mkdirp on the full path would
	 * create the state file itself as a directory, after which fopen()
	 * fails and the pod is never persisted (so list/delete could not find
	 * it across CLI invocations).
	 */
	{
		char *slash = strrchr(path, '/');

		if (slash != NULL) {
			*slash = '\0';
			if (mkdirp(path, 0755) != 0 && errno != EEXIST) {
				free(path);
				return (-1);
			}
			*slash = '/';
		}
	}

	fp = fopen(path, "w");
	free(path);
	
	if (fp == NULL)
		return (-1);
	
	/* For now, just save basic state */
	fprintf(fp, "{\n");
	fprintf(fp, "  \"uid\": \"%s\",\n", pod->uid);
	fprintf(fp, "  \"name\": \"%s\",\n", pod->name);
	fprintf(fp, "  \"namespace\": \"%s\",\n", pod->namespace);
	if (pod->status)
		fprintf(fp, "  \"state\": %d\n", pod->status->state);
	fprintf(fp, "}\n");
	
	fclose(fp);
	return (0);
}

/*
 * Load pod state from disk
 */
static int
load_pod_state(struct pod *pod)
{
	char *path;
	FILE *fp;
	char buf[1024];
	
	path = get_pod_state_path(pod->name, pod->namespace);
	if (path == NULL)
		return (-1);
	
	fp = fopen(path, "r");
	free(path);
	
	if (fp == NULL)
		return (-1);
	
	/* Simple state restoration - for full JSON parsing, would use json_parser */
	while (fgets(buf, sizeof(buf), fp) != NULL) {
		/* Basic state parsing */
		if (strstr(buf, "\"state\":"))
			continue;
	}

	fclose(fp);
	return (0);
}

/*
 * Reconstruct a pod from its on-disk state file (uid/name/namespace/state).
 * Containers are not persisted, so the loaded pod carries metadata + status
 * only. Returns a newly allocated pod, or NULL if no state file exists. This
 * is what lets `pod list`/`pod get`/`pod delete` see pods created by an
 * earlier CLI process rather than only this process's in-memory registry.
 */
static struct pod *
pod_load_disk(const char *name, const char *namespace)
{
	char *path;
	FILE *fp;
	char buf[1024];
	struct pod *pod;
	struct pod_status *status;
	const char *ns = namespace ? namespace : "default";

	path = get_pod_state_path(name, ns);
	if (path == NULL)
		return (NULL);
	fp = fopen(path, "r");
	free(path);
	if (fp == NULL)
		return (NULL);

	pod = calloc(1, sizeof(*pod));
	status = calloc(1, sizeof(*status));
	if (pod == NULL || status == NULL) {
		free(pod);
		free(status);
		fclose(fp);
		return (NULL);
	}
	strlcpy(pod->name, name, sizeof(pod->name));
	strlcpy(pod->namespace, ns, sizeof(pod->namespace));
	status->state = POD_STATE_PENDING;

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		char tmp[64];
		int st;

		if (strstr(buf, "\"uid\":") != NULL &&
		    sscanf(strstr(buf, "\"uid\":"), "\"uid\": \"%63[^\"]\"",
		    tmp) == 1)
			strlcpy(pod->uid, tmp, sizeof(pod->uid));
		else if (strstr(buf, "\"state\":") != NULL &&
		    sscanf(strstr(buf, "\"state\":"), "\"state\": %d", &st) == 1)
			status->state = (pod_state_t)st;
	}
	fclose(fp);

	strlcpy(status->uid, pod->uid, sizeof(status->uid));
	strlcpy(status->name, pod->name, sizeof(status->name));
	strlcpy(status->namespace, pod->namespace, sizeof(status->namespace));
	pod->status = status;
	return (pod);
}

/*
 * Create a new pod
 */
struct pod *
pod_create(struct pod_spec *spec)
{
	struct pod *pod;
	struct pod_status *status;
	int i;
	
	if (spec == NULL || spec->name[0] == '\0') {
		errno = EINVAL;
		return (NULL);
	}
	
	pod = calloc(1, sizeof(struct pod));
	if (pod == NULL)
		return (NULL);
	
	/* Initialize pod */
	generate_pod_uid(pod->uid, sizeof(pod->uid));
	strlcpy(pod->name, spec->name, sizeof(pod->name));
	strlcpy(pod->namespace, 
	    spec->namespace[0] ? spec->namespace : "default",
	    sizeof(pod->namespace));
	
	/* Copy spec */
	pod->spec = calloc(1, sizeof(struct pod_spec));
	if (pod->spec == NULL) {
		free(pod);
		return (NULL);
	}
	memcpy(pod->spec, spec, sizeof(struct pod_spec));
	/*
	 * The shallow struct copy above aliases the caller's containers
	 * array. Deep-copy it so the pod owns its own buffer and the caller
	 * remains free to release (or reuse) its spec.containers.
	 */
	if (spec->ncontainers > 0 && spec->containers != NULL) {
		pod->spec->containers = calloc(spec->ncontainers,
		    sizeof(struct container_spec));
		if (pod->spec->containers == NULL) {
			free(pod->spec);
			free(pod);
			return (NULL);
		}
		memcpy(pod->spec->containers, spec->containers,
		    spec->ncontainers * sizeof(struct container_spec));
	} else {
		pod->spec->containers = NULL;
		pod->spec->ncontainers = 0;
	}
	
	/* Initialize status */
	status = calloc(1, sizeof(struct pod_status));
	if (status == NULL) {
		free(pod->spec);
		free(pod);
		return (NULL);
	}
	
	strlcpy(status->uid, pod->uid, sizeof(status->uid));
	strlcpy(status->name, pod->name, sizeof(status->name));
	strlcpy(status->namespace, pod->namespace, sizeof(status->namespace));
	status->state = POD_STATE_PENDING;
	status->created = time(NULL);
	status->ncontainers = spec->ncontainers;
	
	/* Container status array */
	if (spec->ncontainers > 0) {
		status->containers = calloc(spec->ncontainers,
		    sizeof(struct container_status));
		if (status->containers == NULL) {
			free(status);
			free(pod->spec);
			free(pod);
			return (NULL);
		}
		for (i = 0; i < spec->ncontainers; i++) {
			strlcpy(status->containers[i].name,
			    spec->containers[i].name,
			    sizeof(status->containers[i].name));
			strlcpy(status->containers[i].image,
			    spec->containers[i].image,
			    sizeof(status->containers[i].image));
			status->containers[i].state = REPLICA_STATE_PENDING;
		}
	}
	
	pod->status = status;
	
	/* Add to registry */
	pthread_mutex_lock(&pod_registry_lock);
	if (pod_registry_count >= pod_registry_size) {
		pod_registry_size = pod_registry_size ? 
		    pod_registry_size * 2 : 16;
		pod_registry = realloc(pod_registry, 
		    pod_registry_size * sizeof(struct pod *));
		if (pod_registry == NULL) {
			pthread_mutex_unlock(&pod_registry_lock);
			free(status->containers);
			free(status);
			free(pod->spec);
			free(pod);
			return (NULL);
		}
	}
	pod_registry[pod_registry_count++] = pod;
	pthread_mutex_unlock(&pod_registry_lock);
	
	/* Save initial state */
	save_pod_state(pod);
	
	/* Publish event */
	orch_event_publish("Normal", "Scheduled", pod->namespace,
	    "Pod %s scheduled", pod->name);
	
	return (pod);
}

/*
 * Start a pod
 */
int
pod_start(struct pod *pod)
{
	int i;
	int ret = 0;

	if (pod == NULL)
		return (-1);
	
	if (pod->status->state == POD_STATE_RUNNING)
		return (0);
	
	/* Start each container in the pod */
	for (i = 0; i < pod->spec->ncontainers; i++) {
		struct container_spec *cs = &pod->spec->containers[i];
		char *container_id = NULL;
		
		/* Create and start container using ocifbsd */
		ret = ocifbsd_create_container(cs->name, cs->image,
		    cs->command, cs->args, pod->uid, &container_id);
		
		if (ret != 0) {
			pod->status->containers[i].state = REPLICA_STATE_FAILED;
			pod->status->containers[i].exit_code = ret;
			ret = -1;
			continue;
		}
		
		ret = ocifbsd_start_container(container_id);
		if (ret != 0) {
			pod->status->containers[i].state = REPLICA_STATE_FAILED;
			pod->status->containers[i].exit_code = ret;
			ret = -1;
		} else {
			pod->status->containers[i].state = REPLICA_STATE_RUNNING;
			strlcpy(pod->status->containers[i].container_id,
			    container_id,
			    sizeof(pod->status->containers[i].container_id));
		}
		
		free(container_id);
	}
	
	/* Update pod status */
	if (ret == 0) {
		pod->status->state = POD_STATE_RUNNING;
		pod->status->started = time(NULL);
		orch_event_publish("Normal", "Started", pod->namespace,
		    "Pod %s started", pod->name);
	} else {
		pod->status->state = POD_STATE_FAILED;
		strlcpy(pod->status->reason, "ContainerStartFailed",
		    sizeof(pod->status->reason));
	}
	
	save_pod_state(pod);
	return (ret);
}

/*
 * Stop a pod
 */
int
pod_stop(struct pod *pod, int sig)
{
	int i;
	int ret = 0;
	
	if (pod == NULL)
		return (-1);
	
	if (pod->status->state != POD_STATE_RUNNING)
		return (0);
	
	/* Stop each container */
	for (i = 0; i < pod->spec->ncontainers; i++) {
		char *cid = pod->status->containers[i].container_id;
		int r;
		
		if (cid == NULL || cid[0] == '\0')
			continue;
		
		r = ocifbsd_stop_container(cid, sig);
		if (r != 0)
			ret = -1;
		else
			pod->status->containers[i].state = REPLICA_STATE_TERMINATED;
	}
	
	pod->status->state = POD_STATE_PENDING;
	save_pod_state(pod);
	
	orch_event_publish("Normal", "Stopped", pod->namespace,
	    "Pod %s stopped", pod->name);
	
	return (ret);
}

/*
 * Delete a pod
 */
int
pod_delete(struct pod *pod)
{
	int i;
	int ret = 0;
	
	if (pod == NULL)
		return (-1);
	
	/* Stop if running */
	if (pod->status != NULL && pod->status->state == POD_STATE_RUNNING)
		pod_stop(pod, SIGTERM);

	/*
	 * Delete each backing container. A pod reconstructed from disk
	 * (pod_load_disk) carries no spec/container status, so guard against
	 * both being absent.
	 */
	if (pod->spec != NULL && pod->status != NULL &&
	    pod->status->containers != NULL) {
		for (i = 0; i < pod->spec->ncontainers; i++) {
			char *cid = pod->status->containers[i].container_id;

			if (cid == NULL || cid[0] == '\0')
				continue;

			if (ocifbsd_delete_container(cid, true) != 0)
				ret = -1;
		}
	}
	
	/* Remove from registry */
	pthread_mutex_lock(&pod_registry_lock);
	for (i = 0; i < pod_registry_count; i++) {
		if (pod_registry[i] == pod) {
			memmove(&pod_registry[i], &pod_registry[i + 1],
			    (pod_registry_count - i - 1) * sizeof(struct pod *));
			pod_registry_count--;
			break;
		}
	}
	pthread_mutex_unlock(&pod_registry_lock);

	/*
	 * Remove the on-disk state file so the pod does not reappear in a later
	 * `pod list`/`pod get` from another process.
	 */
	{
		char *path = get_pod_state_path(pod->name, pod->namespace);

		if (path != NULL) {
			(void)unlink(path);
			free(path);
		}
	}

	/* Free resources */
	pod->status->finished = time(NULL);
	orch_event_publish("Normal", "Deleted", pod->namespace,
	    "Pod %s deleted", pod->name);

	pod_free(pod);
	return (ret);
}

/*
 * Get pod by name
 */
struct pod *
pod_get(const char *name, const char *namespace)
{
	struct pod **p;
	int count;
	
	(void)p;
	(void)count;

	pthread_mutex_lock(&pod_registry_lock);
	for (int i = 0; i < pod_registry_count; i++) {
		if (strcmp(pod_registry[i]->name, name) == 0 &&
		    strcmp(pod_registry[i]->namespace,
		    namespace ? namespace : "default") == 0) {
			struct pod *found = pod_registry[i];
			pthread_mutex_unlock(&pod_registry_lock);
			return (found);
		}
	}
	pthread_mutex_unlock(&pod_registry_lock);

	/*
	 * Not in this process's registry — try the on-disk state so a pod
	 * created by an earlier CLI invocation is still found. (pod_load_disk
	 * does not call back into pod_get/pod_list, so there is no recursion.)
	 */
	{
		struct pod *disk = pod_load_disk(name, namespace);

		if (disk != NULL)
			return (disk);
	}

	errno = ENOENT;
	return (NULL);
}

/*
 * List pods in a namespace from the in-memory registry.
 *
 * This used to scan the on-disk state directory and call pod_get() per
 * file, but pod_get() fell back to pod_list() on a registry miss, so any
 * on-disk pod caused unbounded mutual recursion (and disk pods were never
 * reconstructed anyway — load_pod_state is a stub). Listing the registry
 * is correct for the current in-memory model.
 */
struct pod **
pod_list(const char *namespace, int *count)
{
	struct pod **result;
	const char *ns = namespace ? namespace : "default";
	int alloc = 16;
	int n = 0;
	int i;

	*count = 0;
	result = calloc(alloc, sizeof(struct pod *));
	if (result == NULL)
		return (NULL);

	pthread_mutex_lock(&pod_registry_lock);
	for (i = 0; i < pod_registry_count; i++) {
		if (strcmp(pod_registry[i]->namespace, ns) != 0)
			continue;
		if (n >= alloc) {
			struct pod **grown = realloc(result,
			    alloc * 2 * sizeof(struct pod *));
			if (grown == NULL)
				break;
			result = grown;
			alloc *= 2;
		}
		result[n++] = pod_registry[i];
	}
	pthread_mutex_unlock(&pod_registry_lock);

	/*
	 * Add pods persisted on disk by earlier processes, deduped by name
	 * against what the in-memory registry already contributed.
	 */
	{
		char dirpath[PATH_MAX];
		DIR *d;
		struct dirent *e;

		snprintf(dirpath, sizeof(dirpath), "%s/pods/%s",
		    orch_var_dir(), ns);
		d = opendir(dirpath);
		if (d != NULL) {
			while ((e = readdir(d)) != NULL) {
				char pname[256];
				char *dot;
				struct pod *lp;
				int dup = 0, k;

				if (e->d_name[0] == '.')
					continue;
				strlcpy(pname, e->d_name, sizeof(pname));
				dot = strstr(pname, ".json");
				if (dot == NULL)
					continue;
				*dot = '\0';
				for (k = 0; k < n; k++) {
					if (strcmp(result[k]->name, pname) == 0) {
						dup = 1;
						break;
					}
				}
				if (dup)
					continue;
				lp = pod_load_disk(pname, ns);
				if (lp == NULL)
					continue;
				if (n >= alloc) {
					struct pod **grown = realloc(result,
					    alloc * 2 * sizeof(struct pod *));
					if (grown == NULL) {
						pod_free(lp);
						break;
					}
					result = grown;
					alloc *= 2;
				}
				result[n++] = lp;
			}
			closedir(d);
		}
	}

	*count = n;
	return (result);
}

/*
 * Free pod resources
 */
void
pod_free(struct pod *pod)
{
	if (pod == NULL)
		return;

	if (pod->spec != NULL) {
		free(pod->spec->containers);
		free(pod->spec);
	}
	if (pod->status != NULL) {
		free(pod->status->containers);
		free(pod->status);
	}
	free(pod->state_file);
	free(pod);
}

/*
 * Get pod status
 */
struct pod_status *
pod_get_status(struct pod *pod)
{
	if (pod == NULL)
		return (NULL);
	
	/* Refresh status from containers */
	for (int i = 0; i < pod->spec->ncontainers; i++) {
		char *cid = pod->status->containers[i].container_id;
		if (cid == NULL || cid[0] == '\0')
			continue;
		
		/* Query container state from ocifbsd */
		container_state_t state;
		int exit_code;
		ocifbsd_get_container_state(cid, &state, &exit_code);
		
		switch (state) {
		case CONTAINER_STATE_RUNNING:
			pod->status->containers[i].state = REPLICA_STATE_RUNNING;
			break;
		case CONTAINER_STATE_STOPPED:
			pod->status->containers[i].state = REPLICA_STATE_TERMINATED;
			pod->status->containers[i].exit_code = exit_code;
			break;
		case CONTAINER_STATE_PAUSED:
			pod->status->containers[i].state = REPLICA_STATE_STARTING;
			break;
		default:
			break;
		}
	}
	
	/* Update pod-level state */
	bool all_running = true;
	bool any_failed = false;
	for (int i = 0; i < pod->spec->ncontainers; i++) {
		if (pod->status->containers[i].state != REPLICA_STATE_RUNNING)
			all_running = false;
		if (pod->status->containers[i].state == REPLICA_STATE_FAILED)
			any_failed = true;
	}
	
	if (all_running)
		pod->status->state = POD_STATE_RUNNING;
	else if (any_failed)
		pod->status->state = POD_STATE_FAILED;
	
	return (pod->status);
}

/*
 * Update pod
 */
int
pod_update(struct pod *pod, struct pod_spec *new_spec)
{
	if (pod == NULL || new_spec == NULL)
		return (-1);
	
	/* Stop current pod */
	pod_stop(pod, SIGTERM);
	
	/* Update spec */
	free(pod->spec);
	pod->spec = calloc(1, sizeof(struct pod_spec));
	if (pod->spec == NULL)
		return (-1);
	memcpy(pod->spec, new_spec, sizeof(struct pod_spec));
	
	/* Restart pod with new spec */
	return (pod_start(pod));
}

/*
 * Get pod logs
 */
int
pod_logs(struct pod *pod, const char *container, int tail, bool follow)
{
	int i;
	
	if (pod == NULL)
		return (-1);
	
	if (container == NULL) {
		/* Return logs from first container */
		if (pod->spec->ncontainers == 0)
			return (0);
		container = pod->spec->containers[0].name;
	}
	
	for (i = 0; i < pod->spec->ncontainers; i++) {
		if (strcmp(pod->spec->containers[i].name, container) == 0) {
			char *cid = pod->status->containers[i].container_id;
			if (cid == NULL || cid[0] == '\0')
				return (-1);
			return (ocifbsd_logs(cid, tail, follow));
		}
	}
	
	errno = ENOENT;
	return (-1);
}
