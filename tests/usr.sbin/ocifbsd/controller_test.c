/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Unit tests for the controller planner (clustering/controller.c).
 */

#include <atf-c.h>
#include <stdio.h>
#include <string.h>

#include "clustering/control_plane.c"
#include "clustering/controller.c"

static const char *NODES[3] = { "n1", "n2", "n3" };

/* Apply a plan's commands back into the state (as the Raft log would). */
static void
apply_plan(struct cp_state *st, char plan[][256], int n)
{
	for (int i = 0; i < n; i++)
		ATF_REQUIRE_EQ(0, cp_apply(st, plan[i]));
}

/* Initial placement: 3 replicas spread across 2 nodes. */
ATF_TC_WITHOUT_HEAD(controller_initial_spread);
ATF_TC_BODY(controller_initial_spread, tc)
{
	struct cp_state *st = cp_new();
	char plan[16][256];
	int n = 0;

	ATF_REQUIRE(st != NULL);
	ATF_REQUIRE_EQ(0, cp_apply(st, "CREATE web 3 nginx"));

	ATF_REQUIRE_EQ(0, controller_plan(st, NODES, 2, plan, 16, &n));
	ATF_CHECK_EQ_MSG(3, n, "expected 3 ASSIGN commands, got %d", n);
	apply_plan(st, plan, n);

	/* All three replicas are now placed. */
	for (int id = 0; id < 3; id++)
		ATF_CHECK_MSG(cp_replica_node(st, "web", id) != NULL,
		    "replica web-%d not placed", id);
	/* Spread across both nodes (2+1). */
	ATF_CHECK_EQ(3, cp_node_replica_count(st, "n1") +
	    cp_node_replica_count(st, "n2"));
	ATF_CHECK(cp_node_replica_count(st, "n1") > 0);
	ATF_CHECK(cp_node_replica_count(st, "n2") > 0);
	cp_free(st);
}

/* Scale up: only the not-yet-placed replicas are assigned. */
ATF_TC_WITHOUT_HEAD(controller_scale_up);
ATF_TC_BODY(controller_scale_up, tc)
{
	struct cp_state *st = cp_new();
	char plan[16][256];
	int n = 0;

	ATF_REQUIRE(st != NULL);
	ATF_REQUIRE_EQ(0, cp_apply(st, "CREATE web 3 nginx"));
	ATF_REQUIRE_EQ(0, cp_apply(st, "ASSIGN web 0 n1"));	/* already placed */

	ATF_REQUIRE_EQ(0, controller_plan(st, NODES, 3, plan, 16, &n));
	ATF_CHECK_EQ_MSG(2, n, "expected 2 new ASSIGNs, got %d", n);
	for (int i = 0; i < n; i++)
		ATF_CHECK(strncmp(plan[i], "ASSIGN", 6) == 0);
	cp_free(st);
}

/* Scale down: replicas beyond the desired count are unassigned. */
ATF_TC_WITHOUT_HEAD(controller_scale_down);
ATF_TC_BODY(controller_scale_down, tc)
{
	struct cp_state *st = cp_new();
	char plan[16][256];
	int n = 0, unassigns = 0;

	ATF_REQUIRE(st != NULL);
	ATF_REQUIRE_EQ(0, cp_apply(st, "CREATE web 1 nginx"));
	ATF_REQUIRE_EQ(0, cp_apply(st, "ASSIGN web 0 n1"));
	ATF_REQUIRE_EQ(0, cp_apply(st, "ASSIGN web 1 n2"));
	ATF_REQUIRE_EQ(0, cp_apply(st, "ASSIGN web 2 n3"));

	ATF_REQUIRE_EQ(0, controller_plan(st, NODES, 3, plan, 16, &n));
	for (int i = 0; i < n; i++)
		if (strncmp(plan[i], "UNASSIGN", 8) == 0)
			unassigns++;
	ATF_CHECK_EQ_MSG(2, unassigns, "expected to unplace web 1 and 2");
	cp_free(st);
}

/* Convergence: re-planning a satisfied state yields nothing. */
ATF_TC_WITHOUT_HEAD(controller_converges);
ATF_TC_BODY(controller_converges, tc)
{
	struct cp_state *st = cp_new();
	char plan[16][256];
	int n = 0;

	ATF_REQUIRE(st != NULL);
	ATF_REQUIRE_EQ(0, cp_apply(st, "CREATE web 4 nginx"));
	ATF_REQUIRE_EQ(0, cp_apply(st, "CREATE db 2 postgres"));

	ATF_REQUIRE_EQ(0, controller_plan(st, NODES, 3, plan, 16, &n));
	apply_plan(st, plan, n);

	/* Second pass has nothing to do. */
	ATF_REQUIRE_EQ(0, controller_plan(st, NODES, 3, plan, 16, &n));
	ATF_CHECK_EQ_MSG(0, n, "already-placed state should need no commands");
	cp_free(st);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, controller_initial_spread);
	ATF_TP_ADD_TC(tp, controller_scale_up);
	ATF_TP_ADD_TC(tp, controller_scale_down);
	ATF_TP_ADD_TC(tp, controller_converges);
	return (atf_no_error());
}
