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

	gethostname(hn, sizeof(hn));
	for (;;) {
		char leader[256] = "";
		int role = raft_role();
		const char *rs = role == 2 ? "LEADER" :
		    role == 1 ? "CANDIDATE" : "FOLLOWER";

		raft_get_leader(leader, sizeof(leader));
		printf("[%s] role=%-9s term=%llu leader=%s\n", hn, rs,
		    (unsigned long long)raft_term(),
		    leader[0] ? leader : "(none)");
		fflush(stdout);
		sleep(1);
	}
	return (0);
}
