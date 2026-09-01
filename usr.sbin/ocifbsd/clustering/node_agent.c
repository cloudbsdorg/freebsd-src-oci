/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Node agent: assignment wire format and reconciliation. See node_agent.h.
 */

#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "node_agent.h"

int
agent_marshal(const struct agent_replica *reps, int n, char *buf, size_t buflen)
{
	size_t off = 0;

	if (reps == NULL || buf == NULL || n < 0 || buflen == 0)
		return (-1);
	buf[0] = '\0';
	for (int i = 0; i < n; i++) {
		int w = snprintf(buf + off, buflen - off, "%s %d %s\n",
		    reps[i].service, reps[i].replica_id, reps[i].image);
		if (w < 0 || (size_t)w >= buflen - off)
			return (-1);
		off += (size_t)w;
	}
	return (0);
}

int
agent_unmarshal(const char *buf, struct agent_replica *out, int max, int *nout)
{
	char *copy, *line, *save = NULL;
	int n = 0;

	if (buf == NULL || out == NULL || nout == NULL || max <= 0)
		return (-1);
	copy = strdup(buf);
	if (copy == NULL)
		return (-1);

	for (line = strtok_r(copy, "\n", &save); line != NULL;
	    line = strtok_r(NULL, "\n", &save)) {
		char svc[128], img[512];
		int id;

		if (sscanf(line, "%127s %d %511s", svc, &id, img) != 3)
			continue;			/* skip blank/short lines */
		if (n >= max) {
			free(copy);
			return (-1);
		}
		memset(&out[n], 0, sizeof(out[n]));
		strlcpy(out[n].service, svc, sizeof(out[n].service));
		out[n].replica_id = id;
		strlcpy(out[n].image, img, sizeof(out[n].image));
		n++;
	}
	free(copy);
	*nout = n;
	return (0);
}

/* Same replica identity (service + replica_id), ignoring image. */
static int
same_replica(const struct agent_replica *a, const struct agent_replica *b)
{
	return (a->replica_id == b->replica_id &&
	    strcmp(a->service, b->service) == 0);
}

static const struct agent_replica *
find_replica(const struct agent_replica *set, int n,
    const struct agent_replica *want)
{
	for (int i = 0; i < n; i++)
		if (same_replica(&set[i], want))
			return (&set[i]);
	return (NULL);
}

int
agent_reconcile(const struct agent_replica *desired, int nd,
    const struct agent_replica *running, int nr,
    struct agent_action *out, int max, int *nout)
{
	int k = 0;

	if (out == NULL || nout == NULL || nd < 0 || nr < 0)
		return (-1);

#define PUSH(o, rep)						\
	do {							\
		if (k >= max)					\
			return (-1);				\
		out[k].op = (o);				\
		out[k].replica = (rep);				\
		k++;						\
	} while (0)

	/* Launches and image-change relaunches. */
	for (int i = 0; i < nd; i++) {
		const struct agent_replica *r =
		    find_replica(running, nr, &desired[i]);

		if (r == NULL) {
			PUSH(AGENT_LAUNCH, desired[i]);
		} else if (strcmp(r->image, desired[i].image) != 0) {
			PUSH(AGENT_STOP, *r);
			PUSH(AGENT_LAUNCH, desired[i]);
		}
	}

	/* Stops for running replicas that are no longer desired. */
	for (int j = 0; j < nr; j++)
		if (find_replica(desired, nd, &running[j]) == NULL)
			PUSH(AGENT_STOP, running[j]);

#undef PUSH

	*nout = k;
	return (0);
}

/* Build "<service>-<replica_id>" into buf. Returns 0 on success. */
static int
replica_name(const struct agent_replica *r, char *buf, size_t buflen)
{
	int w = snprintf(buf, buflen, "%s-%d", r->service, r->replica_id);

	return (w > 0 && (size_t)w < buflen) ? 0 : -1;
}

int
agent_launch_argv(const struct agent_replica *r, const char *ocifbsd,
    char *namebuf, size_t namelen, const char *argv[], int max, int *argc)
{
	int k = 0;

	if (r == NULL || ocifbsd == NULL || namebuf == NULL || argv == NULL ||
	    argc == NULL || max < 7)
		return (-1);
	if (replica_name(r, namebuf, namelen) != 0)
		return (-1);
	argv[k++] = ocifbsd;
	argv[k++] = "run";
	argv[k++] = "--name";
	argv[k++] = namebuf;
	argv[k++] = "--image";
	argv[k++] = r->image;
	argv[k] = NULL;
	*argc = k;
	return (0);
}

int
agent_stop_argv(const struct agent_replica *r, const char *ocifbsd,
    char *namebuf, size_t namelen, const char *argv[], int max, int *argc)
{
	int k = 0;

	if (r == NULL || ocifbsd == NULL || namebuf == NULL || argv == NULL ||
	    argc == NULL || max < 5)
		return (-1);
	if (replica_name(r, namebuf, namelen) != 0)
		return (-1);
	argv[k++] = ocifbsd;
	argv[k++] = "delete";
	argv[k++] = namebuf;
	argv[k++] = "--force";
	argv[k] = NULL;
	*argc = k;
	return (0);
}

/* Run a NULL-terminated argv to completion. Returns the child's exit status. */
static int
run_argv(const char *argv[])
{
	pid_t pid = fork();
	int status;

	if (pid < 0)
		return (-1);
	if (pid == 0) {
		execv(argv[0], (char *const *)argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return (-1);
	return (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

/*
 * Apply one action against the local runtime: LAUNCH runs the replica's jail;
 * STOP gracefully stops then deletes it. ocifbsd is the path to the ocifbsd(8)
 * binary. Returns 0 on success, non-zero on failure.
 */
int
agent_apply_action(const struct agent_action *a, const char *ocifbsd)
{
	char name[192];
	const char *argv[8];
	int argc;

	if (a == NULL || ocifbsd == NULL)
		return (-1);

	if (a->op == AGENT_LAUNCH) {
		if (agent_launch_argv(&a->replica, ocifbsd, name, sizeof(name),
		    argv, 8, &argc) != 0)
			return (-1);
		return (run_argv(argv));
	}

	/* STOP: a best-effort graceful stop, then delete --force. */
	if (replica_name(&a->replica, name, sizeof(name)) != 0)
		return (-1);
	{
		const char *stop_argv[] = { ocifbsd, "stop", name, NULL };
		(void)run_argv(stop_argv);
	}
	if (agent_stop_argv(&a->replica, ocifbsd, name, sizeof(name),
	    argv, 8, &argc) != 0)
		return (-1);
	return (run_argv(argv));
}
