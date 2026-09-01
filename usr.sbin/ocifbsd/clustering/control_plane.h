/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Control-plane state machine.
 *
 * The desired cluster state -- which services exist, how many replicas each
 * wants, and which node each replica is placed on -- is a deterministic state
 * machine driven by commands committed to the Raft log. Every node applies the
 * same committed command sequence and converges on the same state, so the
 * control plane survives a leader change with no external coordination.
 *
 * Commands are plain text lines so they are easy to log, diff, and test:
 *
 *   CREATE   <service> <replicas> <image>
 *   SCALE    <service> <replicas>
 *   DELETE   <service>
 *   ASSIGN   <service> <replica_id> <node>
 *   UNASSIGN <service> <replica_id>
 *   ENDPOINT <service> <replica_id> <ip>
 *
 * cp_apply() is the state-machine transition applied to each committed entry.
 */

#ifndef OCIFBSD_CONTROL_PLANE_H
#define OCIFBSD_CONTROL_PLANE_H

struct cp_state;

/* Create/destroy an empty control-plane state. */
struct cp_state	*cp_new(void);
void		 cp_free(struct cp_state *st);

/*
 * Apply one committed command line to the state. Returns 0 on success, -1 on a
 * malformed or non-applicable command (the state is left unchanged on error).
 * Applying the same command sequence to two states yields identical states.
 */
int		 cp_apply(struct cp_state *st, const char *cmd);

/* Number of services currently defined. */
int		 cp_service_count(const struct cp_state *st);

/* Name of the i-th service (0..count-1), or NULL if out of range. */
const char	*cp_service_name(const struct cp_state *st, int i);

/*
 * Fill ids[] with the replica ids currently placed for a service (in ascending
 * insertion order, up to max). Returns the number written, or -1 on error.
 */
int		 cp_service_placements(const struct cp_state *st,
		    const char *svc, int *ids, int max);

/* Desired replica count for a service, or -1 if the service does not exist. */
int		 cp_service_replicas(const struct cp_state *st, const char *svc);

/* The image for a service, or NULL if it does not exist. */
const char	*cp_service_image(const struct cp_state *st, const char *svc);

/* The service's virtual IP (load-balancer front), or NULL if unset. */
const char	*cp_service_vip(const struct cp_state *st, const char *svc);

/* The node a replica is placed on, or NULL if unplaced/unknown. */
const char	*cp_replica_node(const struct cp_state *st, const char *svc,
		    int replica_id);

/* A replica's reported endpoint IP, or NULL if unplaced/not yet reported. */
const char	*cp_replica_endpoint(const struct cp_state *st,
		    const char *svc, int replica_id);

/*
 * Fill ips[][64] with the reported endpoint IPs of a service's placed replicas
 * (those with an endpoint), up to max. Returns the count. These are the pf
 * load-balancer backends for the service VIP.
 */
int		 cp_service_endpoints(const struct cp_state *st,
		    const char *svc, char ips[][64], int max);

/* Number of replicas of any service currently placed on a node. */
int		 cp_node_replica_count(const struct cp_state *st,
		    const char *node);

/* Total number of placements (across all services and nodes). */
int		 cp_placement_count(const struct cp_state *st);

/*
 * Read the i-th placement (0..cp_placement_count-1) into the caller's buffers
 * (any of svc/id/node may be NULL). Returns 0 on success, -1 if out of range.
 */
int		 cp_placement_at(const struct cp_state *st, int i, char *svc,
		    size_t svclen, int *id, char *node, size_t nodelen);

#endif /* OCIFBSD_CONTROL_PLANE_H */
