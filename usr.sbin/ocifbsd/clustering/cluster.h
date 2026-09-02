/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH
 * DAMAGE.
 *
 * $FreeBSD$
 *
 * Clustering and gossip protocol header
 */

#ifndef _OCIFBSD_CLUSTER_H
#define _OCIFBSD_CLUSTER_H

#include <sys/tree.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Cluster node states */
#define NODE_STATE_LEFT      0
#define NODE_STATE_JOINING   1
#define NODE_STATE_ACTIVE    2
#define NODE_STATE_SUSPECTED 3
#define NODE_STATE_DEAD      4

/* Cluster roles */
#define NODE_ROLE_WORKER     0
#define NODE_ROLE_MANAGER    1
#define NODE_ROLE_STORAGE    2

/* Message types for gossip protocol */
#define GOSSIP_MSG_JOIN          0
#define GOSSIP_MSG_ALIVE         1
#define GOSSIP_MSG_DEAD          2
#define GOSSIP_MSG_STATE         3
#define GOSSIP_MSG_REQUEST       4
#define GOSSIP_MSG_RESPONSE      5
#define GOSSIP_MSG_USER_DATA     6
#define GOSSIP_MSG_HEARTBEAT    7
/* Raft consensus RPCs (RFC-style RequestVote / AppendEntries). */
#define GOSSIP_MSG_VOTE_REQ      8
#define GOSSIP_MSG_VOTE_RESP     9
#define GOSSIP_MSG_APPEND_REQ   10
#define GOSSIP_MSG_APPEND_RESP  11
#define GOSSIP_MSG_INSTALL_SNAP 12   /* InstallSnapshot (log compaction) */

/* Cluster configuration */
struct cluster_config {
	char cluster_name[256];
	uint16_t cluster_port;         /* UDP port for gossip */
	uint16_t api_port;             /* TCP port for API */
	int gossip_interval;           /* ms between gossip rounds */
	int gossip_fanout;            /* number of peers to contact */
	int suspicion_timeout;         /* ms before suspecting node */
	int node_timeout;             /* ms before marking node dead */
	int max_payload_size;         /* max gossip message size */
	bool enable_encryption;       /* encrypt gossip traffic */
	char *cluster_key;            /* encryption key if enabled */
};

/* Cluster node information */
struct cluster_node {
	char node_id[256];            /* unique node identifier */
	char hostname[256];           /* node hostname */
	char ip[64];                  /* node IP address */
	uint16_t port;                /* gossip port */
	uint16_t api_port;            /* API port */
	int role;                     /* NODE_ROLE_* */
	int state;                    /* NODE_STATE_* */
	time_t last_seen;             /* timestamp of last message */
	time_t joined_at;             /* when node joined cluster */
	uint64_t incarnation;        /* incarnation number for anti-entropy */
	double weight;                 /* scheduling weight */
	uint64_t capacity_cpu;       /* CPU capacity units */
	uint64_t capacity_memory;     /* memory capacity */
	uint64_t capacity_storage;    /* storage capacity */
	uint64_t used_cpu;            /* used CPU */
	uint64_t used_memory;         /* used memory */
	uint64_t used_storage;        /* used storage */
	char **labels;                /* node labels for scheduling */
	int nlabels;

	/* Gossip state */
	uint32_t suspicion_count;     /* number of suspicion rounds */
	time_t suspicion_started;

	/* Raft leader-side replication cursors for this peer (valid while this
	 * node is leader): next log index to send, and highest index known
	 * replicated on the peer. */
	uint64_t raft_next_index;
	uint64_t raft_match_index;

	/* Tree entry */
	RB_ENTRY(cluster_node) entry;
	pthread_mutex_t lock;

	/*
	 * Lifetime management (protected by node_registry_lock): cluster_node_get
	 * hands out a counted reference; cluster_node_remove detaches the node
	 * from the tree but defers free() until the last reference is released
	 * via cluster_node_put. This prevents a use-after-free when one thread
	 * removes a node while another is still operating on it.
	 */
	int refcount;
	bool removed;
};

/* Node RB tree */
RB_HEAD(node_tree, cluster_node);
RB_PROTOTYPE(node_tree, cluster_node, entry, node_compare);

/* Gossip message */
struct gossip_message {
	uint8_t type;                 /* GOSSIP_MSG_* */
	uint8_t version;              /* protocol version */
	uint16_t length;             /* payload length */
	char source_id[256];         /* source node ID */
	uint64_t source_incarnation; /* source incarnation */
	uint64_t timestamp;          /* message timestamp */
	uint8_t payload[];           /* variable length payload */
};

/* SWIM failure detector state */
struct swim_state {
	uint32_t proto_version;
	uint32_t ack_version;
	uint32_t suspect_version;
	uint32_t coordinator_version;
};

/* Cluster event */
struct cluster_event {
	char node_id[256];
	int event_type;              /* JOIN, LEAVE, FAILURE, RECOVERY */
	time_t timestamp;
	char details[512];
};

/* Control-plane state machine (opaque; see control_plane.h). */
struct cp_state;

/* Raft consensus state */
struct raft_state {
	enum {
		RAFT_FOLLOWER,
		RAFT_CANDIDATE,
		RAFT_LEADER
	} state;

	uint64_t current_term;
	char voted_for[256];
	uint64_t commit_index;
	uint64_t last_applied;

	/* Leader info */
	char leader_id[256];
	time_t last_heartbeat;

	/*
	 * Snapshot boundary: log[] holds entries with absolute index
	 * > snapshot_index only; everything up to (snapshot_index, snapshot_term)
	 * has been compacted away. absolute index N maps to log[N -
	 * snapshot_index - 1].
	 */
	uint64_t snapshot_index;
	uint64_t snapshot_term;

	/* Log (entries after the snapshot boundary) */
	struct raft_log_entry {
		uint64_t term;
		uint64_t index;
		char command[256];
		time_t timestamp;
	} *log;
	int log_size;
	int log_capacity;

	/* Replicated control-plane state, rebuilt by applying committed log
	 * entries in order (see raft_apply_committed / control_plane.c). */
	struct cp_state *cp;
};

/* Cluster management functions */
int cluster_init(struct cluster_config *config);
int cluster_shutdown(void);
int cluster_join(const char *manager_address);
int cluster_leave(const char *reason);

/* Node management */
struct cluster_node *cluster_node_add(const char *node_id, const char *ip, uint16_t port);
int cluster_node_remove(const char *node_id);
struct cluster_node *cluster_node_get(const char *node_id);
void cluster_node_put(struct cluster_node *node);
struct cluster_node **cluster_nodes_list(int *count);
/* Release an array returned by cluster_nodes_list (drops each node's ref). */
void cluster_nodes_free(struct cluster_node **nodes, int count);
struct cluster_node **cluster_nodes_by_role(int role, int *count);

/* Gossip protocol */
int gossip_start(void);
int gossip_stop(void);
int gossip_send_message(const char *target_id, uint8_t type, const void *payload, size_t len);
int gossip_broadcast(uint8_t type, const void *payload, size_t len);
/* Announce this node (as a JOIN) to the currently-known peers, so a running
 * cluster learns of a newly-started member. */
int cluster_announce(void);

/* Failure detection */
int swim_init(void);
int swim_process_heartbeat(const char *node_id, uint64_t incarnation);
int swim_mark_suspected(const char *node_id);
int swim_mark_dead(const char *node_id);
int swim_mark_alive(const char *node_id);

/* Raft consensus */
int raft_init(void);
int raft_start(void);   /* start the election/heartbeat ticker thread */
int raft_stop(void);    /* stop the ticker thread */
int raft_role(void);    /* 0=follower, 1=candidate, 2=leader */
uint64_t raft_term(void);
uint64_t raft_commit_index(void);
int raft_log_len(void);
uint64_t raft_snapshot_index(void);
int raft_become_leader(void);
int raft_become_follower(const char *leader_id);
int raft_become_candidate(void);
int raft_append_entry(const char *command, size_t len);

/*
 * Control-plane over Raft: propose a command (leader only) that is replicated
 * and applied on every node, and query the replicated desired state.
 */
int cluster_cp_propose(const char *command);
int cluster_service_replicas(const char *service);

/*
 * Distributed-services glue over Raft: the leader plans placements each tick;
 * every node reads the assignment set the replicated state gives it.
 */
struct agent_replica;	/* node_agent.h */
int cluster_controller_tick(const char *const *names, const char *const *addrs,
	int nnodes);
int cluster_placement_count(void);
int cluster_service_endpoint_count(const char *service);
int cluster_lb_ruleset(const char *service, char *out, size_t outlen);
int cluster_node_assignments(const char *node, struct agent_replica *out,
	int max, int *nout);
int raft_replicate_log(const char *target_id);
int raft_commit_log(uint64_t index);
int raft_get_leader(char *leader_id, size_t len);

/* Cluster events */
typedef void (*cluster_event_cb)(struct cluster_event *event);
int cluster_set_event_callback(cluster_event_cb callback);
int cluster_emit_event(int event_type, const char *node_id, const char *details);

/* Cluster status */
int cluster_status_json(char **json_out, size_t *json_len);
int cluster_nodes_json(char **json_out, size_t *json_len);
int cluster_health_json(char **json_out, size_t *json_len);

/* Service discovery */
struct service_endpoint {
	char service_name[256];
	char namespace[256];
	char node_id[256];
	char ip[64];
	uint16_t port;
	char protocol[16];             /* tcp, udp, http, https */
	int replicas;
	time_t last_updated;
};

int cluster_register_service(const char *name, const char *ns, const char *ip, uint16_t port, const char *protocol);
int cluster_unregister_service(const char *name, const char *ns);
struct service_endpoint **cluster_find_services(const char *name, const char *ns, int *count);
int cluster_services_json(char **json_out, size_t *json_len);

#endif /* _OCIFBSD_CLUSTER_H */
