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
 * Namespace management header
 */

#ifndef _OCIFBSD_NAMESPACE_H
#define _OCIFBSD_NAMESPACE_H

#include <sys/tree.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* Namespace states */
#define NS_STATE_ACTIVE     0
#define NS_STATE_SUSPENDED 1
#define NS_STATE_TERMINATING 2

/* Resource limits structure */
struct ns_resource_limits {
	uint64_t memory_limit;      /* bytes */
	uint64_t memory_reservation;/* bytes */
	uint64_t cpu_limit;         /* percent * 100 (e.g., 8000 = 80%) */
	uint64_t cpu_reservation;   /* percent * 100 */
	uint64_t processes_max;     /* max processes */
	uint64_t files_max;         /* max open files */
	uint64_t sockets_max;       /* max sockets */
	uint64_t vmemory_limit;     /* virtual memory limit */
	uint64_t stack_limit;       /* stack size limit */
	uint64_t coredump_limit;    /* core dump size */
};

/* Network policy */
struct ns_network_policy {
	bool allow_external;        /* allow external traffic */
	bool allow_internal;        /* allow inter-namespace traffic */
	bool egress_limit;         /* apply egress rate limiting */
	uint64_t egress_rate;       /* bytes per second */
	char *allowed_networks;    /* comma-separated CIDRs */
	char *denied_networks;      /* comma-separated CIDRs */
};

/* Volume access policy */
struct ns_volume_policy {
	bool allow_shared;          /* allow shared volumes */
	bool allow_exclusive;       /* allow exclusive volumes */
	bool allow_readonly;        /* allow read-only volumes */
	char **allowed_datasets;    /* allowed ZFS datasets */
	char **denied_datasets;     /* denied ZFS datasets */
};

/* Namespace structure */
struct namespace {
	char name[256];             /* namespace name */
	uint32_t id;                /* numeric namespace ID */
	int state;                  /* NS_STATE_* */
	time_t created;             /* creation timestamp */
	time_t updated;             /* last update timestamp */

	/* Resource limits */
	struct ns_resource_limits limits;

	/* Security */
	char mac_label[256];        /* MAC label, e.g. prod/high */
	int security_level;         /* securelevel */

	/* Network policy */
	struct ns_network_policy net_policy;

	/* Volume policy */
	struct ns_volume_policy vol_policy;

	/* Quota */
	uint64_t pod_limit;         /* max pods */
	uint64_t service_limit;     /* max services */
	uint64_t volume_limit;      /* max volumes */
	uint64_t secret_limit;      /* max secrets */

	/* Usage counters */
	uint32_t pod_count;
	uint32_t service_count;
	uint32_t volume_count;
	uint32_t secret_count;

	/* RBAC */
	char *allowed_users;        /* comma-separated users */
	char *allowed_groups;       /* comma-separated groups */

	/* Annotations */
	char **annotations;
	char **labels;

	/* Internal */
	RB_ENTRY(namespace) entry;
	pthread_mutex_t lock;
};

/* Namespace RB tree */
RB_HEAD(ns_tree, namespace);
RB_PROTOTYPE(ns_tree, namespace, entry, ns_compare);

/* Namespace operations */
struct namespace *ns_create(const char *name);
int ns_delete(struct namespace *ns);
int ns_update(struct namespace *ns);
struct namespace *ns_get(const char *name);
struct namespace **ns_list(int *count);

/* Resource operations */
int ns_set_limits(struct namespace *ns, struct ns_resource_limits *limits);
int ns_get_usage(struct namespace *ns, struct ns_resource_limits *usage);
int ns_check_quota(struct namespace *ns, const char *resource, uint32_t count);

/* Pod operations within namespace */
int ns_add_pod(struct namespace *ns, const char *pod_name);
int ns_remove_pod(struct namespace *ns, const char *pod_name);
int ns_list_pods(struct namespace *ns, char ***pods, int *count);

/* Network operations */
int ns_configure_network(struct namespace *ns, struct ns_network_policy *policy);
int ns_apply_firewall_rules(struct namespace *ns);

/* Volume operations */
int ns_configure_volumes(struct namespace *ns, struct ns_volume_policy *policy);
int ns_check_volume_access(struct namespace *ns, const char *dataset);

/* MAC operations */
int ns_set_mac_label(struct namespace *ns, const char *label);
int ns_apply_mac_label(struct namespace *ns);

/* Stats */
int ns_get_stats(struct namespace *ns, FILE *fp);

#endif /* _OCIFBSD_NAMESPACE_H */
