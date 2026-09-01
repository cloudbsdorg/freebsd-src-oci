/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Unit tests for the control-plane state machine (clustering/control_plane.c).
 */

#include <atf-c.h>
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

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, cp_service_lifecycle);
	ATF_TP_ADD_TC(tp, cp_placement);
	ATF_TP_ADD_TC(tp, cp_delete_drops_placements);
	ATF_TP_ADD_TC(tp, cp_rejects_bad_commands);
	ATF_TP_ADD_TC(tp, cp_deterministic_convergence);
	return (atf_no_error());
}
