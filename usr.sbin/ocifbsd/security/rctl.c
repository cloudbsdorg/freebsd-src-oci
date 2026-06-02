/*-
 * Copyright (c) 2024 The FreeBSD Foundation
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
 * FreeBSD OCI Runtime - Resource Limits (RCTL)
 */

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rctl.h"

/*
 * RCTL resource name mapping
 */
static const char *rctl_resource_names[] = {
	[RCTL_RESOURCE_NPROC]		= "nproc",
	[RCTL_RESOURCE_OPENFILES]	= "openfiles",
	[RCTL_RESOURCE_VMEM]		= "vmemoryuse",
	[RCTL_RESOURCE_STACK]		= "stacksize",
	[RCTL_RESOURCE_CORE]		= "coredumpsize",
	[RCTL_RESOURCE_CPU]		= "cputime",
	[RCTL_RESOURCE_WALLTIME]	= "wallclock",
	[RCTL_RESOURCE_MEMORYUSE]	= "memoryuse",
	[RCTL_RESOURCE_MEMORYLOCKED]	= "memorylocked",
	[RCTL_RESOURCE_PHYSPAGES]	= "physpages",
	[RCTL_RESOURCE_FSIZE]		= "filesize",
	[RCTL_RESOURCE_SOCKBUF]	= "socketbuffer",
	[RCTL_RESOURCE_NOFILE]		= "nofiles",
};

static const int rctl_resource_sysctls[] = {
	[RCTL_RESOURCE_NPROC]		= CTL_KERN, KERN_PROC, KERN_PROC_NTHREADS,
	[RCTL_RESOURCE_MEMORYUSE]	= -1,  /* jail_get */
	[RCTL_RESOURCE_VMEM]		= -1,
	[RCTL_RESOURCE_OPENFILES]	= -1,
	[RCTL_RESOURCE_NOFILE]		= RLIMIT_NOFILE,
	[RCTL_RESOURCE_NPROC]		= RLIMIT_NPROC,
	[RCTL_RESOURCE_FSIZE]		= RLIMIT_FSIZE,
	[RCTL_RESOURCE_STACK]		= RLIMIT_STACK,
	[RCTL_RESOURCE_CORE]		= RLIMIT_CORE,
};

/*
 * Check if RCTL is available
 */
int
rctl_check_available(void)
{
	int rctl_available;

	size_t len = sizeof(rctl_available);
	if (sysctlbyname("security.jail.rctl_available", &rctl_available, &len,
	    NULL, 0) != 0) {
		return (0);  /* RCTL not available */
	}

	return (rctl_available);
}

/*
 * Initialize RCTL subsystem
 */
int
rctl_init(void)
{
	if (!rctl_check_available()) {
		fprintf(stderr, "warning: RCTL not available\n");
		return (-1);
	}

	return (0);
}

/*
 * Parse size string (e.g., "1G", "512M", "1K")
 */
uint64_t
rctl_parse_size(const char *size_str)
{
	uint64_t size;
	char *end;

	if (size_str == NULL || size_str[0] == '\0')
		return (0);

	size = strtoull(size_str, &end, 10);

	switch (*end) {
	case 'G':
	case 'g':
		size *= 1024 * 1024 * 1024;
		break;
	case 'M':
	case 'm':
		size *= 1024 * 1024;
		break;
	case 'K':
	case 'k':
		size *= 1024;
		break;
	case 'T':
	case 't':
		size *= 1024ULL * 1024 * 1024 * 1024;
		break;
	}

	return (size);
}

/*
 * Format size to human-readable string
 */
const char *
rctl_format_size(uint64_t size)
{
	static char buf[32];

	if (size >= 1024ULL * 1024 * 1024 * 1024)
		snprintf(buf, sizeof(buf), "%lluT", (unsigned long long)(size / (1024ULL * 1024 * 1024 * 1024)));
	else if (size >= 1024ULL * 1024 * 1024)
		snprintf(buf, sizeof(buf), "%lluM", (unsigned long long)(size / (1024ULL * 1024)));
	else if (size >= 1024ULL)
		snprintf(buf, sizeof(buf), "%lluK", (unsigned long long)(size / 1024));
	else
		snprintf(buf, sizeof(buf), "%llu", (unsigned long long)size);

	return (buf);
}

/*
 * Get resource name
 */
const char *
rctl_resource_name(rctl_resource_t resource)
{
	if (resource < 0 || resource >= sizeof(rctl_resource_names) / sizeof(rctl_resource_names[0]))
		return ("unknown");

	return (rctl_resource_names[resource]);
}

/*
 * Parse resource name
 */
rctl_resource_t
rctl_parse_resource(const char *name)
{
	int i;

	for (i = 0; i < sizeof(rctl_resource_names) / sizeof(rctl_resource_names[0]); i++) {
		if (rctl_resource_names[i] && strcmp(rctl_resource_names[i], name) == 0)
			return ((rctl_resource_t)i);
	}

	return ((rctl_resource_t)-1);
}

/*
 * Parse action name
 */
rctl_action_t
rctl_parse_action(const char *name)
{
	if (strcmp(name, "deny") == 0)
		return (RCTL_ACTION_DENY);
	if (strcmp(name, "log") == 0)
		return (RCTL_ACTION_LOG);
	if (strcmp(name, "siginfo") == 0)
		return (RCTL_ACTION_SIGINFO);
	if (strcmp(name, "sigterm") == 0)
		return (RCTL_ACTION_SIGTERM);
	if (strcmp(name, "sigkill") == 0)
		return (RCTL_ACTION_SIGKILL);

	return (RCTL_ACTION_DENY);
}

/*
 * Run rctl command
 */
static int
run_rctl(int argc, ...)
{
	va_list ap;
	char **argv;
	pid_t pid;
	int status;

	argv = calloc(argc + 1, sizeof(char *));
	if (argv == NULL)
		return (-1);

	va_start(ap, argc);
	for (int i = 0; i < argc; i++)
		argv[i] = va_arg(ap, char *);
	va_end(ap);
	argv[argc] = NULL;

	pid = fork();
	if (pid < 0) {
		free(argv);
		return (-1);
	}

	if (pid == 0) {
		closefrom(STDERR_FILENO + 1);
		execvp("rctl", argv);
		_exit(127);
	}

	free(argv);
	if (waitpid(pid, &status, 0) < 0)
		return (-1);

	if (WIFEXITED(status))
		return (WEXITSTATUS(status));

	return (-1);
}

/*
 * Apply resource limits to a jail
 */
int
rctl_apply_rules(const char *jail_name, struct rctl_limits *limits)
{
	char rule[512];
	char action[32];
	int ret = 0;

	if (limits == NULL)
		return (0);

	/* Set default action */
	strlcpy(action, "deny", sizeof(action));

	/* Apply memory limits */
	if (limits->memory_limit > 0) {
		snprintf(rule, sizeof(rule), "-j %s resource=%s "
		    "memoryuse=resident=%llu,action=%s",
		    jail_name,
		    rctl_resource_name(RCTL_RESOURCE_MEMORYUSE),
		    (unsigned long long)limits->memory_limit / 4096,  /* pages */
		    action);

		ret |= run_rctl(6, "rctl", "add", "resource", rule);
	}

	/* Apply process limits */
	if (limits->proc_limit > 0) {
		snprintf(rule, sizeof(rule), "-j %s resource=nproc "
		    "max=%llu,action=%s",
		    jail_name,
		    (unsigned long long)limits->proc_limit,
		    action);

		ret |= run_rctl(6, "rctl", "add", "resource", rule);
	}

	/* Apply file limits */
	if (limits->file_limit > 0) {
		snprintf(rule, sizeof(rule), "-j %s resource=openfiles "
		    "max=%llu,action=%s",
		    jail_name,
		    (unsigned long long)limits->file_limit,
		    action);

		ret |= run_rctl(6, "rctl", "add", "resource", rule);
	}

	/* Apply CPU limits via jail parameter */
	if (limits->cpu_shares > 0 || limits->cpu_quota > 0) {
		char cmd[256];

		/* Set via jail command */
		if (limits->cpu_quota > 0) {
			snprintf(cmd, sizeof(cmd), "jail -r %s "
			    "cputime=%llu",
			    jail_name,
			    (unsigned long long)limits->cpu_quota);
			system(cmd);
		}
	}

	return (ret);
}

/*
 * Remove all resource limits for a jail
 */
int
rctl_remove_rules(const char *jail_name)
{
	return (run_rctl(5, "rctl", "-j", jail_name, "remove"));
}

/*
 * Get current rules for a jail
 */
int
rctl_get_rules(const char *jail_name, struct rctl_rule **rules, int *nrules)
{
	char *output = NULL;
	char *line, *save;
	struct rctl_rule *list = NULL;
	int count = 0;

	*rules = NULL;
	*nrules = 0;

	FILE *fp = popen("rctl -j testjail", "r");
	if (fp == NULL)
		return (-1);

	while (fgets((char[256]){0}, sizeof(char[256]), fp) != NULL) {
		struct rctl_rule *r;

		list = realloc(list, (count + 1) * sizeof(*list));
		r = &list[count];

		r->jail_name = strdup(jail_name);
		/* TODO: parse the output properly */

		count++;
	}

	pclose(fp);

	*rules = list;
	*nrules = count;

	return (0);
}

/*
 * Set a single limit
 */
int
rctl_set_limit(const char *jail_name, rctl_resource_t resource,
    uint64_t limit, rctl_action_t action)
{
	char rule[512];
	char action_str[32];

	switch (action) {
	case RCTL_ACTION_DENY:
		strlcpy(action_str, "deny", sizeof(action_str));
		break;
	case RCTL_ACTION_LOG:
		strlcpy(action_str, "log", sizeof(action_str));
		break;
	case RCTL_ACTION_SIGINFO:
		strlcpy(action_str, "siginfo", sizeof(action_str));
		break;
	default:
		strlcpy(action_str, "deny", sizeof(action_str));
	}

	snprintf(rule, sizeof(rule), "-j %s resource=%s "
	    "max=%llu,action=%s",
	    jail_name,
	    rctl_resource_name(resource),
	    (unsigned long long)limit,
	    action_str);

	return (run_rctl(6, "rctl", "add", "resource", rule));
}

/*
 * Remove a limit
 */
int
rctl_remove_limit(const char *jail_name, rctl_resource_t resource)
{
	return (run_rctl(6, "rctl", "-j", jail_name, "-r",
	    rctl_resource_name(resource)));
}

/*
 * Get current limit
 */
int
rctl_get_limit(const char *jail_name, rctl_resource_t resource, uint64_t *limit)
{
	char cmd[256];
	char *output = NULL;
	int ret = -1;

	snprintf(cmd, sizeof(cmd), "rctl -j %s resource=%s",
	    jail_name, rctl_resource_name(resource));

	FILE *fp = popen(cmd, "r");
	if (fp == NULL)
		return (-1);

	char buf[256];
	if (fgets(buf, sizeof(buf), fp) != NULL) {
		/* Parse limit from output */
		/* TODO: proper parsing */
		*limit = 0;
		ret = 0;
	}

	pclose(fp);

	return (ret);
}

/*
 * Get resource usage
 */
int
rctl_get_usage(const char *jail_name, rctl_resource_t resource, uint64_t *usage)
{
	char cmd[256];
	char buf[256];
	int ret = -1;

	snprintf(cmd, sizeof(cmd), "rctl -j %s resource=%s",
	    jail_name, rctl_resource_name(resource));

	FILE *fp = popen(cmd, "r");
	if (fp == NULL)
		return (-1);

	/* TODO: parse usage from rctl output */
	*usage = 0;
	ret = 0;

	pclose(fp);

	return (ret);
}

/*
 * Get all resource usage for a jail
 */
int
rctl_get_all_usage(const char *jail_name, struct rctl_usage **usage, int *nusage)
{
	struct rctl_usage *list = NULL;
	int count = 0;

	*usage = NULL;
	*nusage = 0;

	/* TODO: implement using jailctl or rctl */

	*usage = list;
	*nusage = count;

	return (0);
}

/*
 * Check if any limits are exceeded
 */
int
rctl_check_limits(const char *jail_name, bool *exceeded, char **message)
{
	/* TODO: check current usage against limits */

	*exceeded = false;
	*message = NULL;

	return (0);
}

/*
 * Parse OCI Linux resources JSON
 */
int
rctl_parse_oci_resources(struct rctl_limits *limits, const char *oci_json)
{
	/* TODO: parse OCI Linux resources structure */

	if (limits == NULL)
		return (-1);

	memset(limits, 0, sizeof(*limits));

	/* TODO: actually parse the JSON */

	return (0);
}
