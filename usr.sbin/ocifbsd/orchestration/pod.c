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
 * Append to the registry, growing it if needed. Caller holds the registry
 * lock. Returns 0 on success, -1 on allocation failure with the registry
 * left untouched.
 *
 * The size is committed ONLY after the growth succeeds. The open-coded
 * version this replaces doubled pod_registry_size before calling
 * ocifbsd_realloc_grow(), so a failed growth left the recorded capacity
 * larger than the actual array -- and the next insert, seeing count <
 * size, skipped the growth entirely and wrote past the end of the
 * allocation.
 */
static int
pod_registry_insert_locked(struct pod *pod)
{
	int newsize;

	if (pod_registry_count >= pod_registry_size) {
		newsize = pod_registry_size ? pod_registry_size * 2 : 16;
		if (ocifbsd_realloc_grow((void **)&pod_registry,
		    newsize * sizeof(struct pod *)) != 0)
			return (-1);
		pod_registry_size = newsize;
	}
	pod_registry[pod_registry_count++] = pod;
	return (0);
}

/*
 * Identity Map: adopt a pod just reconstructed from disk into the registry,
 * and return the one canonical in-memory object for that identity.
 *
 * pod_get()/pod_list() used to return the disk-loaded object directly. That
 * made their return value ambiguous -- borrowed when the pod was already in
 * the registry, owned when it came off disk -- so no caller could be correct:
 * freeing crashed on registry pods, not freeing leaked disk pods, and every
 * call site in the tree chose the leak. Worse, two lookups of the same
 * on-disk pod produced two independent objects, so a mutation through one was
 * invisible through the other.
 *
 * Interning fixes both: after this call the registry owns the pod, every
 * lookup of that name returns the same pointer, and the contract is uniform
 * -- pod_get()/pod_list() ALWAYS return borrowed pointers that the caller
 * must never free. The pod is released by pod_delete().
 *
 * Returns the canonical pod (which may be a pre-existing entry, in which case
 * the argument is freed), or NULL on allocation failure (argument freed).
 */
static struct pod *
pod_registry_intern(struct pod *pod)
{
	struct pod *existing = NULL;
	int i;

	if (pod == NULL)
		return (NULL);

	pthread_mutex_lock(&pod_registry_lock);
	/*
	 * Re-check under the lock: another thread may have interned this same
	 * identity between our registry miss and now. Returning the winner
	 * preserves the one-object-per-identity invariant.
	 */
	for (i = 0; i < pod_registry_count; i++) {
		if (strcmp(pod_registry[i]->name, pod->name) == 0 &&
		    strcmp(pod_registry[i]->namespace, pod->namespace) == 0) {
			existing = pod_registry[i];
			break;
		}
	}
	if (existing != NULL) {
		pthread_mutex_unlock(&pod_registry_lock);
		pod_free(pod);
		return (existing);
	}
	if (pod_registry_insert_locked(pod) != 0) {
		pthread_mutex_unlock(&pod_registry_lock);
		pod_free(pod);
		return (NULL);
	}
	pthread_mutex_unlock(&pod_registry_lock);
	return (pod);
}

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

	/*
	 * Widen BEFORE shifting: hash[0] promotes to int, so hash[0] << 24
	 * overflows a signed 32-bit int whenever the top bit is set (half the
	 * time) -- undefined behaviour in the middle of a uniqueness routine.
	 */
	snprintf(uid, len, "pod-%08x%08x",
	    ((uint32_t)hash[0] << 24) | ((uint32_t)hash[1] << 16) |
	    ((uint32_t)hash[2] << 8) | (uint32_t)hash[3],
	    ((uint32_t)hash[4] << 24) | ((uint32_t)hash[5] << 16) |
	    ((uint32_t)hash[6] << 8) | (uint32_t)hash[7]);
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
/*
 * A pod/service/stack name or namespace becomes a path component under the
 * root-owned state dir, so it MUST NOT contain '/', "..", NUL, or a leading
 * '.'/'-'. Without this, `pod create --name ../../etc/cron.d/x` wrote — and
 * `pod delete` unlinked — arbitrary files as root (path traversal). Mirrors
 * ns_name_is_valid in namespace.c. Empty/NULL is invalid.
 */
bool
orch_name_is_valid(const char *s)
{
	size_t i, len;

	if (s == NULL)
		return (false);
	len = strlen(s);
	if (len == 0 || len > 63)
		return (false);
	if (s[0] == '.' || s[0] == '-')
		return (false);
	for (i = 0; i < len; i++) {
		char c = s[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
		    (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
			return (false);
	}
	return (true);
}

static char *
get_pod_state_path(const char *name, const char *namespace)
{
	char *path;

	if (!orch_name_is_valid(name) || !orch_name_is_valid(namespace))
		return (NULL);
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

	/*
	 * Write to a temp file and rename(2) into place so a concurrent reader
	 * (the design reads state cross-process) never sees a half-written or
	 * truncated JSON file. fopen("w") truncated in place.
	 */
	char *tmppath;
	if (asprintf(&tmppath, "%s.tmp", path) == -1) {
		free(path);
		return (-1);
	}
	fp = fopen(tmppath, "w");
	if (fp == NULL) {
		free(tmppath);
		free(path);
		return (-1);
	}

	fprintf(fp, "{\n");
	fprintf(fp, "  \"uid\": \"%s\",\n", pod->uid);
	fprintf(fp, "  \"name\": \"%s\",\n", pod->name);
	fprintf(fp, "  \"namespace\": \"%s\",\n", pod->namespace);
	fprintf(fp, "  \"state\": %d,\n",
	    pod->status != NULL ? pod->status->state : 0);
	/*
	 * Persist the backing container ids so a later `delete` (from another
	 * process, which reconstructs the pod from disk) can tear the jails
	 * down instead of leaking them. Written as indexed "cid<N>" keys.
	 */
	{
		int nc = 0, i;

		if (pod->status != NULL && pod->status->containers != NULL) {
			for (i = 0; i < pod->status->ncontainers; i++) {
				if (pod->status->containers[i].container_id[0]
				    != '\0')
					fprintf(fp, "  \"cid%d\": \"%s\",\n",
					    nc++,
					    pod->status->containers[i].container_id);
			}
		}
		fprintf(fp, "  \"ncids\": %d\n", nc);
	}
	fprintf(fp, "}\n");

	/*
	 * Only publish a COMPLETE file. A short write (ENOSPC, quota) left
	 * ferror set, yet the rename went ahead and atomically replaced good
	 * state with truncated JSON -- which a later process would parse as a
	 * pod with fewer container ids, silently leaking the jails those ids
	 * named. Check the stream before committing, and treat a failing
	 * fclose (where buffered data is actually flushed) the same way.
	 */
	if (ferror(fp) != 0) {
		fclose(fp);
		unlink(tmppath);
		free(tmppath);
		free(path);
		return (-1);
	}
	if (fclose(fp) != 0) {
		unlink(tmppath);
		free(tmppath);
		free(path);
		return (-1);
	}
	if (rename(tmppath, path) != 0) {
		unlink(tmppath);
		free(tmppath);
		free(path);
		return (-1);
	}
	free(tmppath);
	free(path);
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

	struct container_status cids[256];
	int ncids = 0;

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		char tmp[128];
		int st, idx;

		if (strstr(buf, "\"uid\":") != NULL &&
		    sscanf(strstr(buf, "\"uid\":"), "\"uid\": \"%63[^\"]\"",
		    tmp) == 1)
			strlcpy(pod->uid, tmp, sizeof(pod->uid));
		else if (sscanf(buf, " \"cid%d\": \"%127[^\"]\"", &idx, tmp)
		    == 2 && ncids < (int)(sizeof(cids) / sizeof(cids[0]))) {
			memset(&cids[ncids], 0, sizeof(cids[ncids]));
			strlcpy(cids[ncids].container_id, tmp,
			    sizeof(cids[ncids].container_id));
			cids[ncids].state = REPLICA_STATE_RUNNING;
			ncids++;
		} else if (strstr(buf, "\"state\":") != NULL &&
		    sscanf(strstr(buf, "\"state\":"), "\"state\": %d", &st) == 1)
			status->state = (pod_state_t)st;
	}
	fclose(fp);

	strlcpy(status->uid, pod->uid, sizeof(status->uid));
	strlcpy(status->name, pod->name, sizeof(status->name));
	strlcpy(status->namespace, pod->namespace, sizeof(status->namespace));
	/* Restore the backing containers so pod_delete can tear them down. */
	if (ncids > 0) {
		status->containers = calloc(ncids, sizeof(*status->containers));
		/*
		 * Returning a pod with its container ids dropped would be
		 * worse than returning nothing: pod_delete would find no ids,
		 * skip the teardown, unlink the state file, and strand the
		 * jails with no remaining record of them. Fail the load.
		 */
		if (status->containers == NULL) {
			free(status);
			free(pod);
			return (NULL);
		}
		memcpy(status->containers, cids,
		    ncids * sizeof(*status->containers));
		status->ncontainers = ncids;
	}
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

	/*
	 * A count without an array (or a negative one) would be indexed below
	 * and in pod_start; reject it rather than dereference it as root.
	 */
	if (spec->ncontainers < 0 ||
	    (spec->ncontainers > 0 && spec->containers == NULL)) {
		free(pod);
		errno = EINVAL;
		return (NULL);
	}

	/* Copy spec */
	pod->spec = calloc(1, sizeof(struct pod_spec));
	if (pod->spec == NULL) {
		free(pod);
		return (NULL);
	}
	/*
	 * Deep-copy the whole spec, not just its containers array. The old
	 * code block-copied the pod_spec and then deep-copied only
	 * ->containers, so every char * in the pod_spec AND in each
	 * container_spec (env[], volumes[], user, group, ...) stayed aliased
	 * to the caller's storage with no owner on either side. pod_spec_copy
	 * gives the pod its own strings; pod_spec_release in pod_free is its
	 * exact dual.
	 */
	if (pod_spec_copy(pod->spec, spec) != 0) {
		free(pod->spec);
		free(pod);
		return (NULL);
	}

	/* Initialize status */
	status = calloc(1, sizeof(struct pod_status));
	if (status == NULL) {
		pod_spec_release(pod->spec);
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
			pod_spec_release(pod->spec);
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

	/*
	 * Add to registry, refusing a name+namespace that is already resident.
	 * Two live pods sharing an identity share one state file: the second
	 * save overwrites the first's container ids, and deleting either
	 * unlinks the record the other needs to tear its jails down. The
	 * Identity Map the lookups rely on has to be enforced at construction
	 * too, not only when interning from disk.
	 */
	pthread_mutex_lock(&pod_registry_lock);
	for (i = 0; i < pod_registry_count; i++) {
		if (strcmp(pod_registry[i]->name, pod->name) == 0 &&
		    strcmp(pod_registry[i]->namespace, pod->namespace) == 0) {
			pthread_mutex_unlock(&pod_registry_lock);
			free(status->containers);
			free(status);
			pod_spec_release(pod->spec);
			free(pod->spec);
			free(pod);
			errno = EEXIST;
			return (NULL);
		}
	}
	if (pod_registry_insert_locked(pod) != 0) {
		pthread_mutex_unlock(&pod_registry_lock);
		free(status->containers);
		free(status);
		pod_spec_release(pod->spec);
		free(pod->spec);
		free(pod);
		return (NULL);
	}
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
	bool failed = false;

	if (pod == NULL)
		return (-1);

	/*
	 * A pod reconstructed from disk carries status but no spec (the state
	 * file records lifecycle data, not the full spec). Starting one would
	 * dereference a NULL spec, and a spec with more containers than the
	 * status array would write past its end -- both as root. Refuse
	 * instead of corrupting memory.
	 */
	if (pod->spec == NULL || pod->status == NULL) {
		errno = EINVAL;
		return (-1);
	}
	if (pod->spec->ncontainers > 0 &&
	    (pod->spec->containers == NULL ||
	    pod->status->containers == NULL ||
	    pod->status->ncontainers < pod->spec->ncontainers)) {
		errno = EINVAL;
		return (-1);
	}

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
			failed = true;
			continue;
		}

		ret = ocifbsd_start_container(container_id);
		if (ret != 0) {
			pod->status->containers[i].state = REPLICA_STATE_FAILED;
			pod->status->containers[i].exit_code = ret;
			failed = true;
			/*
			 * The container was created but never started. Record
			 * its id anyway so pod_delete can tear the jail down;
			 * dropping the id here stranded a root jail that
			 * nothing afterwards knew how to remove.
			 */
			strlcpy(pod->status->containers[i].container_id,
			    container_id,
			    sizeof(pod->status->containers[i].container_id));
		} else {
			pod->status->containers[i].state = REPLICA_STATE_RUNNING;
			strlcpy(pod->status->containers[i].container_id,
			    container_id,
			    sizeof(pod->status->containers[i].container_id));
		}

		free(container_id);
	}

	/*
	 * `failed` is sticky. `ret` alone was overwritten on every iteration,
	 * so a pod whose first container failed and whose second succeeded
	 * reported success and was marked RUNNING with a FAILED container in it.
	 */
	ret = failed ? -1 : 0;

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

	if (pod->status == NULL || pod->status->state != POD_STATE_RUNNING)
		return (0);

	/*
	 * Stop each container. Iterate the status container list (restored by
	 * pod_load_disk from the persisted ids), not spec->ncontainers — a pod
	 * reconstructed from disk has no spec, so the old spec deref crashed.
	 */
	if (pod->status->containers != NULL) {
		for (i = 0; i < pod->status->ncontainers; i++) {
			char *cid = pod->status->containers[i].container_id;
			int r;

			if (cid == NULL || cid[0] == '\0')
				continue;

			r = ocifbsd_stop_container(cid, sig);
			if (r != 0)
				ret = -1;
			else
				pod->status->containers[i].state =
				    REPLICA_STATE_TERMINATED;
		}
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
	 * Delete each backing container. Iterate the status container list
	 * (populated at start and restored by pod_load_disk from the persisted
	 * container ids), so a pod reconstructed from disk still tears its jails
	 * down instead of leaking them.
	 */
	if (pod->status != NULL && pod->status->containers != NULL) {
		for (i = 0; i < pod->status->ncontainers; i++) {
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
			return (pod_registry_intern(disk));
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

	/*
	 * The namespace is interpolated into a directory path that is opened
	 * as root below. The CLI does not validate it, so `pod list -N
	 * ../../..` walked out of the state directory. Same check the state
	 * path builder applies.
	 */
	if (!orch_name_is_valid(ns)) {
		errno = EINVAL;
		return (NULL);
	}
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
				/*
				 * Intern before publishing, so every element of
				 * the returned array is a borrow owned by the
				 * registry -- the same lifetime rule as a
				 * registry hit. The caller frees the ARRAY only.
				 */
				lp = pod_registry_intern(lp);
				if (lp == NULL)
					continue;
				if (n >= alloc) {
					struct pod **grown = realloc(result,
					    alloc * 2 * sizeof(struct pod *));
					if (grown == NULL)
						break;
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
		pod_spec_release(pod->spec);
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
	/*
	 * A pod loaded from disk (pod_load_disk) has spec == NULL and no live
	 * status array; dereferencing them here crashed the root CLI on
	 * `pod logs`/status of a persisted pod. Return whatever status exists.
	 */
	if (pod->spec == NULL || pod->status == NULL ||
	    pod->status->containers == NULL)
		return (pod->status);

	/* Refresh status from containers */
	for (int i = 0; i < pod->spec->ncontainers; i++) {
		char *cid = pod->status->containers[i].container_id;
		if (cid == NULL || cid[0] == '\0')
			continue;

		/*
		 * Query container state from ocifbsd. On failure the callee
		 * leaves *state and *exit_code untouched, so the switch below
		 * used to read uninitialized stack -- a value that happened to
		 * match CONTAINER_STATE_STOPPED would record a garbage exit
		 * code as fact. Initialize, and skip the update on error
		 * rather than inventing a transition.
		 */
		container_state_t state = CONTAINER_STATE_UNKNOWN;
		int exit_code = 0;

		if (ocifbsd_get_container_state(cid, &state, &exit_code) != 0)
			continue;

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
	struct pod_spec *newspec;
	struct container_status *newstatus = NULL;
	int i;

	if (pod == NULL || new_spec == NULL)
		return (-1);

	/*
	 * Build the replacement spec and the matching status array COMPLETELY
	 * before touching the pod. The old order tore the pod down first, so
	 * any allocation failure left it stopped with spec == NULL -- an
	 * object no later call could use or repair. Prepare, then swap.
	 *
	 * The status array must be rebuilt too: it is indexed by container
	 * position in pod_start, so growing the container count while reusing
	 * the old, shorter array wrote past its end.
	 */
	newspec = calloc(1, sizeof(struct pod_spec));
	if (newspec == NULL)
		return (-1);
	if (pod_spec_copy(newspec, new_spec) != 0) {
		free(newspec);
		return (-1);
	}
	if (newspec->ncontainers > 0) {
		newstatus = calloc((size_t)newspec->ncontainers,
		    sizeof(*newstatus));
		if (newstatus == NULL) {
			pod_spec_release(newspec);
			free(newspec);
			return (-1);
		}
		for (i = 0; i < newspec->ncontainers; i++) {
			strlcpy(newstatus[i].name, newspec->containers[i].name,
			    sizeof(newstatus[i].name));
			strlcpy(newstatus[i].image,
			    newspec->containers[i].image,
			    sizeof(newstatus[i].image));
			newstatus[i].state = REPLICA_STATE_PENDING;
		}
	}

	/* Stop the running pod only once the replacement is ready. */
	pod_stop(pod, SIGTERM);

	/*
	 * Destroy the OLD containers before their ids are discarded. The
	 * status array is the only record of them, so replacing it without
	 * this leaves the previous jails running as root with nothing left
	 * that names them -- pod_start then creates a second set alongside.
	 */
	if (pod->status != NULL && pod->status->containers != NULL) {
		for (i = 0; i < pod->status->ncontainers; i++) {
			const char *cid =
			    pod->status->containers[i].container_id;

			if (cid[0] != '\0')
				(void)ocifbsd_delete_container(cid, true);
		}
	}

	if (pod->spec != NULL) {
		pod_spec_release(pod->spec);
		free(pod->spec);
	}
	pod->spec = newspec;
	if (pod->status != NULL) {
		free(pod->status->containers);
		pod->status->containers = newstatus;
		pod->status->ncontainers = newspec->ncontainers;
		pod->status->state = POD_STATE_PENDING;
	} else {
		free(newstatus);
	}

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
	/* A disk-loaded pod has no spec/status arrays; do not deref them. */
	if (pod->spec == NULL || pod->status == NULL) {
		errno = EINVAL;
		return (-1);
	}

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
