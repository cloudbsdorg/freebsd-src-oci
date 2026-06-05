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
 * Stack management implementation
 */

#include <sys/param.h>
#include <sys/stat.h>
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

#include "orchestration.h"
#include "../include/ocifbsd.h"

extern int mkdirp(const char *path, mode_t mode);

/*
 * Global stack registry
 */
static struct stack **stack_registry = NULL;
static int stack_registry_size = 0;
static int stack_registry_count = 0;
static pthread_mutex_t stack_registry_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Global service registry
 */
static struct service **service_registry = NULL;
static int service_registry_size = 0;
static int service_registry_count = 0;
static pthread_mutex_t service_registry_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Generate a unique stack name
 */
static void
generate_stack_id(char *id, size_t len)
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
	
	snprintf(id, len, "stack-%08x", 
	    (uint32_t)(hash[0] << 24 | hash[1] << 16 | hash[2] << 8 | hash[3]));
}

/*
 * Get stack state file path
 */
static char *
get_stack_state_path(const char *name, const char *namespace)
{
	char *path;
	
	asprintf(&path, "%s/stacks/%s/%s.json", 
	    OCIFBSD_ORCH_VAR_DIR, namespace, name);
	return (path);
}

/*
 * Save stack state to disk
 */
static int
save_stack_state(struct stack *stack)
{
	FILE *fp;
	char *path;
	
	path = get_stack_state_path(stack->name, stack->namespace);
	if (path == NULL)
		return (-1);
	
	if (mkdirp(path, 0755) != 0 && errno != EEXIST) {
		free(path);
		return (-1);
	}
	
	fp = fopen(path, "w");
	free(path);
	
	if (fp == NULL)
		return (-1);
	
	fprintf(fp, "{\n");
	fprintf(fp, "  \"name\": \"%s\",\n", stack->name);
	fprintf(fp, "  \"namespace\": \"%s\",\n", stack->namespace);
	fprintf(fp, "  \"version\": \"%s\",\n", stack->spec->version);
	if (stack->status)
		fprintf(fp, "  \"state\": \"%s\"\n", stack->status->state);
	fprintf(fp, "}\n");
	
	fclose(fp);
	return (0);
}

/*
 * Create a new stack
 */
struct stack *
stack_create(struct stack_spec *spec)
{
	struct stack *stack;
	struct stack_status *status;
	int i;
	
	if (spec == NULL || spec->name[0] == '\0') {
		errno = EINVAL;
		return (NULL);
	}
	
	stack = calloc(1, sizeof(struct stack));
	if (stack == NULL)
		return (NULL);
	
	/* Initialize stack */
	strlcpy(stack->name, spec->name, sizeof(stack->name));
	strlcpy(stack->namespace, 
	    spec->namespace[0] ? spec->namespace : "default",
	    sizeof(stack->namespace));
	
	/* Copy spec */
	stack->spec = calloc(1, sizeof(struct stack_spec));
	if (stack->spec == NULL) {
		free(stack);
		return (NULL);
	}
	memcpy(stack->spec, spec, sizeof(struct stack_spec));
	
	/* Initialize status */
	status = calloc(1, sizeof(struct stack_status));
	if (status == NULL) {
		free(stack->spec);
		free(stack);
		return (NULL);
	}
	
	strlcpy(status->name, stack->name, sizeof(status->name));
	strlcpy(status->namespace, stack->namespace, sizeof(status->namespace));
	strlcpy(status->state, "pending", sizeof(status->state));
	status->created = time(NULL);
	status->ntotal = spec->nservices;
	
	stack->status = status;
	
	/* Create services for this stack */
	for (i = 0; i < spec->nservices; i++) {
		struct service_spec *svc_spec = &spec->services[i];
		struct service *svc;
		
		strlcpy(svc_spec->stack, stack->name, sizeof(svc_spec->stack));
		
		svc = service_create(svc_spec);
		if (svc == NULL) {
			/* Rollback created services */
			for (int j = 0; j < i; j++) {
				/* Service would be freed here */
			}
			free(status);
			free(stack->spec);
			free(stack);
			return (NULL);
		}
	}
	
	/* Add to registry */
	pthread_mutex_lock(&stack_registry_lock);
	if (stack_registry_count >= stack_registry_size) {
		stack_registry_size = stack_registry_size ? 
		    stack_registry_size * 2 : 16;
		stack_registry = realloc(stack_registry, 
		    stack_registry_size * sizeof(struct stack *));
		if (stack_registry == NULL) {
			pthread_mutex_unlock(&stack_registry_lock);
			free(status);
			free(stack->spec);
			free(stack);
			return (NULL);
		}
	}
	stack_registry[stack_registry_count++] = stack;
	pthread_mutex_unlock(&stack_registry_lock);
	
	/* Save initial state */
	save_stack_state(stack);
	
	orch_event_publish("Normal", "Created", stack->namespace,
	    "Stack %s created", stack->name);
	
	return (stack);
}

/*
 * Start a stack (starts all services)
 */
int
stack_start(struct stack *stack)
{
	struct service **services;
	int count;
	int ret = 0;
	
	if (stack == NULL)
		return (-1);
	
	/* Get services belonging to this stack */
	services = service_list(stack->namespace, &count);
	if (services == NULL)
		return (0);  /* No services */
	
	for (int i = 0; i < count; i++) {
		if (strcmp(services[i]->stack, stack->name) != 0)
			continue;
		
		if (service_start(services[i]) != 0)
			ret = -1;
	}
	
	free(services);
	
	stack->status->state[0] = '\0';
	strlcpy(stack->status->state, "running", sizeof(stack->status->state));
	stack->status->updated = time(NULL);
	
	save_stack_state(stack);
	
	orch_event_publish("Normal", "Started", stack->namespace,
	    "Stack %s started", stack->name);
	
	return (ret);
}

/*
 * Stop a stack (stops all services)
 */
int
stack_stop(struct stack *stack)
{
	struct service **services;
	int count;
	int ret = 0;
	
	if (stack == NULL)
		return (-1);
	
	services = service_list(stack->namespace, &count);
	if (services == NULL)
		return (0);
	
	for (int i = 0; i < count; i++) {
		if (strcmp(services[i]->stack, stack->name) != 0)
			continue;
		
		if (service_stop(services[i]) != 0)
			ret = -1;
	}
	
	free(services);
	
	strlcpy(stack->status->state, "stopped", sizeof(stack->status->state));
	stack->status->updated = time(NULL);
	
	save_stack_state(stack);
	
	orch_event_publish("Normal", "Stopped", stack->namespace,
	    "Stack %s stopped", stack->name);
	
	return (ret);
}

/*
 * Delete a stack
 */
int
stack_delete(struct stack *stack)
{
	struct service **services;
	int count;
	int ret = 0;
	
	if (stack == NULL)
		return (-1);
	
	/* Stop all services first */
	stack_stop(stack);
	
	/* Delete all services belonging to this stack */
	services = service_list(stack->namespace, &count);
	if (services != NULL) {
		for (int i = 0; i < count; i++) {
			if (strcmp(services[i]->stack, stack->name) != 0)
				continue;
			
			if (service_delete(services[i]) != 0)
				ret = -1;
		}
		free(services);
	}
	
	/* Remove from registry */
	pthread_mutex_lock(&stack_registry_lock);
	for (int i = 0; i < stack_registry_count; i++) {
		if (stack_registry[i] == stack) {
			memmove(&stack_registry[i], &stack_registry[i + 1],
			    (stack_registry_count - i - 1) * sizeof(struct stack *));
			stack_registry_count--;
			break;
		}
	}
	pthread_mutex_unlock(&stack_registry_lock);
	
	orch_event_publish("Normal", "Deleted", stack->namespace,
	    "Stack %s deleted", stack->name);
	
	stack_free(stack);
	return (ret);
}

/*
 * Get stack by name
 */
struct stack *
stack_get(const char *name, const char *namespace)
{
	const char *ns = namespace ? namespace : "default";
	
	pthread_mutex_lock(&stack_registry_lock);
	for (int i = 0; i < stack_registry_count; i++) {
		if (strcmp(stack_registry[i]->name, name) == 0 &&
		    strcmp(stack_registry[i]->namespace, ns) == 0) {
			pthread_mutex_unlock(&stack_registry_lock);
			return (stack_registry[i]);
		}
	}
	pthread_mutex_unlock(&stack_registry_lock);
	
	errno = ENOENT;
	return (NULL);
}

/*
 * List stacks
 */
struct stack **
stack_list(const char *namespace, int *count)
{
	struct stack **result;
	const char *ns = namespace ? namespace : "default";
	int alloc = 16;
	int n = 0;
	
	*count = 0;
	result = calloc(alloc, sizeof(struct stack *));
	if (result == NULL)
		return (NULL);
	
	pthread_mutex_lock(&stack_registry_lock);
	for (int i = 0; i < stack_registry_count; i++) {
		if (strcmp(stack_registry[i]->namespace, ns) == 0) {
			if (n >= alloc) {
				alloc *= 2;
				result = realloc(result, alloc * sizeof(struct stack *));
			}
			result[n++] = stack_registry[i];
		}
	}
	pthread_mutex_unlock(&stack_registry_lock);
	
	*count = n;
	return (result);
}

/*
 * Free stack resources
 */
void
stack_free(struct stack *stack)
{
	if (stack == NULL)
		return;
	
	free(stack->spec);
	free(stack->status);
	free(stack->state_file);
	free(stack);
}

/*
 * Update stack
 */
int
stack_update(struct stack *stack, struct stack_spec *new_spec)
{
	if (stack == NULL || new_spec == NULL)
		return (-1);
	
	/* Stop current stack */
	stack_stop(stack);
	
	/* Update spec */
	free(stack->spec);
	stack->spec = calloc(1, sizeof(struct stack_spec));
	if (stack->spec == NULL)
		return (-1);
	memcpy(stack->spec, new_spec, sizeof(struct stack_spec));
	
	stack->status->updated = time(NULL);
	save_stack_state(stack);
	
	/* Restart stack */
	return (stack_start(stack));
}

/*
 * Service operations (also used by stack management)
 */

/*
 * Create a new service
 */
struct service *
service_create(struct service_spec *spec)
{
	struct service *service;
	struct service_status *status;
	
	if (spec == NULL || spec->name[0] == '\0') {
		errno = EINVAL;
		return (NULL);
	}
	
	service = calloc(1, sizeof(struct service));
	if (service == NULL)
		return (NULL);
	
	strlcpy(service->name, spec->name, sizeof(service->name));
	strlcpy(service->namespace, 
	    spec->name[0] ? spec->name : "default",
	    sizeof(service->namespace));
	strlcpy(service->stack, spec->stack, sizeof(service->stack));
	
	/* Copy spec */
	service->spec = calloc(1, sizeof(struct service_spec));
	if (service->spec == NULL) {
		free(service);
		return (NULL);
	}
	memcpy(service->spec, spec, sizeof(struct service_spec));
	
	/* Initialize status */
	status = calloc(1, sizeof(struct service_status));
	if (status == NULL) {
		free(service->spec);
		free(service);
		return (NULL);
	}
	
	strlcpy(status->name, service->name, sizeof(status->name));
	strlcpy(status->namespace, service->namespace, sizeof(status->namespace));
	strlcpy(status->stack, service->stack, sizeof(status->stack));
	status->desired_replicas = spec->replicas;
	status->created = time(NULL);
	
	service->status = status;
	
	/* Allocate replicas */
	service->nreplicas = spec->replicas;
	service->replicas = calloc(spec->replicas, sizeof(struct service_replica));
	if (service->replicas == NULL) {
		free(status);
		free(service->spec);
		free(service);
		return (NULL);
	}
	
	/* Initialize replica array */
	for (int i = 0; i < spec->replicas; i++) {
		service->replicas[i].replica_id = i;
		strlcpy(service->replicas[i].service, service->name,
		    sizeof(service->replicas[i].service));
		snprintf(service->replicas[i].name, sizeof(service->replicas[i].name),
		    "%s-replica-%d", service->name, i);
		service->replicas[i].state = REPLICA_STATE_PENDING;
	}
	
	/* Add to registry */
	pthread_mutex_lock(&service_registry_lock);
	if (service_registry_count >= service_registry_size) {
		service_registry_size = service_registry_size ? 
		    service_registry_size * 2 : 16;
		service_registry = realloc(service_registry, 
		    service_registry_size * sizeof(struct service *));
		if (service_registry == NULL) {
			pthread_mutex_unlock(&service_registry_lock);
			free(service->replicas);
			free(status);
			free(service->spec);
			free(service);
			return (NULL);
		}
	}
	service_registry[service_registry_count++] = service;
	pthread_mutex_unlock(&service_registry_lock);
	
	orch_event_publish("Normal", "ServiceCreated", service->namespace,
	    "Service %s created with %d replicas", 
	    service->name, spec->replicas);
	
	return (service);
}

/*
 * Start a service
 */
int
service_start(struct service *service)
{
	struct scheduling_decision *decision;

	if (service == NULL)
		return (-1);
	
	/* Start health checker if configured */
	if (service->spec->health_check.type != HEALTH_CHECK_NONE) {
		health_check_start(service);
	}
	
	/* Create replicas */
	for (int i = 0; i < service->nreplicas; i++) {
		struct pod_spec spec;
		struct pod *pod;
		char pod_name[256];
		
		snprintf(pod_name, sizeof(pod_name), "%s-replica-%d",
		    service->name, i);
		
		/* Build pod spec for this replica */
		memset(&spec, 0, sizeof(spec));
		strlcpy(spec.name, pod_name, sizeof(spec.name));
		strlcpy(spec.namespace, service->namespace, sizeof(spec.namespace));
		spec.ncontainers = 1;
		
		/* Create container spec */
		strlcpy(spec.containers[0].name, service->name,
		    sizeof(spec.containers[0].name));
		strlcpy(spec.containers[0].image, service->spec->image,
		    sizeof(spec.containers[0].image));
		
		/* Schedule pod */
		decision = scheduler_select_node(&spec);
		if (decision == NULL) {
			service->replicas[i].state = REPLICA_STATE_FAILED;
			return (-1);
		}
		
		strlcpy(service->replicas[i].node, decision->node,
		    sizeof(service->replicas[i].node));
		strlcpy(service->replicas[i].pod_name, pod_name,
		    sizeof(service->replicas[i].pod_name));
		free(decision);
		
		/* Create and start pod */
		pod = pod_create(&spec);
		if (pod == NULL) {
			service->replicas[i].state = REPLICA_STATE_FAILED;
			return (-1);
		}
		
		if (pod_start(pod) == 0) {
			service->replicas[i].state = REPLICA_STATE_RUNNING;
			service->replicas[i].started = time(NULL);
			service->status->available_replicas++;
		} else {
			service->replicas[i].state = REPLICA_STATE_FAILED;
		}
	}
	
	service->status->ready_replicas = service->status->available_replicas;
	service->status->updated = time(NULL);
	
	orch_event_publish("Normal", "ServiceStarted", service->namespace,
	    "Service %s started", service->name);
	
	return (0);
}

/*
 * Stop a service
 */
int
service_stop(struct service *service)
{
	if (service == NULL)
		return (-1);
	
	/* Stop health checker */
	health_check_stop(service);
	
	/* Stop all replicas */
	for (int i = 0; i < service->nreplicas; i++) {
		struct pod *pod;
		
		if (service->replicas[i].pod_name[0] == '\0')
			continue;
		
		pod = pod_get(service->replicas[i].pod_name, service->namespace);
		if (pod != NULL) {
			pod_stop(pod, SIGTERM);
			pod_delete(pod);
		}
		
		service->replicas[i].state = REPLICA_STATE_TERMINATED;
	}
	
	service->status->available_replicas = 0;
	service->status->ready_replicas = 0;
	service->status->updated = time(NULL);
	
	orch_event_publish("Normal", "ServiceStopped", service->namespace,
	    "Service %s stopped", service->name);
	
	return (0);
}

/*
 * Delete a service
 */
int
service_delete(struct service *service)
{
	int ret;
	
	if (service == NULL)
		return (-1);
	
	ret = service_stop(service);
	
	/* Remove from registry */
	pthread_mutex_lock(&service_registry_lock);
	for (int i = 0; i < service_registry_count; i++) {
		if (service_registry[i] == service) {
			memmove(&service_registry[i], &service_registry[i + 1],
			    (service_registry_count - i - 1) * sizeof(struct service *));
			service_registry_count--;
			break;
		}
	}
	pthread_mutex_unlock(&service_registry_lock);
	
	orch_event_publish("Normal", "ServiceDeleted", service->namespace,
	    "Service %s deleted", service->name);
	
	service_free(service);
	return (ret);
}

/*
 * Get service by name
 */
struct service *
service_get(const char *name, const char *namespace)
{
	const char *ns = namespace ? namespace : "default";
	
	pthread_mutex_lock(&service_registry_lock);
	for (int i = 0; i < service_registry_count; i++) {
		if (strcmp(service_registry[i]->name, name) == 0 &&
		    strcmp(service_registry[i]->namespace, ns) == 0) {
			pthread_mutex_unlock(&service_registry_lock);
			return (service_registry[i]);
		}
	}
	pthread_mutex_unlock(&service_registry_lock);
	
	errno = ENOENT;
	return (NULL);
}

/*
 * List services
 */
struct service **
service_list(const char *namespace, int *count)
{
	struct service **result;
	const char *ns = namespace ? namespace : "default";
	int alloc = 16;
	int n = 0;
	
	*count = 0;
	result = calloc(alloc, sizeof(struct service *));
	if (result == NULL)
		return (NULL);
	
	pthread_mutex_lock(&service_registry_lock);
	for (int i = 0; i < service_registry_count; i++) {
		if (strcmp(service_registry[i]->namespace, ns) == 0) {
			if (n >= alloc) {
				alloc *= 2;
				result = realloc(result, alloc * sizeof(struct service *));
			}
			result[n++] = service_registry[i];
		}
	}
	pthread_mutex_unlock(&service_registry_lock);
	
	*count = n;
	return (result);
}

/*
 * Free service resources
 */
void
service_free(struct service *service)
{
	if (service == NULL)
		return;
	
	free(service->spec);
	free(service->status);
	free(service->replicas);
	free(service->state_file);
	free(service);
}

/*
 * Scale service
 */
int
service_scale(struct service *service, int replicas)
{
	if (service == NULL)
		return (-1);
	
	if (replicas > service->nreplicas) {
		/* Scale up */
		struct service_replica *new_replicas;
		int old_count = service->nreplicas;
		
		new_replicas = realloc(service->replicas,
		    replicas * sizeof(struct service_replica));
		if (new_replicas == NULL)
			return (-1);
		
		service->replicas = new_replicas;
		
		/* Initialize new replicas */
		for (int i = old_count; i < replicas; i++) {
			service->replicas[i].replica_id = i;
			strlcpy(service->replicas[i].service, service->name,
			    sizeof(service->replicas[i].service));
			snprintf(service->replicas[i].name, 
			    sizeof(service->replicas[i].name),
			    "%s-replica-%d", service->name, i);
			service->replicas[i].state = REPLICA_STATE_PENDING;
		}
		
		service->nreplicas = replicas;
		service->spec->replicas = replicas;
		service->status->desired_replicas = replicas;
	} else if (replicas < service->nreplicas) {
		/* Scale down - stop excess replicas */
		for (int i = service->nreplicas - 1; i >= replicas; i--) {
			struct pod *pod;
			
			if (service->replicas[i].pod_name[0] == '\0')
				continue;
			
			pod = pod_get(service->replicas[i].pod_name, 
			    service->namespace);
			if (pod != NULL) {
				pod_stop(pod, SIGTERM);
				pod_delete(pod);
			}
		}
		
		service->nreplicas = replicas;
		service->spec->replicas = replicas;
		service->status->desired_replicas = replicas;
	}
	
	orch_event_publish("Normal", "Scaled", service->namespace,
	    "Service %s scaled to %d replicas", service->name, replicas);
	
	return (0);
}

/*
 * Update service
 */
int
service_update(struct service *service, struct service_spec *new_spec)
{
	if (service == NULL || new_spec == NULL)
		return (-1);
	
	/* Initiate rolling update */
	return (rolling_update_init(service, new_spec));
}

/*
 * Rollback service
 */
int
service_rollback(struct service *service)
{
	if (service == NULL)
		return (-1);
	
	/* Rollback rolling update */
	struct rolling_update_state *state;
	
	state = rolling_update_get_status(service->name, service->namespace);
	if (state == NULL)
		return (-1);
	
	return (rolling_update_rollback(state));
}

/*
 * Get service replicas
 */
struct service_replica **
service_get_replicas(struct service *service, int *count)
{
	struct service_replica **result;
	int n = 0;
	
	if (service == NULL || count == NULL)
		return (NULL);
	
	*count = 0;
	result = calloc(service->nreplicas, sizeof(struct service_replica *));
	if (result == NULL)
		return (NULL);
	
	for (int i = 0; i < service->nreplicas; i++) {
		result[n++] = &service->replicas[i];
	}
	
	*count = n;
	return (result);
}

/*
 * Get service status
 */
struct service_status *
service_get_status(struct service *service)
{
	int running = 0, ready = 0;
	
	if (service == NULL)
		return (NULL);
	
	for (int i = 0; i < service->nreplicas; i++) {
		if (service->replicas[i].state == REPLICA_STATE_RUNNING)
			running++;
	}
	
	service->status->available_replicas = running;
	service->status->ready_replicas = ready;
	
	return (service->status);
}
