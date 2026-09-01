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
 * FreeBSD OCI Runtime - Resource Limits (RCTL)
 */

#ifndef _OCIFBSD_RCTL_H
#define _OCIFBSD_RCTL_H

#include <sys/types.h>
#include <stdbool.h>

/*
 * Resource limit types (mapped from OCI Linux resources)
 */
typedef enum {
	RCTL_RESOURCE_NPROC = 0,	/* max processes */
	RCTL_RESOURCE_OPENFILES,	/* max files open */
	RCTL_RESOURCE_VMEM,		/* virtual memory */
	RCTL_RESOURCE_STACK,		/* stack size */
	RCTL_RESOURCE_CORE,		/* core dump size */
	RCTL_RESOURCE_CPU,		/* CPU time (seconds) */
	RCTL_RESOURCE_WALLTIME,		/* wall clock time */
	RCTL_RESOURCE_MEMORYUSE,	/* memory use */
	RCTL_RESOURCE_MEMORYLOCKED,	/* locked memory */
	RCTL_RESOURCE_PHYSPAGES,	/* physical pages */
	RCTL_RESOURCE_FSIZE,		/* file size */
	RCTL_RESOURCE_SOCKBUF,		/* socket buffer */
	RCTL_RESOURCE_NOFILE,		/* number of files */
} rctl_resource_t;

/*
 * Resource action on limit
 */
typedef enum {
	RCTL_ACTION_DENY = 0,		/* deny the allocation */
	RCTL_ACTION_LOG,			/* log but allow */
	RCTL_ACTION_SIGINFO,		/* send SIGINFO */
	RCTL_ACTION_SIGTERM,		/* send SIGTERM */
	RCTL_ACTION_SIGKILL,		/* send SIGKILL */
} rctl_action_t;

/*
 * Resource limit rule
 */
struct rctl_rule {
	char		*jail_name;	/* jail name */
	rctl_resource_t	resource;	/* resource type */
	char		*resource_name;	/* string name */
	uint64_t	limit;		/* limit value */
	rctl_action_t	action;		/* action on limit */
	char		*signal;	/* signal name if action=signal */
};

/*
 * Resource limits configuration (from OCI spec)
 */
struct rctl_limits {
	/* CPU limits */
	uint64_t	cpu_shares;		/* relative weight */
	uint64_t	cpu_quota;		/* CPU quota (microseconds) */
	uint64_t	cpu_period;		/* CPU period (microseconds) */
	uint64_t	cpu_rt_runtime;		/* real-time runtime */
	uint64_t	cpu_rt_period;		/* real-time period */

	/* Memory limits */
	uint64_t	memory_limit;		/* memory limit (bytes) */
	uint64_t	memory_reservation;	/* soft limit */
	uint64_t	memory_swap;		/* swap limit */
	bool		memory_oom_kill_disable; /* disable OOM kill */

	/* Process limits */
	uint64_t	proc_limit;		/* max processes */

	/* File limits */
	uint64_t	file_limit;		/* max files */

	/* BlkIO */
	uint64_t	blkio_weight;		/* block I/O weight */
	uint64_t	blkio_read_bps;		/* read bytes per second */
	uint64_t	blkio_write_bps;	/* write bytes per second */
	uint64_t	blkio_read_iops;	/* read IOPS */
	uint64_t	blkio_write_iops;	/* write IOPS */
};

/*
 * RCTL operations
 */
int	 rctl_init(void);
int	 rctl_apply_rules(const char *jail_name, struct rctl_limits *limits);
int	 rctl_remove_rules(const char *jail_name);
int	 rctl_get_rules(const char *jail_name, struct rctl_rule **rules,
	     int *nrules);
int	 rctl_check_available(void);

/*
 * Individual resource operations
 */
int	 rctl_set_limit(const char *jail_name, rctl_resource_t resource,
	     uint64_t limit, rctl_action_t action);
int	 rctl_remove_limit(const char *jail_name, rctl_resource_t resource);
int	 rctl_get_limit(const char *jail_name, rctl_resource_t resource,
	     uint64_t *limit);
int	 rctl_get_usage(const char *jail_name, rctl_resource_t resource,
	     uint64_t *usage);

/*
 * Parse OCI Linux resources to FreeBSD RCTL
 */
int	 rctl_parse_oci_resources(struct rctl_limits *limits,
	     const char *oci_json);

/*
 * Utility functions
 */
const char *rctl_resource_name(rctl_resource_t resource);
rctl_resource_t rctl_parse_resource(const char *name);
rctl_action_t rctl_parse_action(const char *name);
uint64_t rctl_parse_size(const char *size_str);
const char *rctl_format_size(uint64_t size);

/*
 * Resource monitoring
 */
struct rctl_usage {
	char		*jail_name;	/* jail the rule applies to */
	rctl_resource_t	resource;	/* resource type */
	char		*resource_name;	/* string form of resource */
	uint64_t	usage;		/* current usage (maxuse from rctl) */
	uint64_t	limit;		/* configured limit */
	bool		exceeded;	/* usage > limit */
};

int	 rctl_get_all_usage(const char *jail_name, struct rctl_usage **usage,
	     int *nusage);
int	 rctl_check_limits(const char *jail_name, bool *exceeded,
	     char **message);

#endif /* _OCIFBSD_RCTL_H */
