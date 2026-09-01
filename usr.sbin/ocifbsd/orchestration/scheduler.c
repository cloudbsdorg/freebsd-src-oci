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
 * Scheduler implementation - node selection for pod placement
 */

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/if_mib.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "orchestration.h"

static uint64_t get_physmem(void);

#define MAX_NODES 256

/*
 * Node information
 */
struct node_info {
	char		name[256];
	char		address[64];
	bool		ready;
	bool		schedulable;
	time_t		last_heartbeat;
	
	/* Resource capacity */
	uint64_t	memory_capacity;
	uint64_t	memory_used;
	uint64_t	cpu_capacity;	/* in millicores */
	uint64_t	cpu_used;
	uint64_t	storage_capacity;
	uint64_t	storage_used;
	
	/* Attributes */
	char		region[64];
	char		zone[64];
	char		instance_type[64];
	char		labels[512];	/* JSON: {"key": "value"} */
	
	/* Metrics (updated periodically) */
	double		load_average_1m;
	double		load_average_5m;
	double		load_average_15m;
	uint64_t	pods_running;
	uint64_t	pods_capacity;
};

/*
 * Global state
 */
static struct node_info *nodes[MAX_NODES];
static int node_count = 0;
static pthread_mutex_t scheduler_lock = PTHREAD_MUTEX_INITIALIZER;
static bool scheduler_initialized = false;

/*
 * Scoring weights for bin-packing vs spread
 */
static struct {
	int	cpu_weight;
	int	memory_weight;
	int	pod_count_weight;
} scoring_weights = {
	.cpu_weight = 10,
	.memory_weight = 10,
	.pod_count_weight = 5
};

/*
 * Initialize scheduler
 */
int
scheduler_init(void)
{
	pthread_mutex_lock(&scheduler_lock);
	
	if (scheduler_initialized) {
		pthread_mutex_unlock(&scheduler_lock);
		return (0);
	}
	
	/* Add local node by default. nodes[] holds pointers, all NULL; the
	 * local node must be allocated before use. */
	nodes[0] = calloc(1, sizeof(struct node_info));
	if (nodes[0] == NULL) {
		pthread_mutex_unlock(&scheduler_lock);
		return (-1);
	}
	strlcpy(nodes[0]->name, "localhost", sizeof(nodes[0]->name));
	strlcpy(nodes[0]->address, "127.0.0.1", sizeof(nodes[0]->address));
	nodes[0]->ready = true;
	nodes[0]->schedulable = true;
	nodes[0]->last_heartbeat = time(NULL);
	nodes[0]->memory_capacity = get_physmem() * 4096;  /* bytes */
	nodes[0]->cpu_capacity = sysconf(_SC_NPROCESSORS_ONLN) * 1000;  /* millicores */
	nodes[0]->pods_capacity = 110;  /* default k8s limit */
	
	node_count = 1;
	scheduler_initialized = true;
	
	pthread_mutex_unlock(&scheduler_lock);
	
	return (0);
}

/*
 * Get available memory on system
 */
static uint64_t
get_physmem(void)
{
	size_t len = sizeof(uint64_t);
	uint64_t physmem;
	
	if (sysctlbyname("hw.physmem", &physmem, &len, NULL, 0) != 0)
		return (0);
	
	return (physmem);
}

/*
 * Calculate node score for pod placement
 */
static double
score_node(struct node_info *node, struct pod_spec *spec)
{
	double score = 100.0;
	
	if (!node->ready || !node->schedulable)
		return (0.0);
	
	/* Check if node has enough resources */
	if (spec->resources.memory_limit > 0) {
		uint64_t avail = node->memory_capacity - node->memory_used;
		if (avail < spec->resources.memory_limit)
			return (0.0);
	}
	
	if (spec->resources.cpu_limit > 0) {
		uint64_t avail = node->cpu_capacity - node->cpu_used;
		if (avail < (uint64_t)spec->resources.cpu_limit)
			return (0.0);
	}
	
	/* Score based on available resources (bin-packing) */
	if (node->memory_capacity > 0) {
		double mem_avail_pct = (double)(node->memory_capacity - node->memory_used) / 
		    node->memory_capacity;
		score -= (1.0 - mem_avail_pct) * scoring_weights.memory_weight;
	}
	
	if (node->cpu_capacity > 0) {
		double cpu_avail_pct = (double)(node->cpu_capacity - node->cpu_used) / 
		    node->cpu_capacity;
		score -= (1.0 - cpu_avail_pct) * scoring_weights.cpu_weight;
	}
	
	/* Prefer nodes with fewer pods (spread) */
	if (node->pods_capacity > 0) {
		double pods_pct = (double)node->pods_running / node->pods_capacity;
		score -= pods_pct * scoring_weights.pod_count_weight;
	}
	
	/* Score must be non-negative */
	if (score < 0)
		score = 0;
	
	return (score);
}

/*
 * Select best node for pod placement
 */
struct scheduling_decision *
scheduler_select_node(struct pod_spec *spec)
{
	struct scheduling_decision *decision;
	struct node_info *best_node = NULL;
	double best_score = -1.0;
	char reason[256];
	
	decision = calloc(1, sizeof(struct scheduling_decision));
	if (decision == NULL)
		return (NULL);
	
	pthread_mutex_lock(&scheduler_lock);
	
	/* Find best node */
	for (int i = 0; i < node_count; i++) {
		struct node_info *node = nodes[i];
		double score;
		
		if (node == NULL)
			continue;
		
		score = score_node(node, spec);
		
		if (score > best_score) {
			best_score = score;
			best_node = node;
		}
	}
	
	pthread_mutex_unlock(&scheduler_lock);
	
	if (best_node == NULL) {
		strlcpy(reason, "No schedulable nodes available", sizeof(reason));
		decision->score = 0;
		decision->failed_reason = strdup(reason);
		return (decision);
	}
	
	/* Build decision */
	strlcpy(decision->node, best_node->name, sizeof(decision->node));
	decision->score = best_score;
	snprintf(reason, sizeof(reason), 
	    "Node has %.0f%% score (mem: %lu%%, cpu: %lu%%, pods: %lu/%lu)",
	    best_score,
	    (best_node->memory_capacity > 0) ?
	        (best_node->memory_used * 100 / best_node->memory_capacity) : 0,
	    (best_node->cpu_capacity > 0) ?
	        (best_node->cpu_used * 100 / best_node->cpu_capacity) : 0,
	    best_node->pods_running,
	    best_node->pods_capacity);
	strlcpy(decision->reason, reason, sizeof(decision->reason));
	
	/* Update node resource usage (preliminary) */
	pthread_mutex_lock(&scheduler_lock);
	if (spec->resources.memory_limit > 0)
		best_node->memory_used += spec->resources.memory_limit;
	if (spec->resources.cpu_limit > 0)
		best_node->cpu_used += spec->resources.cpu_limit;
	best_node->pods_running++;
	pthread_mutex_unlock(&scheduler_lock);
	
	return (decision);
}

/*
 * Score a specific node
 */
int
scheduler_score_node(const char *node_name, struct pod_spec *spec)
{
	struct node_info *node = NULL;
	
	pthread_mutex_lock(&scheduler_lock);
	
	for (int i = 0; i < node_count; i++) {
		if (nodes[i] != NULL && 
		    strcmp(nodes[i]->name, node_name) == 0) {
			node = nodes[i];
			break;
		}
	}
	
	pthread_mutex_unlock(&scheduler_lock);
	
	if (node == NULL) {
		errno = ENOENT;
		return (-1);
	}
	
	double score = score_node(node, spec);
	return ((int)(score * 100));  /* Return as integer 0-10000 */
}

/*
 * List all nodes
 */
char **
scheduler_list_nodes(int *count)
{
	char **result;
	int alloc;
	
	*count = 0;
	alloc = 16;
	result = malloc(alloc * sizeof(char *));
	if (result == NULL)
		return (NULL);
	
	pthread_mutex_lock(&scheduler_lock);
	
	for (int i = 0; i < node_count; i++) {
		if (nodes[i] == NULL)
			continue;
		
		if (*count >= alloc) {
			alloc *= 2;
			char **new_result = realloc(result, alloc * sizeof(char *));
			if (new_result == NULL) {
				free(result);
				pthread_mutex_unlock(&scheduler_lock);
				return (NULL);
			}
			result = new_result;
		}
		
		result[*count] = strdup(nodes[i]->name);
		(*count)++;
	}
	
	pthread_mutex_unlock(&scheduler_lock);
	
	return (result);
}

/*
 * Add a node to the cluster
 */
int
scheduler_add_node(const char *node_name)
{
	struct node_info *node;
	
	if (node_count >= MAX_NODES) {
		errno = ENOMEM;
		return (-1);
	}
	
	node = calloc(1, sizeof(struct node_info));
	if (node == NULL)
		return (-1);
	
	strlcpy(node->name, node_name, sizeof(node->name));
	node->ready = false;
	node->schedulable = false;
	node->last_heartbeat = time(NULL);
	node->pods_capacity = 110;
	
	pthread_mutex_lock(&scheduler_lock);
	nodes[node_count++] = node;
	pthread_mutex_unlock(&scheduler_lock);
	
	orch_event_publish("Normal", "NodeRegistered", "cluster",
	    "Node %s registered", node_name);
	
	return (0);
}

/*
 * Remove a node from the cluster
 */
int
scheduler_remove_node(const char *node_name)
{
	pthread_mutex_lock(&scheduler_lock);
	
	for (int i = 0; i < node_count; i++) {
		if (nodes[i] != NULL && 
		    strcmp(nodes[i]->name, node_name) == 0) {
			free(nodes[i]);
			memmove(&nodes[i], &nodes[i + 1],
			    (node_count - i - 1) * sizeof(struct node_info *));
			node_count--;
			pthread_mutex_unlock(&scheduler_lock);
			
			orch_event_publish("Warning", "NodeRemoved", "cluster",
			    "Node %s removed", node_name);
			return (0);
		}
	}
	
	pthread_mutex_unlock(&scheduler_lock);
	errno = ENOENT;
	return (-1);
}

/*
 * Mark node as ready
 */
int
scheduler_node_ready(const char *node_name)
{
	pthread_mutex_lock(&scheduler_lock);
	
	for (int i = 0; i < node_count; i++) {
		if (nodes[i] != NULL && 
		    strcmp(nodes[i]->name, node_name) == 0) {
			nodes[i]->ready = true;
			nodes[i]->schedulable = true;
			nodes[i]->last_heartbeat = time(NULL);
			pthread_mutex_unlock(&scheduler_lock);
			
			orch_event_publish("Normal", "NodeReady", "cluster",
			    "Node %s is ready", node_name);
			return (0);
		}
	}
	
	pthread_mutex_unlock(&scheduler_lock);
	errno = ENOENT;
	return (-1);
}

/*
 * Mark node as not ready
 */
int
scheduler_node_not_ready(const char *node_name)
{
	pthread_mutex_lock(&scheduler_lock);
	
	for (int i = 0; i < node_count; i++) {
		if (nodes[i] != NULL && 
		    strcmp(nodes[i]->name, node_name) == 0) {
			nodes[i]->ready = false;
			nodes[i]->schedulable = false;
			pthread_mutex_unlock(&scheduler_lock);
			
			orch_event_publish("Warning", "NodeNotReady", "cluster",
			    "Node %s is not ready", node_name);
			return (0);
		}
	}
	
	pthread_mutex_unlock(&scheduler_lock);
	errno = ENOENT;
	return (-1);
}

/*
 * Update node resources (called by metrics collector)
 */
static int
scheduler_update_node_resources(const char *node_name, 
    uint64_t memory_used, uint64_t cpu_used)
{
	pthread_mutex_lock(&scheduler_lock);
	
	for (int i = 0; i < node_count; i++) {
		if (nodes[i] != NULL && 
		    strcmp(nodes[i]->name, node_name) == 0) {
			nodes[i]->memory_used = memory_used;
			nodes[i]->cpu_used = cpu_used;
			nodes[i]->last_heartbeat = time(NULL);
			pthread_mutex_unlock(&scheduler_lock);
			return (0);
		}
	}
	
	pthread_mutex_unlock(&scheduler_lock);
	errno = ENOENT;
	return (-1);
}

/*
 * Get node information
 */
static struct node_info *
scheduler_get_node(const char *node_name)
{
	pthread_mutex_lock(&scheduler_lock);
	
	for (int i = 0; i < node_count; i++) {
		if (nodes[i] != NULL && 
		    strcmp(nodes[i]->name, node_name) == 0) {
			pthread_mutex_unlock(&scheduler_lock);
			return (nodes[i]);
		}
	}
	
	pthread_mutex_unlock(&scheduler_lock);
	return (NULL);
}

/*
 * Set scheduler scoring weights
 */
static int
scheduler_set_weights(int cpu_weight, int memory_weight, int pod_count_weight)
{
	pthread_mutex_lock(&scheduler_lock);
	scoring_weights.cpu_weight = cpu_weight;
	scoring_weights.memory_weight = memory_weight;
	scoring_weights.pod_count_weight = pod_count_weight;
	pthread_mutex_unlock(&scheduler_lock);
	return (0);
}
