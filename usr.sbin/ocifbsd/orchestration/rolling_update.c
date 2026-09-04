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
 * Rolling update implementation - zero-downtime deployments
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <libutil.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "orchestration.h"

extern int mkdirp(const char *path, mode_t mode);

#define MAX_ROLLING_UPDATES 256

/*
 * Rolling update state
 */
struct rolling_update_info {
	char			service[256];
	char			namespace[128];
	struct service_spec	old_spec;
	struct service_spec	new_spec;
	struct rolling_update_state state;
	rolling_strategy_t	strategy;
	bool			paused;
	bool			active;
	pthread_mutex_t		lock;

	/* For blue-green deployments */
	char			blue_name[256];
	char			green_name[256];
	int			active_set;  /* 0 = blue, 1 = green */

	/* Update progress */
	int			current_replica;
	time_t			last_update;

	/* Rollback support */
	struct service_spec	previous_spec;
	bool			has_previous;
};

/*
 * Global state
 */
static struct rolling_update_info *rolling_updates[MAX_ROLLING_UPDATES];

static int rolling_update_count = 0;
static pthread_mutex_t rolling_lock = PTHREAD_MUTEX_INITIALIZER;

static int rolling_update_progress(struct rolling_update_info *info);

/*
 * Save rolling update state to disk
 */
static int
save_rolling_update_state(struct rolling_update_info *info)
{
	FILE *fp;
	char path[PATH_MAX];
	char dir[PATH_MAX];

	/* Names become path components — reject traversal (see pod.c). */
	if (!orch_name_is_valid(info->service) ||
	    !orch_name_is_valid(info->namespace))
		return (-1);

	snprintf(path, sizeof(path), "%s/rolling-updates/%s/%s.json",
	    OCIFBSD_ORCH_VAR_DIR, info->namespace, info->service);

	/*
	 * mkdirp the PARENT directory only. mkdirp on the full path would
	 * create the "<service>.json" node itself as a directory, after which
	 * fopen(path,"w") fails and rolling-update state is never persisted
	 * (the same bug pod.c/stack.c already fixed).
	 */
	snprintf(dir, sizeof(dir), "%s/rolling-updates/%s",
	    OCIFBSD_ORCH_VAR_DIR, info->namespace);
	if (mkdirp(dir, 0755) != 0 && errno != EEXIST)
		return (-1);

	fp = fopen(path, "w");
	if (fp == NULL)
		return (-1);

	fprintf(fp, "{\n");
	fprintf(fp, "  \"service\": \"%s\",\n", info->service);
	fprintf(fp, "  \"namespace\": \"%s\",\n", info->namespace);
	fprintf(fp, "  \"strategy\": \"%s\",\n",
	    info->strategy == ROLLING_STRATEGY_ROLLING ? "RollingUpdate" :
	    info->strategy == ROLLING_STRATEGY_BLUE_GREEN ? "BlueGreen" :
	    "Recreate");
	fprintf(fp, "  \"status\": \"%s\",\n", info->state.status);
	fprintf(fp, "  \"updated_replicas\": %d,\n", info->state.updated_replicas);
	fprintf(fp, "  \"total_replicas\": %d,\n", info->state.total_replicas);
	fprintf(fp, "  \"paused\": %s\n", info->paused ? "true" : "false");
	fprintf(fp, "}\n");

	fclose(fp);
	return (0);
}

/*
 * Initialize rolling update
 */
int
rolling_update_init(struct service *service, struct service_spec *new_spec)
{
	struct rolling_update_info *info;
	int reuse;

	if (service == NULL || new_spec == NULL)
		return (-1);

	pthread_mutex_lock(&rolling_lock);

	/*
	 * Reject only an update that is still RUNNING. The check used to match
	 * on name alone, so a completed -- or merely failed -- record made
	 * every later update of that service fail EALREADY for the life of the
	 * process, with 255 slots free. A finished record for this service is
	 * not a conflict; it is the slot we should reuse, which also keeps one
	 * service from ever owning two records.
	 */
	reuse = -1;
	for (int i = 0; i < MAX_ROLLING_UPDATES; i++) {
		if (rolling_updates[i] == NULL ||
		    strcmp(rolling_updates[i]->service, service->name) != 0 ||
		    strcmp(rolling_updates[i]->namespace,
		    service->namespace) != 0)
			continue;
		if (rolling_updates[i]->active) {
			pthread_mutex_unlock(&rolling_lock);
			errno = EALREADY;
			return (-1);
		}
		reuse = i;
		break;
	}

	/*
	 * Claim a slot. A finished update's record is deliberately RETAINED so
	 * rolling_update_get_status() can still report on it (it hands out an
	 * interior pointer into this record), which is why nothing is freed at
	 * completion. Retaining forever, though, meant the table filled with
	 * dead records and every later update failed -- and each record's three
	 * service_spec copies leaked with it.
	 *
	 * So: prefer a free slot, and if there is none, reclaim the OLDEST
	 * COMPLETED record. Only when every slot holds an update still running
	 * is this a genuine "too many concurrent updates" failure.
	 */
	info = NULL;
	{
		int slot = -1, oldest = reuse;
		time_t oldest_time = 0;

		if (reuse >= 0)
			goto reclaim;
		for (int i = 0; i < MAX_ROLLING_UPDATES; i++) {
			if (rolling_updates[i] == NULL) {
				slot = i;
				break;
			}
			if (rolling_updates[i]->active)
				continue;
			if (oldest < 0 ||
			    rolling_updates[i]->state.completed < oldest_time) {
				oldest = i;
				oldest_time = rolling_updates[i]->state.completed;
			}
		}
reclaim:
		if (slot >= 0) {
			info = calloc(1, sizeof(struct rolling_update_info));
			if (info == NULL) {
				pthread_mutex_unlock(&rolling_lock);
				return (-1);
			}
			rolling_updates[slot] = info;
			rolling_update_count++;
			pthread_mutex_init(&info->lock, NULL);
		} else if (oldest >= 0) {
			/*
			 * Reuse the oldest completed record IN PLACE rather
			 * than freeing it. rolling_update_get_status() hands
			 * out an interior pointer to ->state, so freeing a
			 * reclaimed record would turn any status pointer taken
			 * earlier into a dangling one. Recycling the same
			 * allocation keeps every such pointer aimed at valid,
			 * pointer-free memory: a stale reader sees the new
			 * update's status instead of the old one's, which is
			 * wrong data but never undefined behaviour.
			 *
			 * The spec copies ARE released here -- they are the
			 * part that actually grows -- and re-established
			 * below.
			 */
			info = rolling_updates[oldest];
			service_spec_release(&info->previous_spec);
			service_spec_release(&info->old_spec);
			service_spec_release(&info->new_spec);
			{
				pthread_mutex_t keep = info->lock;

				memset(info, 0, sizeof(*info));
				info->lock = keep;
			}
		} else {
			pthread_mutex_unlock(&rolling_lock);
			errno = EBUSY;	/* all slots are live updates */
			return (-1);
		}
	}

	/* Initialize rolling update info */
	strlcpy(info->service, service->name, sizeof(info->service));
	strlcpy(info->namespace, service->namespace, sizeof(info->namespace));

	/*
	 * Take PRIVATE deep copies of all three specs. The block copies these
	 * replace aliased the caller's and the service's string pointers, so
	 * the record outlived the memory it pointed at -- new_spec in
	 * particular is routinely a caller stack local, and its failure_policy
	 * was read later, from a dead frame, when a batch failed.
	 */
	info->has_previous = true;
	if (service_spec_copy(&info->previous_spec, service->spec) != 0 ||
	    service_spec_copy(&info->old_spec, service->spec) != 0 ||
	    service_spec_copy(&info->new_spec, new_spec) != 0) {
		/*
		 * Release whatever copies succeeded but LEAVE the record in
		 * the table: a status pointer may already point into it, and
		 * the slot is reusable as an inactive record.
		 */
		service_spec_release(&info->previous_spec);
		service_spec_release(&info->old_spec);
		service_spec_release(&info->new_spec);
		info->active = false;
		/*
		 * Clear the identity so this dead record cannot be mistaken
		 * for an update of this service, and so it sorts first for
		 * reuse (completed == 0 makes it the oldest).
		 */
		info->service[0] = '\0';
		info->namespace[0] = '\0';
		info->state.completed = 0;
		pthread_mutex_unlock(&rolling_lock);
		errno = ENOMEM;
		return (-1);
	}

	/* Initialize state */
	info->state.total_replicas = service->nreplicas;
	info->state.updated_replicas = 0;
	info->state.available_replicas = service->status->available_replicas;
	info->state.ready_replicas = service->status->ready_replicas;
	info->state.started = time(NULL);
	strlcpy(info->state.status, "running", sizeof(info->state.status));

	/* Determine strategy */
	info->strategy = service->spec->update_config.strategy;
	if (info->strategy == 0)
		info->strategy = ROLLING_STRATEGY_ROLLING;

	/* Set update parameters */
	info->state.current_surge = service->spec->update_config.max_surge;
	info->state.current_unavailable = service->spec->update_config.max_unavailable;

	info->active = true;
	info->paused = false;
	info->current_replica = 0;
	info->last_update = time(NULL);

	pthread_mutex_unlock(&rolling_lock);

	/* Publish event */
	orch_event_publish("Normal", "RollingUpdateStarted",
	    info->namespace,
	    "Rolling update started for service %s", info->service);

	/* Save state */
	save_rolling_update_state(info);

	/* Start the rolling update */
	return (rolling_update_progress(info));
}

/*
 * Progress rolling update - update one replica at a time
 */
static int
rolling_update_progress(struct rolling_update_info *info)
{
	struct service *service;
	struct service_spec *target_spec;
	char new_pod_name[256];
	char old_pod_name[256];
	struct pod *old_pod, *new_pod;
	struct pod_spec pod_spec;

	service = service_get(info->service, info->namespace);
	if (service == NULL)
		return (-1);

	target_spec = &info->new_spec;

	/*
	 * Bound the paused wait. rolling_update_progress runs synchronously from
	 * rolling_update_init, so nothing in THIS process can clear info->paused
	 * (a pause set elsewhere lives in another process's memory); the old
	 * unbounded `while (paused) sleep(1)` was an infinite hang. Give up after
	 * a cap rather than spin forever.
	 */
	const int max_paused_secs = 300;
	int paused_secs = 0;

	/* Process each replica */
	while (info->current_replica < info->state.total_replicas) {
		if (info->paused) {
			if (paused_secs >= max_paused_secs) {
				fprintf(stderr, "rolling update: still paused after "
				    "%ds; aborting (cannot resume within this "
				    "process)\n", paused_secs);
				return (-1);
			}
			sleep(1);
			paused_secs++;
			continue;
		}
		paused_secs = 0;

		/*
		 * The current (old) pod is whatever this replica points at now
		 * — which, after a previous rolling update, may already carry a
		 * "-vN" suffix — not always "<svc>-replica-<n>". Fall back to the
		 * canonical name when the replica has none recorded. The new pod
		 * gets a fresh generation suffix so it never collides with the
		 * pod being retired.
		 */
		if (info->current_replica < service->nreplicas &&
		    service->replicas != NULL &&
		    service->replicas[info->current_replica].pod_name[0] != '\0')
			strlcpy(old_pod_name,
			    service->replicas[info->current_replica].pod_name,
			    sizeof(old_pod_name));
		else
			snprintf(old_pod_name, sizeof(old_pod_name),
			    "%s-replica-%d", info->service, info->current_replica);
		snprintf(new_pod_name, sizeof(new_pod_name),
		    "%s-replica-%d-v%d", info->service, info->current_replica,
		    (int)info->state.started + info->current_replica);

		/* Get old pod */
		old_pod = pod_get(old_pod_name, info->namespace);

		/* Create new pod with updated spec */
		memset(&pod_spec, 0, sizeof(pod_spec));
		strlcpy(pod_spec.name, new_pod_name, sizeof(pod_spec.name));
		strlcpy(pod_spec.namespace, info->namespace, sizeof(pod_spec.namespace));
		/* containers is a pointer, NULL after memset — allocate it. */
		pod_spec.containers = calloc(1, sizeof(*pod_spec.containers));
		if (pod_spec.containers == NULL)
			return (-1);
		pod_spec.ncontainers = 1;
		strlcpy(pod_spec.containers[0].name, target_spec->name,
		    sizeof(pod_spec.containers[0].name));
		strlcpy(pod_spec.containers[0].image, target_spec->image,
		    sizeof(pod_spec.containers[0].image));

		new_pod = pod_create(&pod_spec);
		free(pod_spec.containers);
		pod_spec.containers = NULL;
		if (new_pod == NULL) {
			/* Handle failure */
			if (target_spec->update_config.failure_policy != NULL &&
			    strcmp(target_spec->update_config.failure_policy, "rollback") == 0) {
				rolling_update_rollback(&info->state);
				return (-1);
			}
			info->current_replica++;
			continue;
		}

		/* Start new pod */
		if (pod_start(new_pod) != 0) {
			pod_delete(new_pod);
			info->current_replica++;
			continue;
		}

		/* Wait for new pod to be ready */
		sleep(2);  /* In production, wait for health check */

		/* Stop old pod */
		if (old_pod != NULL) {
			pod_stop(old_pod, SIGTERM);
			sleep(5);  /* Graceful shutdown */
			pod_delete(old_pod);
		}

		/*
		 * Point the replica at the new pod so the service tracks it —
		 * without this the service kept referencing the deleted old pod
		 * and the new pod leaked on `service delete`.
		 */
		if (info->current_replica < service->nreplicas &&
		    service->replicas != NULL)
			strlcpy(service->replicas[info->current_replica].pod_name,
			    new_pod_name,
			    sizeof(service->replicas[info->current_replica].pod_name));

		/* Update state */
		info->state.updated_replicas++;
		info->current_replica++;
		info->last_update = time(NULL);

		orch_event_publish("Normal", "RollingUpdateProgress",
		    info->namespace,
		    "Rolling update: %d/%d replicas updated",
		    info->state.updated_replicas, info->state.total_replicas);

		save_rolling_update_state(info);
	}

	/* Update complete */
	strlcpy(info->state.status, "completed", sizeof(info->state.status));
	info->state.completed = time(NULL);
	info->active = false;

	orch_event_publish("Normal", "RollingUpdateComplete",
	    info->namespace,
	    "Rolling update completed for service %s", info->service);

	save_rolling_update_state(info);

	/*
	 * Commit the new image onto the service and persist the remapped replica
	 * pod names. NOT via service_update() — that would re-enter
	 * rolling_update_init(), which sees this still-active update and bails,
	 * leaving the service pointing at the retired pods.
	 */
	info->active = false;
	if (service->spec != NULL)
		strlcpy(service->spec->image, info->new_spec.image,
		    sizeof(service->spec->image));
	save_service_state(service);

	return (0);
}

/*
 * Pause rolling update
 */
int
rolling_update_pause(struct rolling_update_state *state)
{
	pthread_mutex_lock(&rolling_lock);

	for (int i = 0; i < MAX_ROLLING_UPDATES; i++) {
		if (rolling_updates[i] != NULL &&
		    strcmp(rolling_updates[i]->service, state->service) == 0 &&
		    strcmp(rolling_updates[i]->namespace, state->namespace) == 0) {
			rolling_updates[i]->paused = true;
			strlcpy(rolling_updates[i]->state.status, "paused",
			    sizeof(rolling_updates[i]->state.status));
			save_rolling_update_state(rolling_updates[i]);
			pthread_mutex_unlock(&rolling_lock);

			orch_event_publish("Normal", "RollingUpdatePaused",
			    state->namespace,
			    "Rolling update paused for service %s", state->service);
			return (0);
		}
	}

	pthread_mutex_unlock(&rolling_lock);
	errno = ENOENT;
	return (-1);
}

/*
 * Resume rolling update
 */
int
rolling_update_resume(struct rolling_update_state *state)
{
	pthread_mutex_lock(&rolling_lock);

	for (int i = 0; i < MAX_ROLLING_UPDATES; i++) {
		if (rolling_updates[i] != NULL &&
		    strcmp(rolling_updates[i]->service, state->service) == 0 &&
		    strcmp(rolling_updates[i]->namespace, state->namespace) == 0) {
			rolling_updates[i]->paused = false;
			strlcpy(rolling_updates[i]->state.status, "running",
			    sizeof(rolling_updates[i]->state.status));
			save_rolling_update_state(rolling_updates[i]);
			pthread_mutex_unlock(&rolling_lock);

			orch_event_publish("Normal", "RollingUpdateResumed",
			    state->namespace,
			    "Rolling update resumed for service %s", state->service);
			return (0);
		}
	}

	pthread_mutex_unlock(&rolling_lock);
	errno = ENOENT;
	return (-1);
}

/*
 * Rollback rolling update
 */
int
rolling_update_rollback(struct rolling_update_state *state)
{
	struct service *service;

	pthread_mutex_lock(&rolling_lock);

	struct rolling_update_info *info = NULL;
	for (int i = 0; i < MAX_ROLLING_UPDATES; i++) {
		if (rolling_updates[i] != NULL &&
		    strcmp(rolling_updates[i]->service, state->service) == 0 &&
		    strcmp(rolling_updates[i]->namespace, state->namespace) == 0) {
			info = rolling_updates[i];
			break;
		}
	}

	if (info == NULL) {
		pthread_mutex_unlock(&rolling_lock);
		errno = ENOENT;
		return (-1);
	}

	if (!info->has_previous) {
		pthread_mutex_unlock(&rolling_lock);
		errno = ENOENT;
		return (-1);
	}

	/* Stop rolling update */
	info->active = false;
	info->paused = false;
	strlcpy(info->state.status, "rolling_back", sizeof(info->state.status));

	pthread_mutex_unlock(&rolling_lock);

	/* Get service and rollback */
	service = service_get(state->service, state->namespace);
	if (service != NULL) {
		service_update(service, &info->previous_spec);
	}

	pthread_mutex_lock(&rolling_lock);
	strlcpy(info->state.status, "rolled_back", sizeof(info->state.status));
	info->active = false;
	save_rolling_update_state(info);
	pthread_mutex_unlock(&rolling_lock);

	orch_event_publish("Warning", "RollingUpdateRolledBack",
	    state->namespace,
	    "Rolling update rolled back for service %s", state->service);

	return (0);
}

/*
 * Get rolling update status
 */
struct rolling_update_state *
rolling_update_get_status(const char *service_name, const char *namespace)
{
	struct rolling_update_state *copy;

	pthread_mutex_lock(&rolling_lock);

	for (int i = 0; i < MAX_ROLLING_UPDATES; i++) {
		if (rolling_updates[i] != NULL &&
		    strcmp(rolling_updates[i]->service, service_name) == 0 &&
		    strcmp(rolling_updates[i]->namespace, namespace) == 0) {
			/*
			 * Return an OWNED snapshot, not a pointer into the
			 * table. An interior pointer stayed valid only as long
			 * as the slot was not recycled -- and because the
			 * mutators re-look-up by state->service, a pointer
			 * held across a recycle would silently name a
			 * DIFFERENT service and pause or roll back that one
			 * instead. Copying is cheap (the struct owns no
			 * pointers) and makes the hazard impossible rather
			 * than unlikely.
			 */
			copy = malloc(sizeof(*copy));
			if (copy == NULL) {
				pthread_mutex_unlock(&rolling_lock);
				errno = ENOMEM;
				return (NULL);
			}
			*copy = rolling_updates[i]->state;
			pthread_mutex_unlock(&rolling_lock);
			return (copy);
		}
	}

	pthread_mutex_unlock(&rolling_lock);
	errno = ENOENT;
	return (NULL);
}

/*
 * Complete rolling update (for manual completion)
 */
int
rolling_update_complete(struct rolling_update_state *state)
{
	pthread_mutex_lock(&rolling_lock);

	for (int i = 0; i < MAX_ROLLING_UPDATES; i++) {
		if (rolling_updates[i] != NULL &&
		    strcmp(rolling_updates[i]->service, state->service) == 0 &&
		    strcmp(rolling_updates[i]->namespace, state->namespace) == 0) {
			rolling_updates[i]->active = false;
			strlcpy(rolling_updates[i]->state.status, "completed",
			    sizeof(rolling_updates[i]->state.status));
			rolling_updates[i]->state.completed = time(NULL);
			rolling_updates[i]->state.updated_replicas =
			    rolling_updates[i]->state.total_replicas;
			save_rolling_update_state(rolling_updates[i]);

			orch_event_publish("Normal", "RollingUpdateComplete",
			    state->namespace,
			    "Rolling update completed for service %s", state->service);

			pthread_mutex_unlock(&rolling_lock);
			return (0);
		}
	}

	pthread_mutex_unlock(&rolling_lock);
	errno = ENOENT;
	return (-1);
}
