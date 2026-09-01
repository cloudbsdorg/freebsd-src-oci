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
#include "node_agent.h"

#define CTRL_MAX_NODES	256

int
controller_lb_ruleset(const struct cp_state *st, const char *service,
    const char *vip, int port, int backend_port, char *out, size_t outlen)
{
	char ips[CTRL_MAX_NODES][64];
	size_t off = 0;
	int n, w;

	if (st == NULL || service == NULL || vip == NULL || out == NULL ||
	    outlen == 0)
		return (-1);

	n = cp_service_endpoints(st, service, ips, CTRL_MAX_NODES);

	w = snprintf(out, outlen,
	    "# ocifbsd load balancer for service %s (VIP %s)\n", service, vip);
	if (w < 0 || (size_t)w >= outlen)
		return (-1);
	off = (size_t)w;

	if (n == 0) {
		w = snprintf(out + off, outlen - off,
		    "# no endpoints reported yet; no redirect installed\n");
		return (w > 0 && (size_t)w < outlen - off) ? 0 : -1;
	}

	w = snprintf(out + off, outlen - off,
	    "rdr pass proto tcp from any to %s port %d -> {", vip, port);
	if (w < 0 || (size_t)w >= outlen - off)
		return (-1);
	off += (size_t)w;
	for (int i = 0; i < n; i++) {
		w = snprintf(out + off, outlen - off, " %s", ips[i]);
		if (w < 0 || (size_t)w >= outlen - off)
			return (-1);
		off += (size_t)w;
	}
	w = snprintf(out + off, outlen - off, " } port %d round-robin\n",
	    backend_port);
	return (w > 0 && (size_t)w < outlen - off) ? 0 : -1;
}

int
controller_vip_commands(const struct cp_state *st, const char *prefix,
    char (*out)[256], int max, int *nout)
{
	int k = 0, ns;

	if (st == NULL || prefix == NULL || out == NULL || nout == NULL)
		return (-1);

	ns = cp_service_count(st);
	for (int i = 0; i < ns; i++) {
		const char *name = cp_service_name(st, i);

		if (name == NULL || cp_service_vip(st, name) != NULL)
			continue;
		if (k >= max)
			return (-1);
		snprintf(out[k], 256, "VIP %s %s.%d", name, prefix, 10 + i);
		k++;
	}
	*nout = k;
	return (0);
}

int
controller_endpoint_commands(const struct cp_state *st,
    const char *const *names, const char *const *addrs, int nnodes,
    char (*out)[256], int max, int *nout)
{
	int k = 0, npl;

	if (st == NULL || names == NULL || addrs == NULL || out == NULL ||
	    nout == NULL)
		return (-1);

	npl = cp_placement_count(st);
	for (int i = 0; i < npl; i++) {
		char svc[128], node[256];
		const char *addr = NULL;
		int id;

		if (cp_placement_at(st, i, svc, sizeof(svc), &id, node,
		    sizeof(node)) != 0)
			continue;
		if (cp_replica_endpoint(st, svc, id) != NULL)
			continue;		/* already has an endpoint */
		for (int j = 0; j < nnodes; j++)
			if (strcmp(names[j], node) == 0) {
				addr = addrs[j];
				break;
			}
		if (addr == NULL)
			continue;		/* node address unknown */
		if (k >= max)
			return (-1);
		snprintf(out[k], 256, "ENDPOINT %s %d %s", svc, id, addr);
		k++;
	}
	*nout = k;
	return (0);
}

int
controller_node_assignments(const struct cp_state *st, const char *node,
    struct agent_replica *out, int max, int *nout)
{
	int k = 0, npl;

	if (st == NULL || node == NULL || out == NULL || nout == NULL)
		return (-1);

	npl = cp_placement_count(st);
	for (int i = 0; i < npl; i++) {
		char svc[128], n[256];
		int id;
		const char *img;

		if (cp_placement_at(st, i, svc, sizeof(svc), &id, n,
		    sizeof(n)) != 0)
			continue;
		if (strcmp(n, node) != 0)
			continue;
		if (k >= max)
			return (-1);
		memset(&out[k], 0, sizeof(out[k]));
		strlcpy(out[k].service, svc, sizeof(out[k].service));
		out[k].replica_id = id;
		img = cp_service_image(st, svc);
		if (img != NULL)
			strlcpy(out[k].image, img, sizeof(out[k].image));
		k++;
	}
	*nout = k;
	return (0);
}

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
