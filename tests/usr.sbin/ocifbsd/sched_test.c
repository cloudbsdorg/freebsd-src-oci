/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Unit tests for the pod scheduler's node placement (orchestration/scheduler.c).
 */

#include <atf-c.h>
#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "orchestration.h"

/* scheduler.c publishes events; stub it so the module links standalone. */
int
orch_event_publish(const char *type, const char *object,
    const char *namespace, const char *message, ...)
{
	(void)type; (void)object; (void)namespace; (void)message;
	return (0);
}

#include "orchestration/scheduler.c"

/* Place one pod and return the chosen node name ("" if none). */
static const char *
place(void)
{
	static char buf[256];
	struct pod_spec spec;
	struct scheduling_decision *d;

	memset(&spec, 0, sizeof(spec));
	buf[0] = '\0';
	d = scheduler_select_node(&spec);
	if (d != NULL) {
		strlcpy(buf, d->node, sizeof(buf));
		free(d->failed_reason);
		free(d);
	}
	return (buf);
}

/* Replicas spread across multiple registered worker nodes. */
ATF_TC_WITHOUT_HEAD(sched_spreads_across_nodes);
ATF_TC_BODY(sched_spreads_across_nodes, tc)
{
	const char *seen[8];
	int nseen = 0;

	ATF_REQUIRE_EQ(0, scheduler_init());
	ATF_REQUIRE_EQ(0, scheduler_add_node("worker1"));
	ATF_REQUIRE_EQ(0, scheduler_add_node("worker2"));

	for (int i = 0; i < 6; i++) {
		const char *n = place();
		int found = 0;
		ATF_REQUIRE_MSG(n[0] != '\0', "no node chosen for replica %d", i);
		for (int j = 0; j < nseen; j++)
			if (strcmp(seen[j], n) == 0) { found = 1; break; }
		if (!found && nseen < 8)
			seen[nseen++] = strdup(n);
	}
	ATF_CHECK_MSG(nseen >= 2,
	    "replicas did not spread: only %d distinct node(s) used", nseen);
}

/* A cordoned (unschedulable) node never receives pods. */
ATF_TC_WITHOUT_HEAD(sched_skips_cordoned);
ATF_TC_BODY(sched_skips_cordoned, tc)
{
	ATF_REQUIRE_EQ(0, scheduler_init());
	ATF_REQUIRE_EQ(0, scheduler_add_node("worker1"));

	/* Cordon the local node: everything must land on worker1. */
	ATF_REQUIRE_EQ(0, scheduler_set_node_schedulable("localhost", false));

	for (int i = 0; i < 6; i++) {
		const char *n = place();
		ATF_REQUIRE_MSG(n[0] != '\0', "no node chosen for replica %d", i);
		ATF_CHECK_MSG(strcmp(n, "localhost") != 0,
		    "pod scheduled onto a cordoned node");
	}
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, sched_spreads_across_nodes);
	ATF_TP_ADD_TC(tp, sched_skips_cordoned);
	return (atf_no_error());
}
