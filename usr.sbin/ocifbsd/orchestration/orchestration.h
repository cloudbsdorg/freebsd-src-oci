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
 * OCI FreeBSD Orchestration - Core types and interfaces
 */

#ifndef _OCIFBSD_ORCHESTRATION_H
#define _OCIFBSD_ORCHESTRATION_H

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/*
 * Configuration paths
 */
#define OCIFBSD_ORCH_STATE_DIR	"/var/run/ocifbsd/orchestration"
#define OCIFBSD_ORCH_CONFIG_DIR	"/etc/ocifbsd/orchestration"
#define OCIFBSD_ORCH_VAR_DIR	"/var/lib/ocifbsd/orchestration"

/*
 * Pod states
 */
typedef enum {
	POD_STATE_PENDING = 0,
	POD_STATE_RUNNING,
	POD_STATE_SUCCEEDED,
	POD_STATE_FAILED,
	POD_STATE_UNKNOWN
} pod_state_t;

/*
 * Service replica states
 */
typedef enum {
	REPLICA_STATE_PENDING = 0,
	REPLICA_STATE_STARTING,
	REPLICA_STATE_RUNNING,
	REPLICA_STATE_FAILED,
	REPLICA_STATE_TERMINATING,
	REPLICA_STATE_TERMINATED
} replica_state_t;

/*
 * Rolling update strategy
 */
typedef enum {
	ROLLING_STRATEGY_RECREATE = 0,	/* Delete all, then create all */
	ROLLING_STRATEGY_ROLLING,	/* Rolling update (default) */
	ROLLING_STRATEGY_BLUE_GREEN	/* Blue-green deployment */
} rolling_strategy_t;

/*
 * Health check types
 */
typedef enum {
	HEALTH_CHECK_NONE = 0,
	HEALTH_CHECK_TCP,
	HEALTH_CHECK_HTTP,
	HEALTH_CHECK_EXEC,
	HEALTH_CHECK_SHELL
} health_check_type_t;

/*
 * Port mapping for services
 */
struct port_mapping {
	uint16_t	container_port;	/* Port inside the container */
	uint16_t	host_port;	/* Port on the host (0 = assign random) */
	char		protocol[8];	/* tcp, udp */
	char		host_ip[64];	/* Bind address */
};

/*
 * Resource requirements
 */
struct resource_requirements {
	uint64_t	memory_limit;		/* bytes */
	uint64_t	memory_request;		/* bytes */
	int		cpu_shares;		/* relative weight */
	int		cpu_limit;		/* percent * 1000 (e.g., 2000 = 2 cores) */
	uint64_t	storage_limit;		/* bytes */
	int		max_open_files;
	int		max_processes;
	int		max_locked_memory;
};

/*
 * Container specification within a pod
 */
struct container_spec {
	char		name[256];
	char		image[512];
	char		command[1024];
	char		args[2048];
	char		workdir[PATH_MAX];
	char		*env[64];	/* environment variables */
	int		nenv;
	struct resource_requirements resources;
	struct port_mapping *ports;
	int		nports;
	char		*volumes[32];	/* volume mounts */
	int		nvolumes;
	char		*networks[16];
	int		nnetworks;
	char		*dns_servers[8];
	int		ndns_servers;
	char		hostname[256];
	bool		host_network;
	bool		privileged;
	char		*user;
	char		*group;
	char		*seccomp_profile;
	char		*mac_label;
};

/*
 * Pod specification
 */
struct pod_spec {
	char		name[256];
	char		namespace[128];
	char		labels[512];	/* JSON: {"key": "value", ...} */
	char		annotations[1024];
	char		*node_selector;
	char		*affinity;
	bool		host_ipc;
	bool		host_network;
	bool		host_pid;
	char		*service_account;
	char		*image_pull_secrets;
	struct resource_requirements resources;
	struct container_spec *containers;
	int		ncontainers;
	char		*volumes[16];	/* configmaps, secrets, etc. */
	int		nvolumes;
};

/*
 * Pod status
 */
struct pod_status {
	char		uid[64];
	char		name[256];
	char		namespace[128];
	pod_state_t	state;
	time_t		created;
	time_t		started;
	time_t		finished;
	char		host_ip[64];
	char		pod_ip[64];
	char		node[256];
	int		restart_count;
	char		reason[256];
	char		message[512];
	struct container_status {
		char		name[256];
		replica_state_t state;
		int		restart_count;
		time_t		last_restart;
		int		exit_code;
		char		image[512];
		char		container_id[128];	/* 64-hex id + NUL (was 64: truncated the id, breaking teardown) */
	} *containers;
	int		ncontainers;
};

/*
 * Pod handle (opaque reference)
 */
struct pod {
	char		uid[64];
	char		name[256];
	char		namespace[128];
	char		*state_file;
	struct pod_spec *spec;
	struct pod_status *status;
};

/*
 * Stack specification
 */
struct stack_spec {
	char		name[256];
	char		namespace[128];
	char		version[32];
	char		labels[512];
	char		*depends_on;	/* comma-separated list */
	char		*networks[16];
	int		nnetworks;
	char		*volumes[16];
	int		nvolumes;
	char		*configs[16];
	int		nconfigs;
	char		*secrets[16];
	int		nsecrets;
	struct service_spec *services;
	int		nservices;
};

/*
 * Stack status
 */
struct stack_status {
	char		name[256];
	char		namespace[128];
	char		state[64];
	time_t		created;
	time_t		updated;
	int		nrunning;
	int		ntotal;
	int		nfailed;
};

/*
 * Stack handle
 */
struct stack {
	char		name[256];
	char		namespace[128];
	char		*state_file;
	struct stack_spec *spec;
	struct stack_status *status;
};

/*
 * Service specification
 */
struct service_spec {
	char		name[256];
	char		stack[256];	/* parent stack name */
	char		image[512];
	char		command[1024];
	char		args[2048];
	char		*env[64];
	int		nenv;
	struct resource_requirements resources;
	struct port_mapping *ports;
	int		nports;
	int		replicas;	/* desired replica count */
	char		*volumes[32];
	int		nvolumes;
	char		*networks[16];
	int		nnetworks;
	char		placement[512];	/* node selectors, constraints */

	/* Health check configuration */
	struct health_check {
		health_check_type_t type;
		int		initial_delay;	/* seconds */
		int		period;		/* seconds */
		int		timeout;	/* seconds */
		int		success_threshold;	/* consecutive successes */
		int		failure_threshold;	/* consecutive failures */
		char		path[512];	/* for HTTP checks */
		char		command[1024];	/* for exec checks */
		int		port;		/* for TCP checks */
	} health_check;

	/* Rolling update configuration */
	struct {
		rolling_strategy_t strategy;
		int		max_surge;	/* extra replicas during update */
		int		max_unavailable;	/* unavailable replicas */
		int		timeout;	/* seconds per replica */
		char		*failure_policy;	/* pause, rollback, continue */
	} update_config;

	/* Scaling configuration */
	struct {
		int		min_replicas;
		int		max_replicas;
		char		*metrics;	/* JSON metrics config */
		int		target_cpu_percent;
	} scaling_config;

	/* Network configuration */
	struct {
		char		lb_algorithm[32];	/* roundrobin, leastconn, iphash */
		bool		headless;
		char		domain[256];
		char		*session_affinity;	/* none, clientip */
	} network_config;
};

/*
 * Service status
 */
struct service_status {
	char		name[256];
	char		namespace[128];
	char		stack[256];
	int		desired_replicas;
	int		available_replicas;
	int		ready_replicas;
	int		updated_replicas;
	char		load_balancer_ip[64];
	char		external_lb[512];
	time_t		created;
	time_t		updated;
};

/*
 * Service replica
 */
struct service_replica {
	char		uid[64];
	char		name[256];	/* service-X-replica-Y */
	char		service[256];
	int		replica_id;
	char		pod_name[256];
	replica_state_t state;
	int		restarts;
	time_t		started;
	char		node[256];
	char		pod_ip[64];
};

/*
 * Service handle
 */
struct service {
	char		name[256];
	char		namespace[128];
	char		stack[256];
	char		*state_file;
	struct service_spec *spec;
	struct service_status *status;
	struct service_replica *replicas;
	int		nreplicas;
};

/*
 * Scheduler decision
 */
struct scheduling_decision {
	char		node[256];
	double		score;
	char		reason[256];
	char		*failed_reason;	/* why node was rejected */
};

/*
 * Rolling update state
 */
struct rolling_update_state {
	char		service[256];
	char		namespace[128];
	int		total_replicas;
	int		updated_replicas;
	int		available_replicas;
	int		ready_replicas;
	char		strategy[32];
	time_t		started;
	time_t		completed;
	char		status[64];	/* running, paused, completed, failed */
	int		current_surge;
	int		current_unavailable;
};

/*
 * Orchestration initialization
 */
int	orch_init(void);
void	orch_shutdown(void);

/*
 * Orchestration CLI dispatch
 */
int	orch_cli_dispatch(int argc, char **argv);

/*
 * Event system
 */
struct orch_event {
	char		type[64];
	char		object[128];
	char		namespace[128];
	char		message[512];
	time_t		timestamp;
};

typedef void (*orch_event_callback_t)(const struct orch_event *event, void *arg);

int	orch_event_subscribe(orch_event_callback_t callback, void *arg);
int	orch_event_unsubscribe(int subscription_id);
int	orch_event_publish(const char *type, const char *object,
	const char *namespace, const char *message, ...);
struct orch_event **orch_event_list(const char *namespace, int *count);

/*
 * Pod operations
 */
struct pod	*pod_create(struct pod_spec *spec);
int		pod_start(struct pod *pod);
int		pod_stop(struct pod *pod, int sig);
int		pod_delete(struct pod *pod);
struct pod	*pod_get(const char *name, const char *namespace);
struct pod	**pod_list(const char *namespace, int *count);
void		pod_free(struct pod *pod);
struct pod_status *pod_get_status(struct pod *pod);
int		pod_update(struct pod *pod, struct pod_spec *new_spec);
int		pod_logs(struct pod *pod, const char *container, int tail, bool follow);

/*
 * Stack operations
 */
struct stack	*stack_create(struct stack_spec *spec);
int		stack_start(struct stack *stack);
int		stack_stop(struct stack *stack);
int		stack_delete(struct stack *stack);
struct stack	*stack_get(const char *name, const char *namespace);
struct stack	**stack_list(const char *namespace, int *count);
void		stack_free(struct stack *stack);
int		stack_update(struct stack *stack, struct stack_spec *new_spec);

/*
 * Service operations
 */
struct service	*service_create(struct service_spec *spec);
int		service_start(struct service *service);
int		service_stop(struct service *service);
int		service_delete(struct service *service);
struct service	*service_get(const char *name, const char *namespace);
struct service	**service_list(const char *namespace, int *count);
void		service_free(struct service *service);
int		service_scale(struct service *service, int replicas);
int		service_update(struct service *service, struct service_spec *new_spec);
/* Persist a service's state (name/image/replicas/replica pod names) to disk. */
int		save_service_state(struct service *service);
int		service_rollback(struct service *service);
struct service_replica **service_get_replicas(struct service *service, int *count);
struct service_status *service_get_status(struct service *service);

/*
 * Scheduler operations
 */
int		scheduler_init(void);
struct scheduling_decision *scheduler_select_node(struct pod_spec *spec);
int		scheduler_score_node(const char *node, struct pod_spec *spec);
char		**scheduler_list_nodes(int *count);
int		scheduler_add_node(const char *node);
int		scheduler_set_node_schedulable(const char *node, bool schedulable);

/*
 * A minimal node descriptor used to feed cluster membership into the
 * scheduler without coupling it to the clustering module. A thin glue layer
 * builds this array from cluster_nodes_list() and calls scheduler_sync_nodes().
 */
struct sched_node {
	char	name[256];
	char	address[64];
};

/*
 * Reconcile the scheduler's node set with the cluster: nodes in the array are
 * (re)registered as ready and schedulable; previously-synced nodes no longer
 * present are retired (marked not ready / not schedulable). The local node is
 * never retired. Returns 0 on success, -1 on bad arguments.
 */
int		scheduler_sync_nodes(const struct sched_node *nodes, int count);
int		scheduler_remove_node(const char *node);
int		scheduler_node_ready(const char *node);
int		scheduler_node_not_ready(const char *node);

/*
 * Health checker operations
 */
int		health_checker_init(void);
void		health_checker_shutdown(void);
int		health_check_start(struct service *service);
int		health_check_stop(struct service *service);
int		health_check_run(struct service *service, const char *replica_name);
int		health_check_get_status(struct service *service, const char *replica_name);

/*
 * Rolling update operations
 */
int		rolling_update_init(struct service *service, struct service_spec *new_spec);
int		rolling_update_pause(struct rolling_update_state *state);
int		rolling_update_resume(struct rolling_update_state *state);
int		rolling_update_rollback(struct rolling_update_state *state);
struct rolling_update_state *rolling_update_get_status(const char *service,
	const char *namespace);
int		rolling_update_complete(struct rolling_update_state *state);

/*
 * State persistence
 */
int	orch_save_state(void);
int	orch_load_state(void);

/* Old event callbacks removed - use new orch_event_* functions */

/*
 * Validate a pod/service/stack name or namespace before it is used as a path
 * component under the root-owned state dir (rejects '/', "..", NUL, leading
 * '.'/'-'). Defined in pod.c; used by the path builders and CLI handlers.
 */
bool	orch_name_is_valid(const char *s);

/*
 * Upper bound on service replicas. Replica counts drive calloc() and a
 * fork/jail launch loop, so they must be clamped to avoid a resource-
 * exhaustion bomb from an unbounded (or negative, via atoi) value.
 */
#define ORCH_MAX_REPLICAS	4096

#endif /* _OCIFBSD_ORCHESTRATION_H */
