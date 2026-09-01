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
