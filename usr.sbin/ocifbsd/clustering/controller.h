/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Controller planner: the leader's reconciliation from desired state to
 * placements. Given the replicated control-plane state (services and their
 * desired replica counts) and the set of schedulable nodes, it computes the
 * ASSIGN/UNASSIGN commands that place every desired replica on a node --
 * spreading them by current load -- and unplace replicas beyond the desired
 * count. The leader proposes these commands back into the Raft log, so the
 * placements are themselves replicated and every node's agent can read its
 * slice. The planner is pure and deterministic, so it is unit-testable and
 * converges (re-planning an already-satisfied state yields no commands).
 */

#ifndef OCIFBSD_CONTROLLER_H
#define OCIFBSD_CONTROLLER_H

#include "control_plane.h"
#include "node_agent.h"

/*
 * Build the assignment set for a single node: the replicas currently placed on
 * `node` (with each service's image), as an agent_replica array to marshal and
 * send to that node's agent over the mTLS channel. Writes up to max entries to
 * out and sets *nout. Returns 0 on success, -1 on error.
 */
int	controller_node_assignments(const struct cp_state *st, const char *node,
	    struct agent_replica *out, int max, int *nout);

/*
 * Build the pf(4) load-balancer ruleset for a service from its replicated
 * endpoints: a round-robin redirect from vip:port to the pool of reported
 * replica endpoints (each at backend_port). Written to out (NUL-terminated).
 * With no endpoints, out holds only a comment (no rdr rule). Returns 0 on
 * success, -1 on bad arguments or a too-small buffer.
 */
int	controller_lb_ruleset(const struct cp_state *st, const char *service,
	    const char *vip, int port, int backend_port, char *out,
	    size_t outlen);

/*
 * Derive ENDPOINT commands from placements: for each placed replica that has
 * no endpoint yet, emit "ENDPOINT <service> <id> <node-address>" using the
 * names[]/addrs[] node map. The leader proposes these so the load-balancer
 * pool is populated from placement without any agent round-trip (host-reachable
 * replicas answer at their node address). Idempotent. Returns 0 on success.
 */
int	controller_endpoint_commands(const struct cp_state *st,
	    const char *const *names, const char *const *addrs, int nnodes,
	    char (*out)[256], int max, int *nout);

/*
 * Compute the placement commands to converge st toward its desired replica
 * counts across nodes[0..nnodes-1]. Each command (a control-plane command line
 * such as "ASSIGN web 0 node1") is written to out[k] (each at least 256 bytes);
 * *nout gets the count. Returns 0 on success, -1 if out is too small or on bad
 * arguments. With no nodes, only UNASSIGN commands (scale-down) are produced.
 */
int	controller_plan(const struct cp_state *st, const char *const *nodes,
	    int nnodes, char (*out)[256], int max, int *nout);

#endif /* OCIFBSD_CONTROLLER_H */
