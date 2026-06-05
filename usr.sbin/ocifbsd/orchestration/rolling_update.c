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
	
	snprintf(path, sizeof(path), "%s/rolling-updates/%s/%s.json",
	    OCIFBSD_ORCH_VAR_DIR, info->namespace, info->service);
	
	if (mkdirp(path, 0755) != 0 && errno != EEXIST)
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

	if (service == NULL || new_spec == NULL)
		return (-1);
	
	pthread_mutex_lock(&rolling_lock);
	
	/* Find existing update for this service */
	for (int i = 0; i < MAX_ROLLING_UPDATES; i++) {
		if (rolling_updates[i] != NULL &&
		    strcmp(rolling_updates[i]->service, service->name) == 0 &&
		    strcmp(rolling_updates[i]->namespace, service->namespace) == 0) {
			/* Update already in progress */
			pthread_mutex_unlock(&rolling_lock);
			errno = EALREADY;
			return (-1);
		}
	}
	
	/* Find empty slot */
	info = NULL;
	for (int i = 0; i < MAX_ROLLING_UPDATES; i++) {
		if (rolling_updates[i] == NULL) {
			info = calloc(1, sizeof(struct rolling_update_info));
			if (info == NULL) {
				pthread_mutex_unlock(&rolling_lock);
				return (-1);
			}
			rolling_updates[i] = info;
			rolling_update_count++;
			break;
		}
	}
	
	if (info == NULL) {
		pthread_mutex_unlock(&rolling_lock);
		errno = ENOMEM;
		return (-1);
	}
	
	pthread_mutex_init(&info->lock, NULL);
	
	/* Initialize rolling update info */
	strlcpy(info->service, service->name, sizeof(info->service));
	strlcpy(info->namespace, service->namespace, sizeof(info->namespace));
	
	/* Save current spec for rollback */
	info->has_previous = true;
	memcpy(&info->previous_spec, service->spec, sizeof(struct service_spec));
	
	/* Copy specs */
	memcpy(&info->old_spec, service->spec, sizeof(struct service_spec));
	memcpy(&info->new_spec, new_spec, sizeof(struct service_spec));
	
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
	
	/* Process each replica */
	while (info->current_replica < info->state.total_replicas) {
		if (info->paused) {
			sleep(1);
			continue;
		}
		
		snprintf(old_pod_name, sizeof(old_pod_name), "%s-replica-%d",
		    info->service, info->current_replica);
		snprintf(new_pod_name, sizeof(new_pod_name), "%s-replica-%d-new",
		    info->service, info->current_replica);
		
		/* Get old pod */
		old_pod = pod_get(old_pod_name, info->namespace);
		
		/* Create new pod with updated spec */
		memset(&pod_spec, 0, sizeof(pod_spec));
		strlcpy(pod_spec.name, new_pod_name, sizeof(pod_spec.name));
		strlcpy(pod_spec.namespace, info->namespace, sizeof(pod_spec.namespace));
		pod_spec.ncontainers = 1;
		strlcpy(pod_spec.containers[0].name, target_spec->name,
		    sizeof(pod_spec.containers[0].name));
		strlcpy(pod_spec.containers[0].image, target_spec->image,
		    sizeof(pod_spec.containers[0].image));
		
		new_pod = pod_create(&pod_spec);
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
		
		/* Rename new pod to old name */
		/* In production, this would involve updating the pod's name in state */
		
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
	
	/* Update service spec */
	service_update(service, &info->new_spec);
	
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
	pthread_mutex_lock(&rolling_lock);
	
	for (int i = 0; i < MAX_ROLLING_UPDATES; i++) {
		if (rolling_updates[i] != NULL &&
		    strcmp(rolling_updates[i]->service, service_name) == 0 &&
		    strcmp(rolling_updates[i]->namespace, namespace) == 0) {
			struct rolling_update_state *state = &rolling_updates[i]->state;
			pthread_mutex_unlock(&rolling_lock);
			return (state);
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
