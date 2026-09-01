/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Unit tests for the node agent's wire format and reconciliation
 * (clustering/node_agent.c).
 */

#include <atf-c.h>
#include <stdio.h>
#include <string.h>

#include "clustering/node_agent.c"

static struct agent_replica
mk(const char *svc, int id, const char *img)
{
	struct agent_replica r;

	memset(&r, 0, sizeof(r));
	strlcpy(r.service, svc, sizeof(r.service));
	r.replica_id = id;
	strlcpy(r.image, img, sizeof(r.image));
	return (r);
}

/* An assignment set survives marshal -> unmarshal unchanged. */
ATF_TC_WITHOUT_HEAD(agent_marshal_roundtrip);
ATF_TC_BODY(agent_marshal_roundtrip, tc)
{
	struct agent_replica in[3] = {
		mk("web", 0, "nginx:1.27"),
		mk("web", 1, "nginx:1.27"),
		mk("db", 0, "postgres:16"),
	};
	struct agent_replica out[8];
	char buf[1024];
	int n = 0;

	ATF_REQUIRE_EQ(0, agent_marshal(in, 3, buf, sizeof(buf)));
	ATF_REQUIRE_EQ(0, agent_unmarshal(buf, out, 8, &n));
	ATF_REQUIRE_EQ(3, n);
	for (int i = 0; i < 3; i++) {
		ATF_CHECK_STREQ(in[i].service, out[i].service);
		ATF_CHECK_EQ(in[i].replica_id, out[i].replica_id);
		ATF_CHECK_STREQ(in[i].image, out[i].image);
	}
}

/* Reconcile launches missing replicas and stops extra ones. */
ATF_TC_WITHOUT_HEAD(agent_reconcile_launch_and_stop);
ATF_TC_BODY(agent_reconcile_launch_and_stop, tc)
{
	struct agent_replica desired[2] = {
		mk("web", 0, "nginx"), mk("web", 1, "nginx"),
	};
	struct agent_replica running[2] = {
		mk("web", 0, "nginx"), mk("web", 9, "nginx"),
	};
	struct agent_action act[8];
	int n = 0, launched = 0, stopped = 0;

	ATF_REQUIRE_EQ(0, agent_reconcile(desired, 2, running, 2, act, 8, &n));
	for (int i = 0; i < n; i++) {
		if (act[i].op == AGENT_LAUNCH) {
			launched++;
			ATF_CHECK_EQ(1, act[i].replica.replica_id);	/* web-1 */
		} else {
			stopped++;
			ATF_CHECK_EQ(9, act[i].replica.replica_id);	/* web-9 */
		}
	}
	ATF_CHECK_EQ(1, launched);
	ATF_CHECK_EQ(1, stopped);
}

/* Nothing to do when running already matches desired. */
ATF_TC_WITHOUT_HEAD(agent_reconcile_noop);
ATF_TC_BODY(agent_reconcile_noop, tc)
{
	struct agent_replica set[2] = { mk("web", 0, "a"), mk("web", 1, "a") };
	struct agent_action act[8];
	int n = -1;

	ATF_REQUIRE_EQ(0, agent_reconcile(set, 2, set, 2, act, 8, &n));
	ATF_CHECK_EQ(0, n);
}

/* An image change relaunches the replica (stop old + launch new). */
ATF_TC_WITHOUT_HEAD(agent_reconcile_image_change);
ATF_TC_BODY(agent_reconcile_image_change, tc)
{
	struct agent_replica desired[1] = { mk("web", 0, "nginx:1.28") };
	struct agent_replica running[1] = { mk("web", 0, "nginx:1.27") };
	struct agent_action act[8];
	int n = 0, launched = 0, stopped = 0;

	ATF_REQUIRE_EQ(0, agent_reconcile(desired, 1, running, 1, act, 8, &n));
	for (int i = 0; i < n; i++) {
		if (act[i].op == AGENT_LAUNCH)
			launched++;
		else
			stopped++;
	}
	ATF_CHECK_MSG(stopped == 1 && launched == 1,
	    "image change should stop the old and launch the new replica");
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, agent_marshal_roundtrip);
	ATF_TP_ADD_TC(tp, agent_reconcile_launch_and_stop);
	ATF_TP_ADD_TC(tp, agent_reconcile_noop);
	ATF_TP_ADD_TC(tp, agent_reconcile_image_change);
	return (atf_no_error());
}
