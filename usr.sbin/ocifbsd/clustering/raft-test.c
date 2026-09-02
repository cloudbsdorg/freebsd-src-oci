/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * raft-test: a minimal multi-node harness for the ocifbsd Raft election.
 * Brings up cluster/gossip/raft, seeds the peer list, and prints this node's
 * role/term/leader once a second. Run one instance per node:
 *
 *   raft-test <port> <peer-hostname@ip> [<peer-hostname@ip> ...]
 *
 * The local node id is the host's name (as cluster_init derives it), so the
 * peer entries must use each peer's hostname and reachable IP on <port>.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cluster.h"

int
main(int argc, char **argv)
{
	struct cluster_config cfg;
	char hn[256];
	int i;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <port> [host@ip ...]\n", argv[0]);
		return (2);
	}
	memset(&cfg, 0, sizeof(cfg));
	strlcpy(cfg.cluster_name, "raft-test", sizeof(cfg.cluster_name));
	cfg.cluster_port = (uint16_t)atoi(argv[1]);
	cfg.gossip_interval = 1000;
	cfg.gossip_fanout = 3;
	cfg.suspicion_timeout = 5000;
	cfg.node_timeout = 10000;
	cfg.max_payload_size = 65000;

	if (cluster_init(&cfg) != 0) {
		fprintf(stderr, "cluster_init failed\n");
		return (1);
	}
	raft_init();

	for (i = 2; i < argc; i++) {
		char *at = strchr(argv[i], '@');

		if (at == NULL)
			continue;
		*at = '\0';
		cluster_node_add(argv[i], at + 1, cfg.cluster_port);
	}

	if (gossip_start() != 0) {
		fprintf(stderr, "gossip_start failed\n");
		return (1);
	}
	if (raft_start() != 0) {
		fprintf(stderr, "raft_start failed\n");
		return (1);
	}
	cluster_announce();     /* tell any running cluster we exist */

	gethostname(hn, sizeof(hn));
	int tick = 0, proposed = 0;
	for (;;) {
		char leader[256] = "";
		int role = raft_role();
		const char *rs = role == 2 ? "LEADER" :
		    role == 1 ? "CANDIDATE" : "FOLLOWER";

		/* As leader, propose a small burst each tick so replication,
		 * commit advancement, and log compaction can be observed. */
		if (role == 2) {
			int b;

			for (b = 0; b < 4; b++) {
				char cmd[64];

				snprintf(cmd, sizeof(cmd), "set x=%d", ++proposed);
				raft_append_entry(cmd, strlen(cmd));
			}
		}

		int nc = 0;
		struct cluster_node **nl = cluster_nodes_list(&nc);
		cluster_nodes_free(nl, nc);

		raft_get_leader(leader, sizeof(leader));
		printf("[%s] role=%-9s term=%llu leader=%-12s "
		    "nodes=%d log=%d commit=%llu snap=%llu\n", hn, rs,
		    (unsigned long long)raft_term(),
		    leader[0] ? leader : "(none)", nc + 1,
		    raft_log_len(), (unsigned long long)raft_commit_index(),
		    (unsigned long long)raft_snapshot_index());
		fflush(stdout);
		if ((tick % 5) == 0)
			cluster_announce();   /* re-announce so late joiners learn us */
		tick++;
		sleep(1);
	}
	return (0);
}
