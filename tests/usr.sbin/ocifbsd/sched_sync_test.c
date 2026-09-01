/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Unit tests for syncing cluster membership into the scheduler
 * (scheduler_sync_nodes in orchestration/scheduler.c).
 */

#include <atf-c.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "orchestration.h"

int
orch_event_publish(const char *type, const char *object,
    const char *namespace, const char *message, ...)
{
	(void)type; (void)object; (void)namespace; (void)message;
	return (0);
}

#include "orchestration/scheduler.c"

static int
placed_on(const char *want)
{
	struct pod_spec spec;
	struct scheduling_decision *d;
	int hit = 0;

	memset(&spec, 0, sizeof(spec));
	d = scheduler_select_node(&spec);
	if (d != NULL) {
		hit = (strcmp(d->node, want) == 0);
		free(d->failed_reason);
		free(d);
	}
	return (hit);
}

/* Syncing cluster nodes makes them schedulable; dropping one retires it. */
ATF_TC_WITHOUT_HEAD(sync_adds_and_retires_nodes);
ATF_TC_BODY(sync_adds_and_retires_nodes, tc)
{
	struct sched_node in2[] = {
		{ "w1", "203.0.113.10" },
		{ "w2", "203.0.113.11" },
	};
	struct sched_node in1[] = {
		{ "w1", "203.0.113.10" },
	};
	int w2_hits = 0;

	ATF_REQUIRE_EQ(0, scheduler_init());

	/* Before sync, only localhost exists. */
	ATF_REQUIRE_EQ(0, scheduler_sync_nodes(in2, 2));

	/* Both synced workers should now be able to receive pods. */
	for (int i = 0; i < 12; i++)
		if (placed_on("w2"))
			w2_hits++;
	ATF_CHECK_MSG(w2_hits > 0, "synced node w2 never scheduled");

	/* Re-sync without w2: it must retire and stop receiving pods. */
	ATF_REQUIRE_EQ(0, scheduler_sync_nodes(in1, 1));
	for (int i = 0; i < 12; i++)
		ATF_CHECK_MSG(!placed_on("w2"),
		    "retired node w2 still receiving pods");
}

/* The local node is never retired by a cluster sync. */
ATF_TC_WITHOUT_HEAD(sync_keeps_localhost);
ATF_TC_BODY(sync_keeps_localhost, tc)
{
	struct sched_node in[] = { { "w1", "203.0.113.10" } };
	int local_hits = 0;

	ATF_REQUIRE_EQ(0, scheduler_init());
	ATF_REQUIRE_EQ(0, scheduler_sync_nodes(in, 1));

	/* Cordon w1 so only localhost can take pods; it must still work. */
	ATF_REQUIRE_EQ(0, scheduler_set_node_schedulable("w1", false));
	for (int i = 0; i < 6; i++)
		if (placed_on("localhost"))
			local_hits++;
	ATF_CHECK_MSG(local_hits == 6,
	    "localhost was retired or skipped by the sync");
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, sync_adds_and_retires_nodes);
	ATF_TP_ADD_TC(tp, sync_keeps_localhost);
	return (atf_no_error());
}
