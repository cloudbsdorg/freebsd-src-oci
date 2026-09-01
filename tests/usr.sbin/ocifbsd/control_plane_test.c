/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Unit tests for the control-plane state machine (clustering/control_plane.c).
 */

#include <atf-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "clustering/control_plane.c"

/* CREATE / SCALE / DELETE move a service's desired replica count. */
ATF_TC_WITHOUT_HEAD(cp_service_lifecycle);
ATF_TC_BODY(cp_service_lifecycle, tc)
{
	struct cp_state *s = cp_new();

	ATF_REQUIRE(s != NULL);
	ATF_CHECK_EQ(-1, cp_service_replicas(s, "web"));

	ATF_REQUIRE_EQ(0, cp_apply(s, "CREATE web 3 nginx:1.27"));
	ATF_CHECK_EQ(3, cp_service_replicas(s, "web"));
	ATF_CHECK_STREQ("nginx:1.27", cp_service_image(s, "web"));
	ATF_CHECK_EQ(1, cp_service_count(s));

	ATF_REQUIRE_EQ(0, cp_apply(s, "SCALE web 5"));
	ATF_CHECK_EQ(5, cp_service_replicas(s, "web"));

	ATF_REQUIRE_EQ(0, cp_apply(s, "DELETE web"));
	ATF_CHECK_EQ(-1, cp_service_replicas(s, "web"));
	ATF_CHECK_EQ(0, cp_service_count(s));
	cp_free(s);
}

/* ASSIGN / UNASSIGN place replicas on nodes. */
ATF_TC_WITHOUT_HEAD(cp_placement);
ATF_TC_BODY(cp_placement, tc)
{
	struct cp_state *s = cp_new();

	ATF_REQUIRE(s != NULL);
	ATF_REQUIRE_EQ(0, cp_apply(s, "CREATE web 2 nginx"));
	ATF_REQUIRE_EQ(0, cp_apply(s, "ASSIGN web 0 node1"));
	ATF_REQUIRE_EQ(0, cp_apply(s, "ASSIGN web 1 node2"));
	ATF_CHECK_STREQ("node1", cp_replica_node(s, "web", 0));
	ATF_CHECK_STREQ("node2", cp_replica_node(s, "web", 1));
	ATF_CHECK_EQ(1, cp_node_replica_count(s, "node1"));
	ATF_CHECK_EQ(1, cp_node_replica_count(s, "node2"));

	/* Reassigning moves the replica. */
	ATF_REQUIRE_EQ(0, cp_apply(s, "ASSIGN web 1 node1"));
	ATF_CHECK_STREQ("node1", cp_replica_node(s, "web", 1));
	ATF_CHECK_EQ(2, cp_node_replica_count(s, "node1"));
	ATF_CHECK_EQ(0, cp_node_replica_count(s, "node2"));

	ATF_REQUIRE_EQ(0, cp_apply(s, "UNASSIGN web 0"));
	ATF_CHECK(cp_replica_node(s, "web", 0) == NULL);
	cp_free(s);
}

/* Deleting a service also drops its placements. */
ATF_TC_WITHOUT_HEAD(cp_delete_drops_placements);
ATF_TC_BODY(cp_delete_drops_placements, tc)
{
	struct cp_state *s = cp_new();

	ATF_REQUIRE(s != NULL);
	ATF_REQUIRE_EQ(0, cp_apply(s, "CREATE web 1 nginx"));
	ATF_REQUIRE_EQ(0, cp_apply(s, "ASSIGN web 0 node1"));
	ATF_REQUIRE_EQ(0, cp_apply(s, "DELETE web"));
	ATF_CHECK_EQ(0, cp_node_replica_count(s, "node1"));
	ATF_CHECK(cp_replica_node(s, "web", 0) == NULL);
	cp_free(s);
}

/* Malformed commands are rejected and leave the state unchanged. */
ATF_TC_WITHOUT_HEAD(cp_rejects_bad_commands);
ATF_TC_BODY(cp_rejects_bad_commands, tc)
{
	struct cp_state *s = cp_new();

	ATF_REQUIRE(s != NULL);
	ATF_REQUIRE_EQ(0, cp_apply(s, "CREATE web 3 nginx"));
	ATF_CHECK(cp_apply(s, "BOGUS web") != 0);
	ATF_CHECK(cp_apply(s, "CREATE") != 0);
	ATF_CHECK(cp_apply(s, "SCALE nonexistent 4") != 0);
	ATF_CHECK(cp_apply(s, "") != 0);
	ATF_CHECK_EQ(3, cp_service_replicas(s, "web"));	/* unchanged */
	cp_free(s);
}

/* Determinism: two states applying the same log converge identically. */
ATF_TC_WITHOUT_HEAD(cp_deterministic_convergence);
ATF_TC_BODY(cp_deterministic_convergence, tc)
{
	const char *log[] = {
		"CREATE web 3 nginx", "CREATE db 1 postgres",
		"ASSIGN web 0 n1", "ASSIGN web 1 n2", "ASSIGN web 2 n3",
		"SCALE web 4", "ASSIGN web 3 n1", "ASSIGN db 0 n2",
	};
	struct cp_state *a = cp_new();
	struct cp_state *b = cp_new();

	ATF_REQUIRE(a != NULL && b != NULL);
	for (size_t i = 0; i < sizeof(log) / sizeof(log[0]); i++) {
		ATF_REQUIRE_EQ(0, cp_apply(a, log[i]));
		ATF_REQUIRE_EQ(0, cp_apply(b, log[i]));
	}
	ATF_CHECK_EQ(cp_service_replicas(a, "web"), cp_service_replicas(b, "web"));
	ATF_CHECK_EQ(cp_node_replica_count(a, "n1"), cp_node_replica_count(b, "n1"));
	ATF_CHECK_STREQ(cp_replica_node(a, "web", 3), cp_replica_node(b, "web", 3));
	ATF_CHECK_EQ(2, cp_node_replica_count(a, "n1"));	/* web-0, web-3 */
	cp_free(a);
	cp_free(b);
}

/*
 * Stress: many services and placements. Exercises the state machine's growth
 * and lookups at scale and confirms it stays correct under load.
 */
ATF_TC_WITHOUT_HEAD(cp_stress_scale);
ATF_TC_BODY(cp_stress_scale, tc)
{
	const int NSVC = 500;
	const int NNODE = 6;
	struct cp_state *s = cp_new();
	char cmd[256];

	ATF_REQUIRE(s != NULL);

	/* Create NSVC services, each with 4 replicas spread over NNODE nodes. */
	for (int i = 0; i < NSVC; i++) {
		snprintf(cmd, sizeof(cmd), "CREATE svc%d 4 img:%d", i, i);
		ATF_REQUIRE_EQ(0, cp_apply(s, cmd));
		for (int r = 0; r < 4; r++) {
			snprintf(cmd, sizeof(cmd), "ASSIGN svc%d %d node%d",
			    i, r, (i * 4 + r) % NNODE);
			ATF_REQUIRE_EQ(0, cp_apply(s, cmd));
		}
	}

	ATF_CHECK_EQ(NSVC, cp_service_count(s));

	/* Every placement landed: 4*NSVC replicas across NNODE nodes evenly. */
	int total = 0;
	for (int n = 0; n < NNODE; n++) {
		char node[32];
		snprintf(node, sizeof(node), "node%d", n);
		total += cp_node_replica_count(s, node);
	}
	ATF_CHECK_EQ(NSVC * 4, total);

	/* Spot-check a few services survived intact. */
	ATF_CHECK_EQ(4, cp_service_replicas(s, "svc0"));
	ATF_CHECK_EQ(4, cp_service_replicas(s, "svc499"));
	ATF_CHECK(cp_service_image(s, "svc250") != NULL);

	/* Scale and delete half, verify consistency. */
	for (int i = 0; i < NSVC; i += 2) {
		snprintf(cmd, sizeof(cmd), "DELETE svc%d", i);
		ATF_REQUIRE_EQ(0, cp_apply(s, cmd));
	}
	ATF_CHECK_EQ(NSVC / 2, cp_service_count(s));
	total = 0;
	for (int n = 0; n < NNODE; n++) {
		char node[32];
		snprintf(node, sizeof(node), "node%d", n);
		total += cp_node_replica_count(s, node);
	}
	ATF_CHECK_EQ((NSVC / 2) * 4, total);	/* deleted services' placements gone */
	cp_free(s);
}

/* ENDPOINT records a replica's IP; endpoints feed the load balancer. */
ATF_TC_WITHOUT_HEAD(cp_endpoints);
ATF_TC_BODY(cp_endpoints, tc)
{
	struct cp_state *s = cp_new();
	char ips[8][64];
	int n;

	ATF_REQUIRE(s != NULL);
	ATF_REQUIRE_EQ(0, cp_apply(s, "CREATE web 3 nginx"));
	ATF_REQUIRE_EQ(0, cp_apply(s, "ASSIGN web 0 n1"));
	ATF_REQUIRE_EQ(0, cp_apply(s, "ASSIGN web 1 n2"));

	/* No endpoints reported yet. */
	n = cp_service_endpoints(s, "web", ips, 8);
	ATF_CHECK_EQ(0, n);

	/* Report two replica endpoints. */
	ATF_REQUIRE_EQ(0, cp_apply(s, "ENDPOINT web 0 203.0.113.11"));
	ATF_REQUIRE_EQ(0, cp_apply(s, "ENDPOINT web 1 203.0.113.12"));
	ATF_CHECK_STREQ("203.0.113.11", cp_replica_endpoint(s, "web", 0));

	n = cp_service_endpoints(s, "web", ips, 8);
	ATF_CHECK_EQ_MSG(2, n, "expected 2 endpoints, got %d", n);

	/* An endpoint for an unplaced replica is ignored. */
	ATF_CHECK(cp_apply(s, "ENDPOINT web 2 203.0.113.13") != 0);
	ATF_CHECK_EQ(2, cp_service_endpoints(s, "web", ips, 8));
	cp_free(s);
}

/* VIP assigns a service's virtual IP (the load-balancer front). */
ATF_TC_WITHOUT_HEAD(cp_service_vip);
ATF_TC_BODY(cp_service_vip, tc)
{
	struct cp_state *s = cp_new();

	ATF_REQUIRE(s != NULL);
	ATF_REQUIRE_EQ(0, cp_apply(s, "CREATE web 3 nginx"));
	ATF_CHECK(cp_service_vip(s, "web") == NULL);	/* none yet */

	ATF_REQUIRE_EQ(0, cp_apply(s, "VIP web 203.0.113.100"));
	ATF_CHECK_STREQ("203.0.113.100", cp_service_vip(s, "web"));

	/* VIP for an unknown service is rejected. */
	ATF_CHECK(cp_apply(s, "VIP nope 203.0.113.1") != 0);
	cp_free(s);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, cp_service_lifecycle);
	ATF_TP_ADD_TC(tp, cp_service_vip);
	ATF_TP_ADD_TC(tp, cp_endpoints);
	ATF_TP_ADD_TC(tp, cp_stress_scale);
	ATF_TP_ADD_TC(tp, cp_placement);
	ATF_TP_ADD_TC(tp, cp_delete_drops_placements);
	ATF_TP_ADD_TC(tp, cp_rejects_bad_commands);
	ATF_TP_ADD_TC(tp, cp_deterministic_convergence);
	return (atf_no_error());
}
