/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * End-to-end integration test of the distributed-services control path, using
 * the pure pieces exactly as the daemon composes them:
 *
 *   desired state  --controller_plan-->  ASSIGN commands
 *                  --cp_apply (Raft)-->  replicated placements
 *   per node       --controller_node_assignments-->  its assignment set
 *                  --agent_marshal/unmarshal (mTLS wire)-->  received set
 *                  --agent_reconcile-->  launch actions
 *
 * and checks that every desired replica is launched exactly once, spread across
 * the cluster.
 */

#include <atf-c.h>
#include <stdio.h>
#include <string.h>

#include "clustering/control_plane.c"
#include "clustering/controller.c"
#include "clustering/node_agent.c"

ATF_TC_WITHOUT_HEAD(pipeline_deploy_service_across_nodes);
ATF_TC_BODY(pipeline_deploy_service_across_nodes, tc)
{
	const char *nodes[3] = { "n1", "n2", "n3" };
	struct cp_state *st = cp_new();
	char plan[64][256];
	int nplan = 0;
	int launched_total = 0;
	int seen[16];			/* replica_id -> launched count */

	memset(seen, 0, sizeof(seen));
	ATF_REQUIRE(st != NULL);

	/* 1. Desired: a 5-replica service. */
	ATF_REQUIRE_EQ(0, cp_apply(st, "CREATE web 5 nginx:1.27"));

	/* 2. Leader plans placements; 3. they replicate (apply to the state). */
	ATF_REQUIRE_EQ(0, controller_plan(st, nodes, 3, plan, 64, &nplan));
	ATF_CHECK_EQ(5, nplan);
	for (int i = 0; i < nplan; i++)
		ATF_REQUIRE_EQ(0, cp_apply(st, plan[i]));

	/* 4. Each node receives its slice over the wire and reconciles it. */
	for (int j = 0; j < 3; j++) {
		struct agent_replica desired[16], received[16];
		struct agent_action act[32];
		char blob[4096];
		int nd = 0, nr = 0, na = 0;

		ATF_REQUIRE_EQ(0, controller_node_assignments(st, nodes[j],
		    desired, 16, &nd));

		/* mTLS wire roundtrip. */
		ATF_REQUIRE_EQ(0, agent_marshal(desired, nd, blob, sizeof(blob)));
		ATF_REQUIRE_EQ(0, agent_unmarshal(blob, received, 16, &nr));
		ATF_CHECK_EQ(nd, nr);

		/* Nothing running yet -> every assignment is a LAUNCH. */
		ATF_REQUIRE_EQ(0, agent_reconcile(received, nr, NULL, 0, act,
		    32, &na));
		for (int a = 0; a < na; a++) {
			ATF_CHECK_EQ(AGENT_LAUNCH, act[a].op);
			ATF_CHECK_STREQ("web", act[a].replica.service);
			ATF_CHECK_STREQ("nginx:1.27", act[a].replica.image);
			ATF_REQUIRE(act[a].replica.replica_id < 16);
			seen[act[a].replica.replica_id]++;
			launched_total++;
		}
	}

	/* Every one of the 5 replicas launched exactly once, cluster-wide. */
	ATF_CHECK_EQ_MSG(5, launched_total,
	    "expected 5 launches across the cluster, got %d", launched_total);
	for (int id = 0; id < 5; id++)
		ATF_CHECK_EQ_MSG(1, seen[id],
		    "replica web-%d launched %d times (want exactly 1)", id,
		    seen[id]);
	cp_free(st);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, pipeline_deploy_service_across_nodes);
	return (atf_no_error());
}
