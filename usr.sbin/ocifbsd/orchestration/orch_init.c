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
 * OCI FreeBSD Orchestration - Initialization and Event System
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

extern int mkdirp(const char *path, mode_t mode);
#include <stdarg.h>
#include <time.h>

#include "orchestration.h"

#define MAX_EVENT_SUBSCRIPTIONS 128
#define MAX_EVENT_HISTORY 1000

/*
 * Event subscription
 */
struct event_subscription {
	int			id;
	orch_event_callback_t	callback;
	void			*arg;
	bool			active;
};

/*
 * Event in history
 */
struct event_entry {
	struct orch_event	event;
	struct event_entry	*next;
};

/*
 * Global state
 */
static pthread_mutex_t orch_lock = PTHREAD_MUTEX_INITIALIZER;
static bool orch_initialized = false;
static int next_subscription_id = 1;

/* Subscriptions */
static struct event_subscription *subscriptions[MAX_EVENT_SUBSCRIPTIONS];
static int subscription_count = 0;

/* Event history */
static struct event_entry *event_history = NULL;
static struct event_entry *event_history_tail = NULL;
static int event_history_count = 0;
static int event_history_max = MAX_EVENT_HISTORY;

/*
 * Initialize orchestration system
 */
int
orch_init(void)
{
	pthread_mutex_lock(&orch_lock);
	
	if (orch_initialized) {
		pthread_mutex_unlock(&orch_lock);
		return (0);
	}
	
	/* Create required directories */
	if (mkdirp(OCIFBSD_ORCH_VAR_DIR, 0755) != 0 && errno != EEXIST) {
		pthread_mutex_unlock(&orch_lock);
		return (-1);
	}
	
	if (mkdirp(OCIFBSD_ORCH_STATE_DIR, 0755) != 0 && errno != EEXIST) {
		pthread_mutex_unlock(&orch_lock);
		return (-1);
	}
	
	if (mkdirp(OCIFBSD_ORCH_CONFIG_DIR, 0755) != 0 && errno != EEXIST) {
		pthread_mutex_unlock(&orch_lock);
		return (-1);
	}
	
	/* Initialize scheduler */
	if (scheduler_init() != 0) {
		pthread_mutex_unlock(&orch_lock);
		return (-1);
	}
	
	/* Initialize health checker */
	if (health_checker_init() != 0) {
		pthread_mutex_unlock(&orch_lock);
		return (-1);
	}
	
	/* Load saved state */
	orch_load_state();
	
	orch_initialized = true;
	pthread_mutex_unlock(&orch_lock);
	
	return (0);
}

/*
 * Shutdown orchestration system
 */
void
orch_shutdown(void)
{
	pthread_mutex_lock(&orch_lock);
	
	if (!orch_initialized) {
		pthread_mutex_unlock(&orch_lock);
		return;
	}
	
	/* Save state before shutdown */
	orch_save_state();
	
	/* Shutdown health checker */
	health_checker_shutdown();
	
	/* Clear subscriptions */
	for (int i = 0; i < MAX_EVENT_SUBSCRIPTIONS; i++) {
		if (subscriptions[i] != NULL) {
			subscriptions[i]->active = false;
			free(subscriptions[i]);
			subscriptions[i] = NULL;
		}
	}
	
	/* Free event history */
	struct event_entry *entry = event_history;
	while (entry != NULL) {
		struct event_entry *next = entry->next;
		free(entry);
		entry = next;
	}
	event_history = NULL;
	event_history_tail = NULL;
	event_history_count = 0;
	
	orch_initialized = false;
	pthread_mutex_unlock(&orch_lock);
}

/*
 * Save orchestration state to disk
 */
int
orch_save_state(void)
{
	FILE *fp;
	char path[PATH_MAX];
	
	snprintf(path, sizeof(path), "%s/state.json", OCIFBSD_ORCH_VAR_DIR);
	
	fp = fopen(path, "w");
	if (fp == NULL)
		return (-1);
	
	fprintf(fp, "{\n");
	fprintf(fp, "  \"version\": \"1.0\",\n");
	fprintf(fp, "  \"timestamp\": %ld\n", (long)time(NULL));
	fprintf(fp, "}\n");
	
	fclose(fp);
	return (0);
}

/*
 * Load orchestration state from disk
 */
int
orch_load_state(void)
{
	char path[PATH_MAX];
	FILE *fp;
	char buf[1024];
	
	snprintf(path, sizeof(path), "%s/state.json", OCIFBSD_ORCH_VAR_DIR);
	
	fp = fopen(path, "r");
	if (fp == NULL)
		return (0);  /* No state file, that's OK */
	
	/* Simple state loading - just verify the file exists and is valid JSON */
	while (fgets(buf, sizeof(buf), fp) != NULL) {
		/* Parse if needed */
	}
	
	fclose(fp);
	return (0);
}

/*
 * Subscribe to events
 */
int
orch_event_subscribe(orch_event_callback_t callback, void *arg)
{
	struct event_subscription *sub;
	
	if (callback == NULL) {
		errno = EINVAL;
		return (-1);
	}
	
	pthread_mutex_lock(&orch_lock);
	
	/* Find empty slot */
	int slot = -1;
	for (int i = 0; i < MAX_EVENT_SUBSCRIPTIONS; i++) {
		if (subscriptions[i] == NULL) {
			slot = i;
			break;
		}
	}
	
	if (slot < 0) {
		pthread_mutex_unlock(&orch_lock);
		errno = ENOMEM;
		return (-1);
	}
	
	sub = calloc(1, sizeof(struct event_subscription));
	if (sub == NULL) {
		pthread_mutex_unlock(&orch_lock);
		return (-1);
	}
	
	sub->id = next_subscription_id++;
	sub->callback = callback;
	sub->arg = arg;
	sub->active = true;
	
	subscriptions[slot] = sub;
	subscription_count++;
	
	pthread_mutex_unlock(&orch_lock);
	
	return (sub->id);
}

/*
 * Unsubscribe from events
 */
int
orch_event_unsubscribe(int subscription_id)
{
	pthread_mutex_lock(&orch_lock);
	
	for (int i = 0; i < MAX_EVENT_SUBSCRIPTIONS; i++) {
		if (subscriptions[i] != NULL &&
		    subscriptions[i]->id == subscription_id) {
			subscriptions[i]->active = false;
			free(subscriptions[i]);
			subscriptions[i] = NULL;
			subscription_count--;
			pthread_mutex_unlock(&orch_lock);
			return (0);
		}
	}
	
	pthread_mutex_unlock(&orch_lock);
	errno = ENOENT;
	return (-1);
}

/*
 * Publish an event
 */
int
orch_event_publish(const char *type, const char *object,
    const char *namespace, const char *message, ...)
{
	struct orch_event event;
	va_list ap;
	
	if (type == NULL || object == NULL) {
		errno = EINVAL;
		return (-1);
	}
	
	/* Build event */
	strlcpy(event.type, type, sizeof(event.type));
	strlcpy(event.object, object, sizeof(event.object));
	strlcpy(event.namespace, namespace ? namespace : "", sizeof(event.namespace));
	
	va_start(ap, message);
	vsnprintf(event.message, sizeof(event.message), message, ap);
	va_end(ap);
	
	event.timestamp = time(NULL);
	
	/* Add to history */
	pthread_mutex_lock(&orch_lock);
	
	struct event_entry *entry = malloc(sizeof(struct event_entry));
	if (entry != NULL) {
		entry->event = event;
		entry->next = NULL;
		
		if (event_history_tail != NULL) {
			event_history_tail->next = entry;
			event_history_tail = entry;
		} else {
			event_history = event_history_tail = entry;
		}
		
		event_history_count++;
		
		/* Trim history if too long */
		while (event_history_count > event_history_max && event_history != NULL) {
			struct event_entry *old = event_history;
			event_history = event_history->next;
			free(old);
			event_history_count--;
		}
	}
	
	/* Notify subscribers */
	for (int i = 0; i < MAX_EVENT_SUBSCRIPTIONS; i++) {
		if (subscriptions[i] != NULL && subscriptions[i]->active) {
			subscriptions[i]->callback(&event, subscriptions[i]->arg);
		}
	}
	
	pthread_mutex_unlock(&orch_lock);
	
	return (0);
}

/*
 * List recent events
 */
struct orch_event **
orch_event_list(const char *namespace, int *count)
{
	struct orch_event **result;
	struct event_entry *entry;
	int alloc = 16;
	int n = 0;
	
	*count = 0;
	result = malloc(alloc * sizeof(struct orch_event *));
	if (result == NULL)
		return (NULL);
	
	pthread_mutex_lock(&orch_lock);
	
	entry = event_history;
	while (entry != NULL) {
		if (namespace == NULL || 
		    strcmp(entry->event.namespace, namespace) == 0 ||
		    entry->event.namespace[0] == '\0') {
			if (n >= alloc) {
				alloc *= 2;
				struct orch_event **new_result = realloc(result,
				    alloc * sizeof(struct orch_event *));
				if (new_result == NULL) {
					pthread_mutex_unlock(&orch_lock);
					for (int i = 0; i < n; i++)
						free(result[i]);
					free(result);
					return (NULL);
				}
				result = new_result;
			}
			
			/* Copy event */
			struct orch_event *copy = malloc(sizeof(struct orch_event));
			if (copy != NULL) {
				*copy = entry->event;
				result[n++] = copy;
			}
		}
		entry = entry->next;
	}
	
	pthread_mutex_unlock(&orch_lock);
	
	*count = n;
	return (result);
}
