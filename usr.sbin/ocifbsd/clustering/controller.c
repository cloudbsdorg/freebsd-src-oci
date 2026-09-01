/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Controller planner: desired replica counts -> placement commands, spreading
 * replicas across nodes by current load. See controller.h.
 */

#include <stdio.h>
#include <string.h>

#include "control_plane.h"
#include "controller.h"

#define CTRL_MAX_NODES	256

int
controller_plan(const struct cp_state *st, const char *const *nodes,
    int nnodes, char (*out)[256], int max, int *nout)
{
	int load[CTRL_MAX_NODES];
	int k = 0, nsvc;

	if (st == NULL || out == NULL || nout == NULL || nnodes < 0)
		return (-1);
	if (nnodes > CTRL_MAX_NODES)
		nnodes = CTRL_MAX_NODES;

	/* Seed each node's load with the replicas already placed on it. */
	for (int j = 0; j < nnodes; j++)
		load[j] = cp_node_replica_count(st, nodes[j]);

	nsvc = cp_service_count(st);
	for (int s = 0; s < nsvc; s++) {
		const char *name = cp_service_name(st, s);
		int desired = cp_service_replicas(st, name);
		int ids[CTRL_MAX_NODES];
		int np;

		/* Assign each desired replica that isn't placed yet. */
		for (int id = 0; id < desired; id++) {
			int best;

			if (cp_replica_node(st, name, id) != NULL)
				continue;		/* already placed */
			if (nnodes <= 0)
				continue;		/* nowhere to place */
			best = 0;
			for (int j = 1; j < nnodes; j++)
				if (load[j] < load[best])
					best = j;
			if (k >= max)
				return (-1);
			snprintf(out[k], 256, "ASSIGN %s %d %s", name, id,
			    nodes[best]);
			k++;
			load[best]++;
		}

		/* Unplace any placed replica beyond the desired count. */
		np = cp_service_placements(st, name, ids, CTRL_MAX_NODES);
		for (int i = 0; i < np; i++) {
			if (ids[i] < desired)
				continue;
			if (k >= max)
				return (-1);
			snprintf(out[k], 256, "UNASSIGN %s %d", name, ids[i]);
			k++;
		}
	}

	*nout = k;
	return (0);
}
