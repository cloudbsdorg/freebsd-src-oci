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
 * FreeBSD OCI Runtime - Resource Limits (RCTL)
 */

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <errno.h>
#include <stdarg.h>
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

/*
 * Check if RCTL is available
 */
int
rctl_check_available(void)
{
	int enabled;
	size_t len = sizeof(enabled);

	/*
	 * RCTL is usable only when RACCT accounting is compiled in AND enabled.
	 * The signal for both is the kern.racct.enable sysctl: it is absent when
	 * the kernel lacks "options RACCT"/"options RCTL", and 0 when present but
	 * disabled (kern.racct.enable=1 is a boot-time tunable). The previously
	 * queried "security.jail.rctl_available" oid does not exist on FreeBSD,
	 * so this always reported unavailable and no limits were ever applied.
	 */
	if (sysctlbyname("kern.racct.enable", &enabled, &len, NULL, 0) != 0)
		return (0);  /* RACCT/RCTL not compiled in */

	return (enabled != 0);
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
		snprintf(buf, sizeof(buf), "%lluG", (unsigned long long)(size / (1024ULL * 1024 * 1024)));
	else if (size >= 1024ULL * 1024)
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
	const size_t nnames = sizeof(rctl_resource_names) /
	    sizeof(rctl_resource_names[0]);

	if (resource < 0 || (size_t)resource >= nnames)
		return ("unknown");

	return (rctl_resource_names[resource]);
}

/*
 * Parse resource name
 */
rctl_resource_t
rctl_parse_resource(const char *name)
{
	size_t i;
	const size_t nnames = sizeof(rctl_resource_names) /
	    sizeof(rctl_resource_names[0]);

	for (i = 0; i < nnames; i++) {
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

static int
run_rctl_output(char **output, int argc, ...)
{
	va_list ap;
	char **argv;
	int pipefd[2];
	pid_t pid;
	int status;
	char buf[4096];
	ssize_t n;
	size_t total = 0;
	char *result = NULL;

	*output = NULL;

	argv = calloc(argc + 1, sizeof(char *));
	if (argv == NULL)
		return (-1);

	va_start(ap, argc);
	for (int i = 0; i < argc; i++)
		argv[i] = va_arg(ap, char *);
	va_end(ap);
	argv[argc] = NULL;

	if (pipe(pipefd) != 0) {
		free(argv);
		return (-1);
	}

	pid = fork();
	if (pid < 0) {
		close(pipefd[0]);
		close(pipefd[1]);
		free(argv);
		return (-1);
	}

	if (pid == 0) {
		close(pipefd[0]);
		dup2(pipefd[1], STDOUT_FILENO);
		close(pipefd[1]);
		closefrom(STDERR_FILENO + 1);
		execvp("rctl", argv);
		_exit(127);
	}

	close(pipefd[1]);
	free(argv);

	while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
		char *new_result = realloc(result, total + (size_t)n + 1);
		if (new_result == NULL) {
			free(result);
			close(pipefd[0]);
			waitpid(pid, &status, 0);
			return (-1);
		}
		result = new_result;
		memcpy(result + total, buf, (size_t)n);
		total += (size_t)n;
	}
	close(pipefd[0]);

	if (result != NULL)
		result[total] = '\0';

	waitpid(pid, &status, 0);

	*output = result;
	return (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

/*
 * Convert an OCI CPU quota/period (both in microseconds) to an RCTL pcpu
 * percentage. pcpu is a whole-percent share of a single core and may exceed
 * 100 across multiple cores. An unspecified period uses the OCI default of
 * 100000us; any positive quota floors at 1% so a small request is never
 * rounded down to zero (which rctl(8) would treat as unlimited).
 */
static uint64_t
rctl_quota_to_pcpu(uint64_t quota, uint64_t period)
{
	uint64_t pcpu;

	if (quota == 0)
		return (0);
	if (period == 0)
		period = 100000;
	pcpu = (quota * 100) / period;
	if (pcpu == 0)
		pcpu = 1;
	return (pcpu);
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

	/*
	 * Apply limits with the correct rctl(8) rule syntax and a matching
	 * argument count. Rules are "jail:<name>:<resource>:<action>=<amount>"
	 * added with `rctl -a <rule>`. The previous calls passed argc=6 with
	 * only 4 arguments (reading two garbage va_arg pointers into the
	 * execvp argv) and used an invalid "add resource -j ..." form, so the
	 * limits silently never applied.
	 */
	if (limits->memory_limit > 0) {
		snprintf(rule, sizeof(rule), "jail:%s:%s:%s=%llu",
		    jail_name,
		    rctl_resource_name(RCTL_RESOURCE_MEMORYUSE),
		    action,
		    (unsigned long long)limits->memory_limit);

		ret |= run_rctl(3, "rctl", "-a", rule);
	}

	/* Apply process limits */
	if (limits->proc_limit > 0) {
		snprintf(rule, sizeof(rule), "jail:%s:nproc:%s=%llu",
		    jail_name, action,
		    (unsigned long long)limits->proc_limit);

		ret |= run_rctl(3, "rctl", "-a", rule);
	}

	/* Apply file limits */
	if (limits->file_limit > 0) {
		snprintf(rule, sizeof(rule), "jail:%s:openfiles:%s=%llu",
		    jail_name, action,
		    (unsigned long long)limits->file_limit);

		ret |= run_rctl(3, "rctl", "-a", rule);
	}

	/*
	 * Apply CPU limits via rctl pcpu (no shell; jail_name is untrusted).
	 * OCI expresses CPU as cpu.quota / cpu.period microseconds; RCTL pcpu is
	 * a whole-percent share of one core (and may exceed 100 across multiple
	 * cores). Convert quota/period to a percentage rather than passing the
	 * raw microsecond quota — the previous code applied e.g. 50000 as pcpu,
	 * which is not a meaningful CPU percentage. An unspecified period uses
	 * the OCI default of 100000us, and any positive request floors at 1% so
	 * a small quota is never rounded down to "unlimited".
	 */
	if (limits->cpu_quota > 0) {
		uint64_t pcpu = rctl_quota_to_pcpu(limits->cpu_quota,
		    limits->cpu_period);

		snprintf(rule, sizeof(rule), "jail:%s:pcpu:%s=%llu",
		    jail_name, action, (unsigned long long)pcpu);
		ret |= run_rctl(3, "rctl", "-a", rule);
	}

	return (ret);
}

/*
 * Remove all resource limits for a jail
 */
int
rctl_remove_rules(const char *jail_name)
{
	char filter[256];

	/* Remove every rule whose subject is this jail: `rctl -r jail:<name>`.
	 * The old call passed argc=5 with 4 args and an invalid "remove"
	 * verb, reading a garbage va_arg pointer. */
	snprintf(filter, sizeof(filter), "jail:%s", jail_name);
	return (run_rctl(3, "rctl", "-r", filter));
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
	char type[64], action[32], maxuse[32], pct[16], limit[32];

	*rules = NULL;
	*nrules = 0;

	if (run_rctl_output(&output, 3, "rctl", "-j", jail_name) != 0) {
		free(output);
		return (-1);
	}

	line = strtok_r(output, "\n", &save);
	while (line != NULL) {
		struct rctl_rule *r;

		if (sscanf(line, "%63s %31s %31s %15s %31s",
		    type, action, maxuse, pct, limit) != 5)
			goto next_line;

		if (strcmp(type, "type") == 0)
			goto next_line;

		void *_new_r = realloc(list, (count + 1) * sizeof(*list));
		if (_new_r == NULL) goto next_line;
		list = _new_r;
		r = &list[count];

		r->jail_name = strdup(jail_name);
		r->resource = rctl_parse_resource(type);
		r->resource_name = strdup(type);
		r->limit = strtoull(limit, NULL, 10);

		if (strcmp(action, "deny") == 0)
			r->action = RCTL_ACTION_DENY;
		else if (strcmp(action, "log") == 0)
			r->action = RCTL_ACTION_LOG;
		else if (strcmp(action, "siginfo") == 0)
			r->action = RCTL_ACTION_SIGINFO;
		else if (strcmp(action, "sigterm") == 0)
			r->action = RCTL_ACTION_SIGTERM;
		else if (strcmp(action, "sigkill") == 0)
			r->action = RCTL_ACTION_SIGKILL;
		else
			r->action = RCTL_ACTION_DENY;

		r->signal = (r->action >= RCTL_ACTION_SIGINFO) ?
		    strdup(action) : NULL;

		count++;
next_line:
		line = strtok_r(NULL, "\n", &save);
	}

	free(output);

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

	snprintf(rule, sizeof(rule), "jail:%s:%s:%s=%llu",
	    jail_name,
	    rctl_resource_name(resource),
	    action_str,
	    (unsigned long long)limit);

	return (run_rctl(3, "rctl", "-a", rule));
}

/*
 * Remove a limit
 */
int
rctl_remove_limit(const char *jail_name, rctl_resource_t resource)
{
	char filter[256];

	/* `rctl -r jail:<name>:<resource>` (argc matches the 3 args). */
	snprintf(filter, sizeof(filter), "jail:%s:%s", jail_name,
	    rctl_resource_name(resource));
	return (run_rctl(3, "rctl", "-r", filter));
}

/*
 * Get current limit
 */
int
rctl_get_limit(const char *jail_name, rctl_resource_t resource, uint64_t *limit)
{
	char *output = NULL;
	char *line, *save;
	char type[64], action[32], maxuse[32], pct[16], limitstr[32];
	int ret = -1;

	*limit = 0;

	if (run_rctl_output(&output, 4, "rctl", "-j", jail_name,
	    rctl_resource_name(resource)) != 0) {
		free(output);
		return (-1);
	}

	line = strtok_r(output, "\n", &save);
	while (line != NULL) {
		if (sscanf(line, "%63s %31s %31s %15s %31s",
		    type, action, maxuse, pct, limitstr) == 5 &&
		    strcmp(type, "type") != 0) {
			*limit = strtoull(limitstr, NULL, 10);
			ret = 0;
			break;
		}
		line = strtok_r(NULL, "\n", &save);
	}

	free(output);
	return (ret);
}

/*
 * Get resource usage
 */
int
rctl_get_usage(const char *jail_name, rctl_resource_t resource, uint64_t *usage)
{
	char *output = NULL;
	char *line, *save;
	char type[64], action[32], maxuse[32], pct[16], limitstr[32];
	int ret = -1;

	*usage = 0;

	if (run_rctl_output(&output, 4, "rctl", "-j", jail_name,
	    rctl_resource_name(resource)) != 0) {
		free(output);
		return (-1);
	}

	line = strtok_r(output, "\n", &save);
	while (line != NULL) {
		if (sscanf(line, "%63s %31s %31s %15s %31s",
		    type, action, maxuse, pct, limitstr) == 5 &&
		    strcmp(type, "type") != 0) {
			*usage = strtoull(maxuse, NULL, 10);
			ret = 0;
			break;
		}
		line = strtok_r(NULL, "\n", &save);
	}

	free(output);
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
	char *output = NULL;
	char *line, *save;
	char type[64], action[32], maxuse[32], pct[16], limitstr[32];

	*usage = NULL;
	*nusage = 0;

	if (run_rctl_output(&output, 3, "rctl", "-j", jail_name) != 0) {
		free(output);
		return (-1);
	}

	line = strtok_r(output, "\n", &save);
	while (line != NULL) {
		struct rctl_usage *u;

		if (sscanf(line, "%63s %31s %31s %15s %31s",
		    type, action, maxuse, pct, limitstr) != 5)
			goto next_line;

		if (strcmp(type, "type") == 0)
			goto next_line;

		void *_new_u = realloc(list, (count + 1) * sizeof(*list));
		if (_new_u == NULL) goto next_line;
		list = _new_u;
		u = &list[count];

		u->jail_name = strdup(jail_name);
		u->resource = rctl_parse_resource(type);
		u->resource_name = strdup(type);
		u->usage = strtoull(maxuse, NULL, 10);
		u->limit = strtoull(limitstr, NULL, 10);
		u->exceeded = (u->usage > u->limit);

		count++;
next_line:
		line = strtok_r(NULL, "\n", &save);
	}

	free(output);

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
	struct rctl_usage *usage = NULL;
	int nusage = 0;
	int i;
	int any = 0;
	char *msg = NULL;
	size_t msg_len = 0, msg_cap = 0;

	*exceeded = false;
	*message = NULL;

	if (rctl_get_all_usage(jail_name, &usage, &nusage) != 0)
		return (-1);

	for (i = 0; i < nusage; i++) {
		if (usage[i].exceeded) {
			any = 1;
			char line[256];
			int n = snprintf(line, sizeof(line),
			    "%s: %llu > %llu\n",
			    usage[i].resource_name,
			    (unsigned long long)usage[i].usage,
			    (unsigned long long)usage[i].limit);
			if (n > 0) {
				if (msg_len + (size_t)n + 1 > msg_cap) {
					msg_cap = msg_cap ? msg_cap * 2 : 256;
					char *new_msg = realloc(msg, msg_cap);
					if (new_msg == NULL) {
						free(msg);
						free(usage);
						return (-1);
					}
					msg = new_msg;
				}
				memcpy(msg + msg_len, line, (size_t)n);
				msg_len += (size_t)n;
				msg[msg_len] = '\0';
			}
		}
	}

	for (i = 0; i < nusage; i++) {
		free(usage[i].jail_name);
		free(usage[i].resource_name);
	}
	free(usage);

	*exceeded = (any != 0);
	*message = msg;

	return (0);
}

/*
 * Parse a single number from a JSON value (best-effort, no json-c).
 */
static uint64_t
parse_json_number(const char *json, const char *key)
{
	char pattern[128];
	const char *p, *end;
	uint64_t result = 0;

	if (json == NULL || key == NULL)
		return (0);

	snprintf(pattern, sizeof(pattern), "\"%s\"", key);
	p = strstr(json, pattern);
	if (p == NULL)
		return (0);
	p += strlen(pattern);
	while (*p == ' ' || *p == '\t' || *p == ':')
		p++;
	if (*p < '0' || *p > '9')
		return (0);
	end = p;
	while (*end >= '0' && *end <= '9')
		end++;
	char numbuf[32];
	size_t n = (size_t)(end - p);
	if (n >= sizeof(numbuf))
		n = sizeof(numbuf) - 1;
	memcpy(numbuf, p, n);
	numbuf[n] = '\0';
	result = strtoull(numbuf, NULL, 10);

	return (result);
}

/*
 * Parse OCI Linux resources JSON
 */
int
rctl_parse_oci_resources(struct rctl_limits *limits, const char *oci_json)
{
	if (limits == NULL)
		return (-1);

	memset(limits, 0, sizeof(*limits));

	if (oci_json == NULL)
		return (0);

	limits->cpu_shares = parse_json_number(oci_json, "shares");
	limits->cpu_quota = parse_json_number(oci_json, "quota");
	limits->cpu_period = parse_json_number(oci_json, "period");
	limits->cpu_rt_runtime = parse_json_number(oci_json, "rtRuntime");
	limits->cpu_rt_period = parse_json_number(oci_json, "rtPeriod");

	limits->memory_limit = parse_json_number(oci_json, "limit");
	limits->memory_reservation = parse_json_number(oci_json, "reservation");
	limits->memory_swap = parse_json_number(oci_json, "swap");
	limits->memory_oom_kill_disable =
	    parse_json_number(oci_json, "disableOOMKiller") != 0;

	limits->proc_limit = parse_json_number(oci_json, "pidsLimit");

	limits->blkio_weight = parse_json_number(oci_json, "weight");
	limits->blkio_read_bps = parse_json_number(oci_json, "throttleReadBpsDevice");
	limits->blkio_write_bps = parse_json_number(oci_json, "throttleWriteBpsDevice");
	limits->blkio_read_iops = parse_json_number(oci_json, "throttleReadIOPSDevice");
	limits->blkio_write_iops = parse_json_number(oci_json, "throttleWriteIOPSDevice");

	return (0);
}
