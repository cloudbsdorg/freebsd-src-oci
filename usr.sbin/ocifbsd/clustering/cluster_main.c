/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ocifbsd-cluster: run an ocifbsd cluster node — gossip membership + SWIM
 * failure detection + Raft consensus (leader election, log replication, and
 * snapshots). This is the user-facing front end for the clustering library;
 * it wires a node's configuration, joins the given peers, and runs the Raft
 * state machine, reporting leadership on change (and on SIGINFO).
 */

#include <sys/types.h>

#include <err.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "cluster.h"

#define OCLUSTER_VERSION "0.1.0"

static volatile sig_atomic_t stop_requested;
static volatile sig_atomic_t status_requested;

static void
on_stop(int sig)
{
	(void)sig;
	stop_requested = 1;
}

static void
on_info(int sig)
{
	(void)sig;
	status_requested = 1;
}

static const char *
role_str(int role)
{
	return (role == 2 ? "leader" : role == 1 ? "candidate" : "follower");
}

static void
print_status(const char *node)
{
	char leader[256] = "";
	int n = 0;
	struct cluster_node **nl = cluster_nodes_list(&n);

	free(nl);
	raft_get_leader(leader, sizeof(leader));
	printf("node=%s role=%s term=%llu leader=%s members=%d "
	    "log=%d commit=%llu snapshot=%llu\n",
	    node, role_str(raft_role()), (unsigned long long)raft_term(),
	    leader[0] ? leader : "(none)", n + 1, raft_log_len(),
	    (unsigned long long)raft_commit_index(),
	    (unsigned long long)raft_snapshot_index());
	fflush(stdout);
}

static void
usage(const char *p)
{
	fprintf(stderr,
	    "usage: %s [-p port] [-P host@ip]... [-s statefile] [-n name] run\n"
	    "       %s -h | -v\n"
	    "  -p, --port      gossip/consensus UDP port (default 6789)\n"
	    "  -P, --peer      seed peer as hostname@ip (repeatable)\n"
	    "  -s, --state     Raft state file for crash recovery\n"
	    "  -n, --name      override this node's id (default: hostname)\n"
	    "  -i, --interval  status print interval in seconds (0 = on change)\n",
	    p, p);
}

int
main(int argc, char **argv)
{
	struct cluster_config cfg;
	char hn[256];
	const char *state = NULL, *name = NULL;
	char *peers[64];
	int npeers = 0, interval = 0, ch;
	int port = 6789;

	static struct option lo[] = {
		{ "port",	required_argument,	NULL, 'p' },
		{ "peer",	required_argument,	NULL, 'P' },
		{ "state",	required_argument,	NULL, 's' },
		{ "name",	required_argument,	NULL, 'n' },
		{ "interval",	required_argument,	NULL, 'i' },
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'v' },
		{ NULL,		0,			NULL, 0 }
	};

	while ((ch = getopt_long(argc, argv, "p:P:s:n:i:hv", lo, NULL)) != -1) {
		switch (ch) {
		case 'p': port = atoi(optarg); break;
		case 'P':
			if (npeers < (int)(sizeof(peers) / sizeof(peers[0])))
				peers[npeers++] = optarg;
			break;
		case 's': state = optarg; break;
		case 'n': name = optarg; break;
		case 'i': interval = atoi(optarg); break;
		case 'v': printf("ocifbsd-cluster %s\n", OCLUSTER_VERSION);
			return (0);
		case 'h': usage(argv[0]); return (0);
		default: usage(argv[0]); return (2);
		}
	}
	argc -= optind;
	argv += optind;
	if (argc < 1 || strcmp(argv[0], "run") != 0) {
		usage("ocifbsd-cluster");
		return (2);
	}
	if (port <= 0 || port > 65535)
		errx(2, "invalid port %d", port);

	/* State file and node-id/ip overrides are read from the environment by
	 * the clustering library; set them from the flags before init. */
	if (state != NULL)
		setenv("OCIFBSD_RAFT_STATE", state, 1);
	if (name != NULL)
		setenv("OCIFBSD_NODE_ID", name, 1);

	memset(&cfg, 0, sizeof(cfg));
	strlcpy(cfg.cluster_name, "ocifbsd", sizeof(cfg.cluster_name));
	cfg.cluster_port = (uint16_t)port;
	cfg.gossip_interval = 1000;
	cfg.gossip_fanout = 3;
	cfg.suspicion_timeout = 5000;
	cfg.node_timeout = 10000;
	cfg.max_payload_size = 65000;

	if (cluster_init(&cfg) != 0)
		errx(1, "cluster_init failed");
	raft_init();
	for (int i = 0; i < npeers; i++) {
		char *at = strchr(peers[i], '@');

		if (at == NULL)
			errx(2, "bad --peer '%s' (want hostname@ip)", peers[i]);
		*at = '\0';
		cluster_node_add(peers[i], at + 1, (uint16_t)port);
	}
	if (gossip_start() != 0)
		errx(1, "gossip_start failed (port %d in use?)", port);
	if (raft_start() != 0)
		errx(1, "raft_start failed");
	cluster_announce();

	signal(SIGINT, on_stop);
	signal(SIGTERM, on_stop);
	signal(SIGINFO, on_info);

	gethostname(hn, sizeof(hn));
	printf("ocifbsd-cluster: node '%s' up on udp/%d with %d seed peer(s)\n",
	    name ? name : hn, port, npeers);
	print_status(name ? name : hn);

	{
		int last_role = -1, secs = 0;
		uint64_t last_term = 0;
		char last_leader[256] = "";

		while (!stop_requested) {
			char leader[256] = "";
			int role = raft_role();
			uint64_t term = raft_term();

			raft_get_leader(leader, sizeof(leader));
			if (role != last_role || term != last_term ||
			    strcmp(leader, last_leader) != 0 ||
			    (interval > 0 && secs % interval == 0)) {
				print_status(name ? name : hn);
				last_role = role;
				last_term = term;
				strlcpy(last_leader, leader, sizeof(last_leader));
			}
			if (status_requested) {
				status_requested = 0;
				print_status(name ? name : hn);
			}
			sleep(1);
			secs++;
		}
	}

	printf("ocifbsd-cluster: shutting down\n");
	raft_stop();
	cluster_shutdown();
	return (0);
}
