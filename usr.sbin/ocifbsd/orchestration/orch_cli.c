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
 * OCI FreeBSD Orchestration CLI - CLI integration for orchestration commands
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libutil.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "orchestration.h"
#include "../include/ocifbsd.h"

/*
 * Forward declarations
 */
static int cmd_pod_create(int argc, char **argv);
static int cmd_pod_list(int argc, char **argv);
static int cmd_pod_delete(int argc, char **argv);
static int cmd_pod_logs(int argc, char **argv);

static int cmd_service_create(int argc, char **argv);
static int cmd_service_scale(int argc, char **argv);
static int cmd_service_update(int argc, char **argv);
static int cmd_service_list(int argc, char **argv);
static int cmd_service_delete(int argc, char **argv);

static int cmd_stack_create(int argc, char **argv);
static int cmd_stack_up(int argc, char **argv);
static int cmd_stack_down(int argc, char **argv);
static int cmd_stack_list(int argc, char **argv);
static int cmd_stack_delete(int argc, char **argv);

static int cmd_rolling_update(int argc, char **argv);
static int cmd_rolling_pause(int argc, char **argv);
static int cmd_rolling_resume(int argc, char **argv);
static int cmd_rolling_rollback(int argc, char **argv);

static int cmd_node_list(int argc, char **argv);
static int cmd_node_add(int argc, char **argv);

/*
 * Pod commands
 */
static int
cmd_pod_create(int argc, char **argv)
{
	struct pod_spec spec;
	char *name = NULL;
	char *namespace = "default";
	
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "namespace", required_argument, NULL, 'N' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:N:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'N':
			namespace = optarg;
			break;
		case 'h':
		default:
			fprintf(stderr, "Usage: ocifbsd pod create [-n name] [-N namespace]\n");
			return (ch == 'h' ? 0 : 1);
		}
	}
	
	if (name == NULL) {
		fprintf(stderr, "Error: --name is required\n");
		return (1);
	}
	
	memset(&spec, 0, sizeof(spec));
	strlcpy(spec.name, name, sizeof(spec.name));
	strlcpy(spec.namespace, namespace, sizeof(spec.namespace));
	
	struct pod *pod = pod_create(&spec);
	if (pod == NULL) {
		fprintf(stderr, "Error: Failed to create pod: %s\n", strerror(errno));
		return (1);
	}
	
	printf("Pod %s created with UID %s\n", pod->name, pod->uid);
	/* pod is owned by the registry; do not free it here. */
	return (0);
}

static int
cmd_pod_list(int argc, char **argv)
{
	struct pod **pods;
	int count;
	char *namespace = "default";
	
	static struct option longopts[] = {
		{ "namespace", required_argument, NULL, 'N' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "N:h", longopts, NULL)) != -1) {
		switch (ch) {
		case 'N':
			namespace = optarg;
			break;
		case 'h':
		default:
			return (ch == 'h' ? 0 : 1);
		}
	}
	
	pods = pod_list(namespace, &count);
	if (pods == NULL) {
		printf("No pods found in namespace %s\n", namespace);
		return (0);
	}
	
	printf("%-30s %-20s %-10s %s\n", "NAME", "UID", "STATE", "NAMESPACE");
	printf("%-30s %-20s %-10s %s\n", "----", "---", "-----", "---------");
	
	for (int i = 0; i < count; i++) {
		printf("%-30s %-20s %-10d %s\n",
		    pods[i]->name,
		    pods[i]->uid,
		    pods[i]->status->state,
		    pods[i]->namespace);
	}
	
	/*
	 * pods[] are registry-owned pointers (pod_list returns references,
	 * not copies); only free the array, never the elements. Freeing them
	 * here left dangling pointers in the registry (UAF on next access,
	 * double-free on delete).
	 */
	free(pods);

	return (0);
}

static int
cmd_pod_delete(int argc, char **argv)
{
	char *name = NULL;
	char *namespace = "default";
	
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "namespace", required_argument, NULL, 'N' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:N:", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'N':
			namespace = optarg;
			break;
		}
	}
	
	if (name == NULL) {
		fprintf(stderr, "Error: --name is required\n");
		return (1);
	}
	
	struct pod *pod = pod_get(name, namespace);
	if (pod == NULL) {
		fprintf(stderr, "Error: Pod %s not found\n", name);
		return (1);
	}
	
	if (pod_delete(pod) != 0) {
		fprintf(stderr, "Error: Failed to delete pod: %s\n", strerror(errno));
		return (1);
	}
	
	printf("Pod %s deleted\n", name);
	return (0);
}

static int
cmd_pod_logs(int argc, char **argv)
{
	char *name = NULL;
	char *namespace = "default";
	char *container = NULL;
	int tail = 100;
	bool follow = false;
	
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "namespace", required_argument, NULL, 'N' },
		{ "container", required_argument, NULL, 'c' },
		{ "tail", required_argument, NULL, 't' },
		{ "follow", no_argument, NULL, 'f' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:N:c:t:f", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'N':
			namespace = optarg;
			break;
		case 'c':
			container = optarg;
			break;
		case 't':
			tail = atoi(optarg);
			break;
		case 'f':
			follow = true;
			break;
		}
	}
	
	if (name == NULL) {
		fprintf(stderr, "Error: --name is required\n");
		return (1);
	}
	
	struct pod *pod = pod_get(name, namespace);
	if (pod == NULL) {
		fprintf(stderr, "Error: Pod %s not found\n", name);
		return (1);
	}
	
	return (pod_logs(pod, container, tail, follow));
}

/*
 * Service commands
 */
static int
cmd_service_create(int argc, char **argv)
{
	struct service_spec spec;
	char *name = NULL;
	char *image = NULL;
	int replicas = 1;
	
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "image", required_argument, NULL, 'i' },
		{ "namespace", required_argument, NULL, 'N' },
		{ "replicas", required_argument, NULL, 'r' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:i:N:r:", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'i':
			image = optarg;
			break;
		case 'N':
			break;
		case 'r':
			replicas = atoi(optarg);
			break;
		}
	}
	
	if (name == NULL || image == NULL) {
		fprintf(stderr, "Error: --name and --image are required\n");
		return (1);
	}
	
	memset(&spec, 0, sizeof(spec));
	strlcpy(spec.name, name, sizeof(spec.name));
	strlcpy(spec.image, image, sizeof(spec.image));
	spec.replicas = replicas;
	
	struct service *svc = service_create(&spec);
	if (svc == NULL) {
		fprintf(stderr, "Error: Failed to create service: %s\n", strerror(errno));
		return (1);
	}

	/*
	 * Compose-"up"/apply semantics: creating a service also launches its
	 * replicas. Best-effort — a replica whose image is not in the local
	 * store is marked failed, but the service is still created and can be
	 * scaled/deleted. Report how many replicas actually came up.
	 */
	(void)service_start(svc);

	printf("Service %s created with %d replicas (%d running)\n",
	    svc->name, replicas,
	    svc->status != NULL ? svc->status->available_replicas : 0);
	/* svc is owned by the registry; do not free it here. */
	return (0);
}

static int
cmd_service_scale(int argc, char **argv)
{
	char *name = NULL;
	char *namespace = "default";
	int replicas = 1;
	
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "namespace", required_argument, NULL, 'N' },
		{ "replicas", required_argument, NULL, 'r' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:N:r:", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'N':
			namespace = optarg;
			break;
		case 'r':
			replicas = atoi(optarg);
			break;
		}
	}
	
	if (name == NULL) {
		fprintf(stderr, "Error: --name is required\n");
		return (1);
	}
	
	struct service *svc = service_get(name, namespace);
	if (svc == NULL) {
		fprintf(stderr, "Error: Service %s not found\n", name);
		return (1);
	}
	
	if (service_scale(svc, replicas) != 0) {
		fprintf(stderr, "Error: Failed to scale service: %s\n", strerror(errno));
		return (1);
	}

	printf("Service %s scaled to %d replicas\n", name, replicas);
	/* svc is registry-owned; do not free. */
	return (0);
}

static int
cmd_service_update(int argc, char **argv)
{
	char *name = NULL;
	char *namespace = "default";
	char *image = NULL;
	
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "namespace", required_argument, NULL, 'N' },
		{ "image", required_argument, NULL, 'i' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:N:i:", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'N':
			namespace = optarg;
			break;
		case 'i':
			image = optarg;
			break;
		}
	}
	
	if (name == NULL) {
		fprintf(stderr, "Error: --name is required\n");
		return (1);
	}
	
	struct service *svc = service_get(name, namespace);
	if (svc == NULL) {
		fprintf(stderr, "Error: Service %s not found\n", name);
		return (1);
	}
	
	if (image != NULL) {
		struct service_spec new_spec;
		memcpy(&new_spec, svc->spec, sizeof(new_spec));
		strlcpy(new_spec.image, image, sizeof(new_spec.image));
		
		if (service_update(svc, &new_spec) != 0) {
			fprintf(stderr, "Error: Failed to update service: %s\n", strerror(errno));
			return (1);
		}

		printf("Service %s update initiated\n", name);
	} else {
		printf("No image specified for update\n");
	}

	/* svc is registry-owned; do not free. */
	return (0);
}

static int
cmd_service_list(int argc, char **argv)
{
	struct service **services;
	int count;
	char *namespace = "default";
	
	static struct option longopts[] = {
		{ "namespace", required_argument, NULL, 'N' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "N:", longopts, NULL)) != -1) {
		switch (ch) {
		case 'N':
			namespace = optarg;
			break;
		}
	}
	
	services = service_list(namespace, &count);
	if (services == NULL) {
		printf("No services found in namespace %s\n", namespace);
		return (0);
	}
	
	printf("%-30s %-15s %-10s %-10s %s\n", "NAME", "IMAGE", "REPLICAS", "AVAILABLE", "NAMESPACE");
	printf("%-30s %-15s %-10s %-10s %s\n", "----", "-----", "--------", "---------", "---------");
	
	for (int i = 0; i < count; i++) {
		struct service_status *status = service_get_status(services[i]);
		printf("%-30s %-15s %-10d %-10d %s\n",
		    services[i]->name,
		    services[i]->spec->image,
		    services[i]->spec->replicas,
		    status ? status->available_replicas : 0,
		    services[i]->namespace);
	}
	
	/* services[] are registry-owned references; free only the array. */
	free(services);
	
	return (0);
}

static int
cmd_service_delete(int argc, char **argv)
{
	char *name = NULL;
	char *namespace = "default";
	
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "namespace", required_argument, NULL, 'N' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:N:", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'N':
			namespace = optarg;
			break;
		}
	}
	
	if (name == NULL) {
		fprintf(stderr, "Error: --name is required\n");
		return (1);
	}
	
	struct service *svc = service_get(name, namespace);
	if (svc == NULL) {
		fprintf(stderr, "Error: Service %s not found\n", name);
		return (1);
	}
	
	if (service_delete(svc) != 0) {
		fprintf(stderr, "Error: Failed to delete service: %s\n", strerror(errno));
		return (1);
	}
	
	printf("Service %s deleted\n", name);
	return (0);
}

/*
 * Stack commands
 */
static int
cmd_stack_create(int argc, char **argv)
{
	char *name = NULL;

	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ "file", required_argument, NULL, 'f' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:f:", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		case 'f':
			break;
		}
	}
	
	if (name == NULL) {
		fprintf(stderr, "Error: --name is required\n");
		return (1);
	}
	
	/* Stack creation from file would parse the stack spec here */
	struct stack_spec spec;
	memset(&spec, 0, sizeof(spec));
	strlcpy(spec.name, name, sizeof(spec.name));
	
	struct stack *stack = stack_create(&spec);
	if (stack == NULL) {
		fprintf(stderr, "Error: Failed to create stack: %s\n", strerror(errno));
		return (1);
	}
	
	printf("Stack %s created\n", name);
	/* stack is registry-owned; do not free. */
	return (0);
}

static int
cmd_stack_up(int argc, char **argv)
{
	char *name = NULL;
	
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		}
	}
	
	if (name == NULL) {
		fprintf(stderr, "Error: --name is required\n");
		return (1);
	}
	
	struct stack *stack = stack_get(name, "default");
	if (stack == NULL) {
		fprintf(stderr, "Error: Stack %s not found\n", name);
		return (1);
	}
	
	if (stack_start(stack) != 0) {
		fprintf(stderr, "Error: Failed to start stack: %s\n", strerror(errno));
		return (1);
	}

	printf("Stack %s started\n", name);
	/* stack is registry-owned; do not free. */
	return (0);
}

static int
cmd_stack_down(int argc, char **argv)
{
	char *name = NULL;
	
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		}
	}
	
	if (name == NULL) {
		fprintf(stderr, "Error: --name is required\n");
		return (1);
	}
	
	struct stack *stack = stack_get(name, "default");
	if (stack == NULL) {
		fprintf(stderr, "Error: Stack %s not found\n", name);
		return (1);
	}
	
	if (stack_stop(stack) != 0) {
		fprintf(stderr, "Error: Failed to stop stack: %s\n", strerror(errno));
		return (1);
	}

	printf("Stack %s stopped\n", name);
	/* stack is registry-owned; do not free. */
	return (0);
}

static int
cmd_stack_list(int argc, char **argv)
{
	struct stack **stacks;
	int count;
	char *namespace = "default";
	
	stacks = stack_list(namespace, &count);
	if (stacks == NULL) {
		printf("No stacks found\n");
		return (0);
	}
	
	printf("%-30s %-15s %-10s %s\n", "NAME", "VERSION", "STATE", "NAMESPACE");
	printf("%-30s %-15s %-10s %s\n", "----", "-------", "-----", "---------");
	
	for (int i = 0; i < count; i++) {
		/*
		 * A stack reconstructed from disk carries metadata + status but
		 * no spec (services are not persisted), so spec may be NULL.
		 */
		printf("%-30s %-15s %-10s %s\n",
		    stacks[i]->name,
		    (stacks[i]->spec != NULL &&
		     stacks[i]->spec->version[0] != '\0') ?
		    stacks[i]->spec->version : "-",
		    (stacks[i]->status != NULL) ?
		    stacks[i]->status->state : "-",
		    stacks[i]->namespace);
	}
	
	/* stacks[] are registry-owned references; free only the array. */
	free(stacks);
	
	return (0);
}

static int
cmd_stack_delete(int argc, char **argv)
{
	char *name = NULL;
	
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		}
	}
	
	if (name == NULL) {
		fprintf(stderr, "Error: --name is required\n");
		return (1);
	}
	
	struct stack *stack = stack_get(name, "default");
	if (stack == NULL) {
		fprintf(stderr, "Error: Stack %s not found\n", name);
		return (1);
	}
	
	if (stack_delete(stack) != 0) {
		fprintf(stderr, "Error: Failed to delete stack: %s\n", strerror(errno));
		return (1);
	}
	
	printf("Stack %s deleted\n", name);
	return (0);
}

/*
 * Rolling update commands
 */
static int
cmd_rolling_update(int argc, char **argv)
{
	char *service = NULL;
	char *namespace = "default";
	
	static struct option longopts[] = {
		{ "service", required_argument, NULL, 's' },
		{ "namespace", required_argument, NULL, 'N' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "s:N:", longopts, NULL)) != -1) {
		switch (ch) {
		case 's':
			service = optarg;
			break;
		case 'N':
			namespace = optarg;
			break;
		}
	}
	
	if (service == NULL) {
		fprintf(stderr, "Error: --service is required\n");
		return (1);
	}
	
	struct rolling_update_state *state = rolling_update_get_status(service, namespace);
	if (state != NULL) {
		printf("Rolling update in progress for %s:\n", service);
		printf("  Updated: %d/%d replicas\n", state->updated_replicas, state->total_replicas);
		printf("  Status: %s\n", state->status);
		return (0);
	}
	
	printf("No rolling update in progress for %s\n", service);
	return (0);
}

static int
cmd_rolling_pause(int argc, char **argv)
{
	char *service = NULL;
	
	static struct option longopts[] = {
		{ "service", required_argument, NULL, 's' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "s:", longopts, NULL)) != -1) {
		switch (ch) {
		case 's':
			service = optarg;
			break;
		}
	}
	
	if (service == NULL) {
		fprintf(stderr, "Error: --service is required\n");
		return (1);
	}
	
	struct rolling_update_state state = { .service = "", .namespace = "" };
	strlcpy(state.service, service, sizeof(state.service));
	
	if (rolling_update_pause(&state) != 0) {
		fprintf(stderr, "Error: Failed to pause rolling update\n");
		return (1);
	}
	
	printf("Rolling update paused for %s\n", service);
	return (0);
}

static int
cmd_rolling_resume(int argc, char **argv)
{
	char *service = NULL;
	
	static struct option longopts[] = {
		{ "service", required_argument, NULL, 's' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "s:", longopts, NULL)) != -1) {
		switch (ch) {
		case 's':
			service = optarg;
			break;
		}
	}
	
	if (service == NULL) {
		fprintf(stderr, "Error: --service is required\n");
		return (1);
	}
	
	struct rolling_update_state state = { .service = "", .namespace = "" };
	strlcpy(state.service, service, sizeof(state.service));
	
	if (rolling_update_resume(&state) != 0) {
		fprintf(stderr, "Error: Failed to resume rolling update\n");
		return (1);
	}
	
	printf("Rolling update resumed for %s\n", service);
	return (0);
}

static int
cmd_rolling_rollback(int argc, char **argv)
{
	char *service = NULL;
	
	static struct option longopts[] = {
		{ "service", required_argument, NULL, 's' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "s:", longopts, NULL)) != -1) {
		switch (ch) {
		case 's':
			service = optarg;
			break;
		}
	}
	
	if (service == NULL) {
		fprintf(stderr, "Error: --service is required\n");
		return (1);
	}
	
	struct rolling_update_state state = { .service = "", .namespace = "" };
	strlcpy(state.service, service, sizeof(state.service));
	
	if (rolling_update_rollback(&state) != 0) {
		fprintf(stderr, "Error: Failed to rollback\n");
		return (1);
	}
	
	printf("Rolling update rolled back for %s\n", service);
	return (0);
}

/*
 * Node commands
 */
static int
cmd_node_list(int argc, char **argv)
{
	char **nodes;
	int count;
	
	nodes = scheduler_list_nodes(&count);
	if (nodes == NULL) {
		printf("No nodes found\n");
		return (0);
	}
	
	printf("%-30s %-15s %s\n", "NAME", "STATUS", "ADDRESS");
	printf("%-30s %-15s %s\n", "----", "------", "-------");
	
	for (int i = 0; i < count; i++) {
		printf("%-30s %-15s %s\n", nodes[i], "Ready", "127.0.0.1");
		free(nodes[i]);
	}
	free(nodes);
	
	return (0);
}

static int
cmd_node_add(int argc, char **argv)
{
	char *name = NULL;
	
	static struct option longopts[] = {
		{ "name", required_argument, NULL, 'n' },
		{ NULL, 0, NULL, 0 }
	};
	
	optind = 0;
	int ch;
	while ((ch = getopt_long(argc, argv, "n:", longopts, NULL)) != -1) {
		switch (ch) {
		case 'n':
			name = optarg;
			break;
		}
	}
	
	if (name == NULL) {
		fprintf(stderr, "Error: --name is required\n");
		return (1);
	}
	
	if (scheduler_add_node(name) != 0) {
		fprintf(stderr, "Error: Failed to add node: %s\n", strerror(errno));
		return (1);
	}
	
	printf("Node %s added\n", name);
	return (0);
}

/*
 * Orchestration command dispatch
 */
int
orch_cli_dispatch(int argc, char **argv)
{
	if (argc < 2)
		goto usage;
	
	/* Pod commands */
	if (strcmp(argv[1], "pod") == 0) {
		if (argc < 3)
			goto usage;
		if (strcmp(argv[2], "create") == 0)
			return cmd_pod_create(argc - 2, argv + 2);
		if (strcmp(argv[2], "list") == 0)
			return cmd_pod_list(argc - 2, argv + 2);
		if (strcmp(argv[2], "delete") == 0)
			return cmd_pod_delete(argc - 2, argv + 2);
		if (strcmp(argv[2], "logs") == 0)
			return cmd_pod_logs(argc - 2, argv + 2);
	}
	
	/* Service commands */
	if (strcmp(argv[1], "service") == 0) {
		if (argc < 3)
			goto usage;
		if (strcmp(argv[2], "create") == 0)
			return cmd_service_create(argc - 2, argv + 2);
		if (strcmp(argv[2], "scale") == 0)
			return cmd_service_scale(argc - 2, argv + 2);
		if (strcmp(argv[2], "update") == 0)
			return cmd_service_update(argc - 2, argv + 2);
		if (strcmp(argv[2], "list") == 0)
			return cmd_service_list(argc - 2, argv + 2);
		if (strcmp(argv[2], "delete") == 0)
			return cmd_service_delete(argc - 2, argv + 2);
	}
	
	/* Stack commands */
	if (strcmp(argv[1], "stack") == 0) {
		if (argc < 3)
			goto usage;
		if (strcmp(argv[2], "create") == 0)
			return cmd_stack_create(argc - 2, argv + 2);
		if (strcmp(argv[2], "up") == 0)
			return cmd_stack_up(argc - 2, argv + 2);
		if (strcmp(argv[2], "down") == 0)
			return cmd_stack_down(argc - 2, argv + 2);
		if (strcmp(argv[2], "list") == 0)
			return cmd_stack_list(argc - 2, argv + 2);
		if (strcmp(argv[2], "delete") == 0)
			return cmd_stack_delete(argc - 2, argv + 2);
	}
	
	/* Rolling update commands */
	if (strcmp(argv[1], "rolling") == 0) {
		if (argc < 3)
			goto usage;
		if (strcmp(argv[2], "update") == 0)
			return cmd_rolling_update(argc - 2, argv + 2);
		if (strcmp(argv[2], "pause") == 0)
			return cmd_rolling_pause(argc - 2, argv + 2);
		if (strcmp(argv[2], "resume") == 0)
			return cmd_rolling_resume(argc - 2, argv + 2);
		if (strcmp(argv[2], "rollback") == 0)
			return cmd_rolling_rollback(argc - 2, argv + 2);
	}
	
	/* Node commands */
	if (strcmp(argv[1], "node") == 0) {
		if (argc < 3)
			goto usage;
		if (strcmp(argv[2], "list") == 0)
			return cmd_node_list(argc - 2, argv + 2);
		if (strcmp(argv[2], "add") == 0)
			return cmd_node_add(argc - 2, argv + 2);
	}

usage:
	fprintf(stderr, "Usage: ocifbsd <command> [options]\n");
	fprintf(stderr, "\nCommands:\n");
	fprintf(stderr, "  pod <create|list|delete|logs>   Pod management\n");
	fprintf(stderr, "  service <create|scale|update|list|delete>  Service management\n");
	fprintf(stderr, "  stack <create|up|down|list|delete>  Stack management\n");
	fprintf(stderr, "  rolling <update|pause|resume|rollback>  Rolling update control\n");
	fprintf(stderr, "  node <list|add>  Node management\n");
	return (1);
}
