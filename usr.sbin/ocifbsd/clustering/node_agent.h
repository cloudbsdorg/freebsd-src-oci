/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Node agent: the component that runs a node's assigned replicas.
 *
 * The controller sends each node, over the mTLS control channel, the set of
 * replicas that should be running there. The agent reconciles that desired set
 * against what is actually running and launches or stops jails to match. This
 * header covers the two pure, deterministic pieces -- the wire format for an
 * assignment set and the reconciliation that turns (desired, running) into a
 * list of launch/stop actions -- so they can be unit-tested without a live
 * runtime. The actual jail create/start/stop is applied from the action list.
 */

#ifndef OCIFBSD_NODE_AGENT_H
#define OCIFBSD_NODE_AGENT_H

#include <stddef.h>

/* One replica assigned to (or running on) this node. */
struct agent_replica {
	char	service[128];
	int	replica_id;
	char	image[512];
};

enum agent_op {
	AGENT_LAUNCH,	/* create + start this replica's jail */
	AGENT_STOP	/* stop + delete this replica's jail */
};

struct agent_action {
	enum agent_op		op;
	struct agent_replica	replica;
};

/*
 * Serialize an assignment set to a text blob (one "service replica_id image"
 * line per replica) for transport. Returns 0 on success, -1 if the buffer is
 * too small or on bad arguments.
 */
int	agent_marshal(const struct agent_replica *reps, int n, char *buf,
	    size_t buflen);

/*
 * Parse an assignment blob produced by agent_marshal back into an array (up to
 * max entries). *nout gets the count. Returns 0 on success, -1 on error.
 */
int	agent_unmarshal(const char *buf, struct agent_replica *out, int max,
	    int *nout);

/*
 * Compute the actions needed to move the node from its running set to the
 * desired set, matching replicas by (service, replica_id):
 *   - a desired replica not running (or running a different image) -> LAUNCH
 *     (a running replica with a different image is first STOPped);
 *   - a running replica not desired -> STOP.
 * Writes up to max actions to out and sets *nout. Returns 0 on success, -1 if
 * out is too small or on bad arguments.
 */
int	agent_reconcile(const struct agent_replica *desired, int nd,
	    const struct agent_replica *running, int nr,
	    struct agent_action *out, int max, int *nout);

/*
 * A replica's container name is "<service>-<replica_id>". These build the
 * argv to launch or stop it via the local ocifbsd(8) runtime:
 *   launch: ocifbsd run --name <name> --image <image>
 *   stop:   ocifbsd delete <name> --force   (after a graceful stop)
 * namebuf receives the container name (argv points into it); argv is filled
 * with the NULL-terminated command and *argc gets the argument count. Returns
 * 0 on success, -1 on bad arguments or too-small buffers.
 */
int	agent_launch_argv(const struct agent_replica *r, const char *ocifbsd,
	    char *namebuf, size_t namelen, const char *argv[], int max,
	    int *argc);
int	agent_stop_argv(const struct agent_replica *r, const char *ocifbsd,
	    char *namebuf, size_t namelen, const char *argv[], int max,
	    int *argc);

/*
 * Apply one reconcile action against the local ocifbsd(8) runtime (LAUNCH runs
 * the replica's jail; STOP gracefully stops then deletes it). ocifbsd is the
 * path to the ocifbsd binary. Returns 0 on success, non-zero on failure.
 */
int	agent_apply_action(const struct agent_action *a, const char *ocifbsd);

#endif /* OCIFBSD_NODE_AGENT_H */
