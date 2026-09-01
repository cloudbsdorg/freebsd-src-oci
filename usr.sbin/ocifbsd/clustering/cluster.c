/*-
 * Copyright (c) 2026 REVYTECH, Inc.
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
 * Clustering and gossip protocol implementation
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <pthread.h>
#include <pwd.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sha256.h>

#include "cluster.h"
#include "control_plane.h"
#include "controller.h"
#include "node_agent.h"
#include "../include/ocifbsd.h"

/* Global cluster state */
static struct cluster_config cluster_conf;
static int cluster_initialized = 0;
static pthread_mutex_t cluster_lock = PTHREAD_MUTEX_INITIALIZER;

/* Node registry */
static struct node_tree node_registry;
static int node_registry_initialized = 0;
static pthread_mutex_t node_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static struct cluster_node *local_node;

/* Gossip state */
static int gossip_running = 0;
static pthread_t gossip_thread;
static int gossip_socket = -1;
static pthread_mutex_t gossip_lock = PTHREAD_MUTEX_INITIALIZER;

/* SWIM failure detector */
static struct swim_state swim;

/* Raft state */
static struct raft_state raft;

/*
 * Raft RPC wire payloads (host byte order; homogeneous amd64 cluster) and
 * inbound-RPC handlers. Declared here so the gossip dispatch below can decode
 * them; the implementations live with the election machinery further down.
 */
struct raft_vote_req {
    uint64_t term;
    uint64_t last_log_index;
    uint64_t last_log_term;
};
struct raft_vote_resp {
    uint64_t term;
    uint8_t  granted;
};
struct raft_append_req {
    uint64_t term;
    uint64_t prev_log_index;
    uint64_t prev_log_term;
    uint64_t leader_commit;
    uint32_t n_entries;
};
struct raft_append_resp {
    uint64_t term;
    uint8_t  success;
    uint64_t match_index;
};
/* One replicated log entry on the wire (fixed-size; command is bounded by the
 * raft_log_entry command field). AppendEntries carries n_entries of these
 * immediately after the struct raft_append_req header. */
struct raft_wire_entry {
    uint64_t term;
    uint64_t index;
    char     command[256];
};
#define RAFT_MAX_BATCH  16   /* entries per AppendEntries */

/*
 * InstallSnapshot: sent when a follower's needed entries have been compacted
 * away on the leader. Our state machine is the log itself, so the snapshot is
 * just the (index, term) boundary; a real state machine would ship its state
 * blob here. The reply reuses raft_append_resp with match_index = last_index.
 */
struct raft_snap_req {
    uint64_t term;
    uint64_t last_index;
    uint64_t last_term;
};

/* Gossip transport internals, used before their definitions below. */
static void *gossip_loop(void *arg);
static void gossip_handle_message(uint8_t *buf, size_t len,
    struct sockaddr_in *sender);
static void gossip_process_state(const char *source_id, uint8_t *payload,
    size_t len);

static void raft_on_vote_req(const char *from, const struct raft_vote_req *r);
static void raft_on_vote_resp(const char *from, const struct raft_vote_resp *r);
static void raft_on_append_req(const char *from, const struct raft_append_req *r);
static void raft_on_append_resp(const char *from,
    const struct raft_append_resp *r);
static void raft_on_install_snap(const char *from,
    const struct raft_snap_req *r);
static void raft_replicate_to_peers(void);
static void raft_advance_commit(void);
static void raft_apply_committed(void);
static void raft_maybe_compact(void);

/* Event callback */
static cluster_event_cb event_callback;

/* Service registry */
static struct service_endpoint **service_registry = NULL;
static int n_services = 0;
static pthread_mutex_t service_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * Compare nodes by ID
 */
int
node_compare(struct cluster_node *a, struct cluster_node *b)
{
    return (strcmp(a->node_id, b->node_id));
}

/*
 * Generate the red-black tree routines the node registry uses. cluster.h only
 * RB_PROTOTYPEs them; without the matching RB_GENERATE the node_tree_RB_*
 * symbols are undefined, so the clustering code could never link into a
 * program (only compile as an object). Emit them here, after node_compare.
 */
RB_GENERATE(node_tree, cluster_node, entry, node_compare);

/*
 * Initialize cluster subsystem
 */
int
cluster_init(struct cluster_config *config)
{
    struct ifaddrs *ifaddrs, *ifa;
    char hostname[256];
    char ip[64];
    
    if (__sync_fetch_and_add(&cluster_initialized, 0) != 0)
        return (0);
    
    pthread_mutex_lock(&cluster_lock);
    
    if (config != NULL) {
        memcpy(&cluster_conf, config, sizeof(struct cluster_config));
    } else {
        memset(&cluster_conf, 0, sizeof(struct cluster_config));
        cluster_conf.cluster_port = 6789;
        cluster_conf.api_port = 8080;
        cluster_conf.gossip_interval = 1000;
        cluster_conf.gossip_fanout = 3;
        cluster_conf.suspicion_timeout = 5000;
        cluster_conf.node_timeout = 30000;
        cluster_conf.max_payload_size = 65536;
    }
    
    /* Initialize node registry */
    if (!node_registry_initialized) {
        RB_INIT(&node_registry);
        node_registry_initialized = 1;
    }
    
    /* Create local node entry */
    local_node = calloc(1, sizeof(struct cluster_node));
    if (local_node == NULL) {
        pthread_mutex_unlock(&cluster_lock);
        return (-1);
    }
    
    gethostname(hostname, sizeof(hostname));
    strlcpy(local_node->hostname, hostname, sizeof(local_node->hostname));
    strlcpy(local_node->node_id, hostname, sizeof(local_node->node_id));
    
    /* Get IP address */
    if (getifaddrs(&ifaddrs) == 0) {
        for (ifa = ifaddrs; ifa != NULL; ifa = ifa->ifa_next) {
            if (ifa->ifa_addr == NULL)
                continue;
            if (ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (strcmp(ifa->ifa_name, "lo0") == 0)
                continue;
            
            struct sockaddr_in *addr = (struct sockaddr_in *)ifa->ifa_addr;
            inet_ntop(AF_INET, &addr->sin_addr, ip, sizeof(ip));
            strlcpy(local_node->ip, ip, sizeof(local_node->ip));
            break;
        }
        freeifaddrs(ifaddrs);
    }

    /*
     * Optional overrides: OCIFBSD_NODE_ID pins a stable node identity (useful
     * when several instances share a host), OCIFBSD_NODE_IP pins the address
     * this node advertises when it announces itself to a running cluster.
     */
    {
        const char *env_id = getenv("OCIFBSD_NODE_ID");
        const char *env_ip = getenv("OCIFBSD_NODE_IP");

        if (env_id != NULL && env_id[0] != '\0')
            strlcpy(local_node->node_id, env_id, sizeof(local_node->node_id));
        if (env_ip != NULL && env_ip[0] != '\0')
            strlcpy(local_node->ip, env_ip, sizeof(local_node->ip));
    }

    local_node->port = cluster_conf.cluster_port;
    local_node->api_port = cluster_conf.api_port;
    local_node->role = NODE_ROLE_WORKER;
    local_node->state = NODE_STATE_JOINING;
    local_node->last_seen = time(NULL);
    local_node->joined_at = time(NULL);
    local_node->incarnation = 1;
    local_node->weight = 1.0;
    
    pthread_mutex_init(&local_node->lock, NULL);
    
    /* Initialize Raft state */
    memset(&raft, 0, sizeof(raft));
    raft.state = RAFT_FOLLOWER;
    raft.current_term = 0;
    
    /* Initialize SWIM state */
    memset(&swim, 0, sizeof(swim));
    
    __sync_fetch_and_add(&cluster_initialized, 1);
    pthread_mutex_unlock(&cluster_lock);
    
    return (0);
}

/*
 * Shutdown cluster subsystem
 */
int
cluster_shutdown(void)
{
    if (__sync_fetch_and_add(&cluster_initialized, 0) == 0)
        return (0);
    
    gossip_stop();
    
    if (local_node != NULL) {
        pthread_mutex_destroy(&local_node->lock);
        free(local_node);
        local_node = NULL;
    }
    
    __sync_fetch_and_add(&cluster_initialized, 0);
    return (0);
}

/*
 * Join cluster via manager
 */
int
cluster_join(const char *manager_address)
{
    int sock;
    struct sockaddr_in addr;
    struct gossip_message msg;
    char *payload;
    size_t payload_len;
    ssize_t sent;
    
    if (!cluster_initialized || manager_address == NULL)
        return (-1);
    
    /* Create UDP socket */
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return (-1);
    
    /* Parse manager address */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cluster_conf.cluster_port);
    
    /* Handle hostname:port format */
    char host[256];
    char *port_str;
    strlcpy(host, manager_address, sizeof(host));
    port_str = strchr(host, ':');
    if (port_str != NULL) {
        *port_str = '\0';
        port_str++;
        addr.sin_port = htons(atoi(port_str));
    }
    
    if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
        /* Try hostname resolution */
        struct hostent *he = gethostbyname(host);
        if (he == NULL) {
            close(sock);
            return (-1);
        }
        memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
    }
    
    /* Build JOIN message */
    memset(&msg, 0, sizeof(msg));
    msg.type = GOSSIP_MSG_JOIN;
    msg.version = 1;
    strlcpy(msg.source_id, local_node->node_id, sizeof(msg.source_id));
    msg.source_incarnation = local_node->incarnation;
    msg.timestamp = time(NULL);
    
    /* Add local node info to payload */
    payload_len = sizeof(struct cluster_node);
    payload = malloc(payload_len);
    if (payload == NULL) {
        close(sock);
        return (-1);
    }
    memcpy(payload, local_node, sizeof(struct cluster_node));
    msg.length = payload_len;
    
    /* Send join request */
    struct iovec iov[2];
    iov[0].iov_base = &msg;
    iov[0].iov_len = offsetof(struct gossip_message, payload);
    iov[1].iov_base = payload;
    iov[1].iov_len = payload_len;
    
    struct msghdr mhdr;
    memset(&mhdr, 0, sizeof(mhdr));
    mhdr.msg_name = &addr;
    mhdr.msg_namelen = sizeof(addr);
    mhdr.msg_iov = iov;
    mhdr.msg_iovlen = 2;
    
    sent = sendmsg(sock, &mhdr, 0);
    free(payload);
    close(sock);
    
    if (sent < 0)
        return (-1);
    
    /* Start gossip protocol */
    return (gossip_start());
}

/*
 * Leave cluster
 */
int
cluster_leave(const char *reason)
{
    char payload[512];
    size_t payload_len;
    
    if (!cluster_initialized)
        return (-1);
    
    /* Broadcast DEAD message */
    snprintf(payload, sizeof(payload), "left: %s", reason ? reason : "normal shutdown");
    payload_len = strlen(payload) + 1;
    
    gossip_broadcast(GOSSIP_MSG_DEAD, payload, payload_len);
    
    /* Stop gossip */
    gossip_stop();
    
    /* Update local state */
    pthread_mutex_lock(&local_node->lock);
    local_node->state = NODE_STATE_LEFT;
    pthread_mutex_unlock(&local_node->lock);
    
    return (0);
}

/*
 * Add node to registry
 */
struct cluster_node *
cluster_node_add(const char *node_id, const char *ip, uint16_t port)
{
    struct cluster_node *node, *existing;
    
    if (node_id == NULL)
        return (NULL);
    
    pthread_mutex_lock(&node_registry_lock);
    
    node = calloc(1, sizeof(struct cluster_node));
    if (node == NULL) {
        pthread_mutex_unlock(&node_registry_lock);
        return (NULL);
    }
    
    strlcpy(node->node_id, node_id, sizeof(node->node_id));
    if (ip != NULL)
        strlcpy(node->ip, ip, sizeof(node->ip));
    node->port = port;
    node->state = NODE_STATE_JOINING;
    node->last_seen = time(NULL);
    node->incarnation = 1;
    node->weight = 1.0;
    
    pthread_mutex_init(&node->lock, NULL);
    
    existing = RB_FIND(node_tree, &node_registry, node);
    if (existing != NULL) {
        free(node);
        pthread_mutex_unlock(&node_registry_lock);
        return (existing);
    }
    
    RB_INSERT(node_tree, &node_registry, node);
    pthread_mutex_unlock(&node_registry_lock);
    
    /* Emit event */
    cluster_emit_event(1, node_id, "node joined cluster");
    
    return (node);
}

/*
 * Remove node from registry
 */
int
cluster_node_remove(const char *node_id)
{
    struct cluster_node node_find, *node;
    
    if (node_id == NULL)
        return (-1);
    
    strlcpy(node_find.node_id, node_id, sizeof(node_find.node_id));
    
    pthread_mutex_lock(&node_registry_lock);
    node = RB_FIND(node_tree, &node_registry, &node_find);
    if (node != NULL) {
        RB_REMOVE(node_tree, &node_registry, node);
        /*
         * Detach from the tree now, but only free once no other thread
         * holds a reference. If references remain, mark it removed and let
         * the last cluster_node_put free it.
         */
        if (node->refcount == 0) {
            pthread_mutex_destroy(&node->lock);
            free(node);
        } else {
            node->removed = true;
        }
    }
    pthread_mutex_unlock(&node_registry_lock);

    if (node != NULL) {
        cluster_emit_event(2, node_id, "node left cluster");
        return (0);
    }

    return (-1);
}

/*
 * Get node by ID. Returns a counted reference; the caller must release it
 * with cluster_node_put when done (except the local node, which is never
 * freed but is still safe to put).
 */
struct cluster_node *
cluster_node_get(const char *node_id)
{
    struct cluster_node node_find;
    struct cluster_node *node;

    if (node_id == NULL)
        return (NULL);

    /* Check local node first */
    if (local_node != NULL && strcmp(node_id, local_node->node_id) == 0)
        return (local_node);

    strlcpy(node_find.node_id, node_id, sizeof(node_find.node_id));

    pthread_mutex_lock(&node_registry_lock);
    node = RB_FIND(node_tree, &node_registry, &node_find);
    if (node != NULL)
        node->refcount++;
    pthread_mutex_unlock(&node_registry_lock);

    return (node);
}

/*
 * Release a reference obtained from cluster_node_get. Frees the node if it
 * has been removed from the tree and this was the last reference.
 */
void
cluster_node_put(struct cluster_node *node)
{
    if (node == NULL || node == local_node)
        return;

    pthread_mutex_lock(&node_registry_lock);
    if (node->refcount > 0)
        node->refcount--;
    if (node->refcount == 0 && node->removed) {
        pthread_mutex_unlock(&node_registry_lock);
        pthread_mutex_destroy(&node->lock);
        free(node);
        return;
    }
    pthread_mutex_unlock(&node_registry_lock);
}

/*
 * List all nodes
 */
struct cluster_node **
cluster_nodes_list(int *count)
{
    struct cluster_node **result;
    struct cluster_node *node;
    int alloc = 16;
    int n = 0;
    
    *count = 0;
    
    result = calloc(alloc, sizeof(struct cluster_node *));
    if (result == NULL)
        return (NULL);
    
    pthread_mutex_lock(&node_registry_lock);
    RB_FOREACH(node, node_tree, &node_registry) {
        if (n >= alloc) {
            alloc *= 2;
            void *_new = realloc(result, alloc * sizeof(struct cluster_node *));
            if (_new == NULL) {
                pthread_mutex_unlock(&node_registry_lock);
                *count = 0;
                return (NULL);
            }
            result = _new;
        }
        result[n++] = node;
    }
    pthread_mutex_unlock(&node_registry_lock);
    
    *count = n;
    return (result);
}

/*
 * List nodes by role
 */
struct cluster_node **
cluster_nodes_by_role(int role, int *count)
{
    struct cluster_node **all, **result;
    int all_count, alloc = 16;
    int n = 0, i;
    
    all = cluster_nodes_list(&all_count);
    if (all == NULL) {
        *count = 0;
        return (NULL);
    }
    
    result = calloc(alloc, sizeof(struct cluster_node *));
    if (result == NULL) {
        free(all);
        *count = 0;
        return (NULL);
    }
    
    for (i = 0; i < all_count; i++) {
        if (all[i]->role == role) {
            if (n >= alloc) {
                alloc *= 2;
                void *_new = realloc(result, alloc * sizeof(struct cluster_node *));
                if (_new == NULL) {
                    free(all);
                    *count = 0;
                    return (NULL);
                }
                result = _new;
            }
            result[n++] = all[i];
        }
    }
    
    free(all);
    *count = n;
    return (result);
}

/*
 * Start gossip protocol
 */
int
gossip_start(void)
{
    struct sockaddr_in addr;
    int reuse = 1;
    
    if (__sync_fetch_and_add(&gossip_running, 0) != 0)
        return (0);
    
    __sync_fetch_and_or(&gossip_running, 1);
    
    /* Create UDP socket */
    gossip_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (gossip_socket < 0)
        return (-1);
    
    setsockopt(gossip_socket, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    
    /* Bind to cluster port */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(cluster_conf.cluster_port);
    
    if (bind(gossip_socket, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(gossip_socket);
        gossip_socket = -1;
        __sync_fetch_and_and(&gossip_running, 0);
        return (-1);
    }
    
    /* Start gossip thread */
    pthread_create(&gossip_thread, NULL, gossip_loop, NULL);
    
    return (0);
}

/*
 * Stop gossip protocol
 */
int
gossip_stop(void)
{
    if (__sync_fetch_and_add(&gossip_running, 0) == 0)
        return (0);
    
    __sync_fetch_and_and(&gossip_running, 0);
    
    if (gossip_socket >= 0) {
        close(gossip_socket);
        gossip_socket = -1;
    }
    
    pthread_join(gossip_thread, NULL);
    
    return (0);
}

/*
 * Gossip loop - runs in separate thread
 */
static void *
gossip_loop(void *arg)
{
    uint8_t recv_buf[65536];
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);
    ssize_t n;
    
    (void)arg;
    
    while (__sync_fetch_and_add(&gossip_running, 0) != 0) {
        struct timeval tv;
        fd_set fds;
        
        FD_ZERO(&fds);
        FD_SET(gossip_socket, &fds);
        
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        int ready = select(gossip_socket + 1, &fds, NULL, NULL, &tv);
        if (ready <= 0)
            continue;
        
        n = recvfrom(gossip_socket, recv_buf, sizeof(recv_buf), 0,
            (struct sockaddr *)&sender_addr, &addr_len);
        
        if (n > 0) {
            gossip_handle_message(recv_buf, n, &sender_addr);
        }
    }
    
    return (NULL);
}

/*
 * Handle incoming gossip message
 */
static void
gossip_handle_message(uint8_t *buf, size_t len, struct sockaddr_in *sender)
{
    struct gossip_message *msg = (struct gossip_message *)buf;
    char sender_ip[64];
    
    inet_ntop(AF_INET, &sender->sin_addr, sender_ip, sizeof(sender_ip));

    /* Validate message */
    if (len < offsetof(struct gossip_message, payload))
        return;

    /*
     * source_id is a fixed char[256] filled from the wire and is not
     * guaranteed NUL-terminated; force a terminator before any string
     * operation to prevent an over-read past the field.
     */
    msg->source_id[sizeof(msg->source_id) - 1] = '\0';

    /* Update sender's last_seen */
    struct cluster_node *node = cluster_node_get(msg->source_id);
    if (node != NULL) {
        pthread_mutex_lock(&node->lock);
        node->last_seen = time(NULL);
        if (node->state == NODE_STATE_SUSPECTED)
            node->state = NODE_STATE_ACTIVE;
        pthread_mutex_unlock(&node->lock);
        cluster_node_put(node);
    }

    switch (msg->type) {
        case GOSSIP_MSG_JOIN:
            /*
             * New node joining. The payload is reinterpreted as a
             * struct cluster_node, so require that the received payload is
             * at least that large — otherwise the node_id/ip/port reads
             * would run past the received bytes into stale buffer data.
             */
            if (msg->length >= sizeof(struct cluster_node) &&
                len >= offsetof(struct gossip_message, payload) + msg->length) {
                struct cluster_node *new_node = (struct cluster_node *)msg->payload;
                new_node->node_id[sizeof(new_node->node_id) - 1] = '\0';
                new_node->ip[sizeof(new_node->ip) - 1] = '\0';
                cluster_node_add(new_node->node_id, new_node->ip, new_node->port);
            }
            break;
            
        case GOSSIP_MSG_ALIVE:
            /* Node is alive - update state */
            swim_mark_alive(msg->source_id);
            break;
            
        case GOSSIP_MSG_DEAD:
            /* Node is dead - remove from registry */
            cluster_node_remove(msg->source_id);
            break;
            
        case GOSSIP_MSG_HEARTBEAT:
            /* Process heartbeat for SWIM */
            swim_process_heartbeat(msg->source_id, msg->source_incarnation);
            break;
            
        case GOSSIP_MSG_STATE:
            /* Anti-entropy state exchange */
            gossip_process_state(msg->source_id, msg->payload, msg->length);
            break;

        case GOSSIP_MSG_VOTE_REQ:
            if (msg->length >= sizeof(struct raft_vote_req))
                raft_on_vote_req(msg->source_id,
                    (const struct raft_vote_req *)msg->payload);
            break;

        case GOSSIP_MSG_VOTE_RESP:
            if (msg->length >= sizeof(struct raft_vote_resp))
                raft_on_vote_resp(msg->source_id,
                    (const struct raft_vote_resp *)msg->payload);
            break;

        case GOSSIP_MSG_APPEND_REQ: {
            const struct raft_append_req *ar =
                (const struct raft_append_req *)msg->payload;

            /* Bound n_entries and require the payload actually carry them,
             * so the handler never reads past the received bytes. */
            if (msg->length >= sizeof(*ar) &&
                ar->n_entries <= RAFT_MAX_BATCH &&
                (size_t)msg->length >= sizeof(*ar) +
                (size_t)ar->n_entries * sizeof(struct raft_wire_entry))
                raft_on_append_req(msg->source_id, ar);
            break;
        }

        case GOSSIP_MSG_APPEND_RESP:
            if (msg->length >= sizeof(struct raft_append_resp))
                raft_on_append_resp(msg->source_id,
                    (const struct raft_append_resp *)msg->payload);
            break;

        case GOSSIP_MSG_INSTALL_SNAP:
            if (msg->length >= sizeof(struct raft_snap_req))
                raft_on_install_snap(msg->source_id,
                    (const struct raft_snap_req *)msg->payload);
            break;

        default:
            break;
    }
}

/*
 * Announce this node to the currently-known peers as a JOIN, so a running
 * cluster adds it to the membership. The JOIN handler reads node_id/ip/port
 * from the payload; the remaining struct fields are ignored on the wire.
 */
int
cluster_announce(void)
{
    if (local_node == NULL)
        return (-1);
    return (gossip_broadcast(GOSSIP_MSG_JOIN, local_node,
        sizeof(struct cluster_node)));
}

/*
 * Process state from anti-entropy
 */
static void
gossip_process_state(const char *source_id, uint8_t *payload, size_t len)
{
    /*
     * Anti-entropy state reconciliation is not yet implemented.
     * The gossip protocol delivers state digests to this function
     * (payload = serialized state, len = length), and we should:
     *
     *   1. Deserialize the incoming state
     *   2. Compare against local state (key-by-key, version-by-version)
     *   3. Pull missing/older keys from the source node
     *   4. Push newer keys to the source node
     *   5. Resolve conflicts using a tie-breaker (e.g., higher version,
     *      or node ID lex order)
     *
     * A full implementation is ~200 LOC of:
     * - State serialization (use existing cluster_state_* funcs)
     * - Merkle tree or version-vector comparison
     * - Pull/push request handlers (already exist as gossip_send_message)
     * - Conflict resolution logic
     *
     * For now, the function silently drops incoming state. This means
     * cluster nodes will diverge over time (no anti-entropy).
     * Single-node clusters work fine; multi-node clusters need this.
     *
     * See MIGRATION.md and the original SWIM paper for the algorithm:
     * https://www.cs.cornell.edu/projects/Quicksilver/public_pdfs/SWIM.pdf
     */
    (void)source_id;
    (void)payload;
    (void)len;
}

/*
 * Send message to specific node
 */
int
gossip_send_message(const char *target_id, uint8_t type, const void *payload, size_t len)
{
    struct cluster_node *target;
    struct sockaddr_in addr;
    struct gossip_message msg;
    struct iovec iov[2];
    struct msghdr mhdr;
    ssize_t sent;
    
    if (!cluster_initialized || target_id == NULL)
        return (-1);
    
    /* The wire length field is 16-bit; refuse oversized payloads rather
     * than silently truncating msg.length. */
    if (len > UINT16_MAX)
        return (-1);

    target = cluster_node_get(target_id);
    if (target == NULL)
        return (-1);

    /* Build message */
    memset(&msg, 0, sizeof(msg));
    msg.type = type;
    msg.version = 1;
    strlcpy(msg.source_id, local_node->node_id, sizeof(msg.source_id));
    msg.source_incarnation = local_node->incarnation;
    msg.timestamp = time(NULL);
    msg.length = (uint16_t)len;
    
    /* Set destination */
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(target->port);
    inet_pton(AF_INET, target->ip, &addr.sin_addr);
    
    /* Send */
    iov[0].iov_base = &msg;
    iov[0].iov_len = offsetof(struct gossip_message, payload);
    iov[1].iov_base = (void *)payload;
    iov[1].iov_len = len;
    
    memset(&mhdr, 0, sizeof(mhdr));
    mhdr.msg_name = &addr;
    mhdr.msg_namelen = sizeof(addr);
    mhdr.msg_iov = iov;
    mhdr.msg_iovlen = 2;
    
    pthread_mutex_lock(&gossip_lock);
    sent = sendmsg(gossip_socket, &mhdr, 0);
    pthread_mutex_unlock(&gossip_lock);

    cluster_node_put(target);
    return (sent >= 0 ? 0 : -1);
}

/*
 * Broadcast message to all nodes
 */
int
gossip_broadcast(uint8_t type, const void *payload, size_t len)
{
    struct cluster_node **nodes;
    int count, i;
    
    nodes = cluster_nodes_list(&count);
    if (nodes == NULL)
        return (-1);
    
    for (i = 0; i < count; i++) {
        if (strcmp(nodes[i]->node_id, local_node->node_id) != 0)
            gossip_send_message(nodes[i]->node_id, type, payload, len);
    }
    
    free(nodes);
    return (0);
}

/*
 * Initialize SWIM failure detector
 */
int
swim_init(void)
{
    memset(&swim, 0, sizeof(swim));
    swim.proto_version = 1;
    return (0);
}

/*
 * Process heartbeat from node
 */
int
swim_process_heartbeat(const char *node_id, uint64_t incarnation)
{
    struct cluster_node *node;
    
    node = cluster_node_get(node_id);
    if (node == NULL)
        return (-1);
    
    pthread_mutex_lock(&node->lock);
    
    /* Check incarnation - higher is better */
    if (incarnation > node->incarnation) {
        node->incarnation = incarnation;
        node->state = NODE_STATE_ACTIVE;
        node->suspicion_count = 0;
    }
    
    node->last_seen = time(NULL);
    pthread_mutex_unlock(&node->lock);

    cluster_node_put(node);
    return (0);
}

/*
 * Mark node as suspected
 */
int
swim_mark_suspected(const char *node_id)
{
    struct cluster_node *node;
    
    node = cluster_node_get(node_id);
    if (node == NULL)
        return (-1);
    
    pthread_mutex_lock(&node->lock);
    node->state = NODE_STATE_SUSPECTED;
    node->suspicion_count++;
    if (node->suspicion_started == 0)
        node->suspicion_started = time(NULL);
    pthread_mutex_unlock(&node->lock);

    cluster_node_put(node);

    /* Emit warning event */
    cluster_emit_event(3, node_id, "node suspected to be dead");

    return (0);
}

/*
 * Mark node as dead
 */
int
swim_mark_dead(const char *node_id)
{
    struct cluster_node *node;
    
    node = cluster_node_get(node_id);
    if (node == NULL)
        return (-1);
    
    pthread_mutex_lock(&node->lock);
    node->state = NODE_STATE_DEAD;
    pthread_mutex_unlock(&node->lock);

    /*
     * Remove from the registry. Our held reference makes cluster_node_remove
     * defer the free; releasing it below frees the node safely once no other
     * thread holds a reference.
     */
    cluster_node_remove(node_id);
    cluster_node_put(node);

    /* Emit failure event */
    cluster_emit_event(4, node_id, "node marked as dead");

    return (0);
}

/*
 * Mark node as alive
 */
int
swim_mark_alive(const char *node_id)
{
    struct cluster_node *node;
    
    node = cluster_node_get(node_id);
    if (node == NULL)
        return (-1);
    
    pthread_mutex_lock(&node->lock);
    if (node->state == NODE_STATE_SUSPECTED || node->state == NODE_STATE_DEAD) {
        node->state = NODE_STATE_ACTIVE;
        node->suspicion_count = 0;
        node->suspicion_started = 0;
        
        /* Emit recovery event */
        cluster_emit_event(5, node_id, "node recovered");
    }
    pthread_mutex_unlock(&node->lock);

    cluster_node_put(node);
    return (0);
}

/*
 * Initialize Raft consensus
 */
/*
 * Persistent state. Raft requires currentTerm, votedFor, and the log to
 * survive a crash (§5); commitIndex/lastApplied are volatile and rebuilt from
 * the leader after restart. The state file path comes from OCIFBSD_RAFT_STATE;
 * when unset, persistence is disabled (in-memory only).
 */
#define RAFT_PERSIST_MAGIC  0x52414654u   /* "RAFT" */
static char raft_state_path[1024];

/* Atomically write currentTerm, votedFor, and the log. Call with cluster_lock
 * held. Best-effort: an I/O failure is not fatal to the running node. */
static void
raft_persist(void)
{
    char tmp[1088];
    FILE *f;
    uint32_t magic = RAFT_PERSIST_MAGIC, ver = 1, n;
    int i, fd;

    if (raft_state_path[0] == '\0')
        return;
    snprintf(tmp, sizeof(tmp), "%s.tmp", raft_state_path);
    f = fopen(tmp, "wb");
    if (f == NULL)
        return;
    ver = 2;    /* v2 adds the snapshot boundary */
    n = (uint32_t)raft.log_size;
    fwrite(&magic, sizeof(magic), 1, f);
    fwrite(&ver, sizeof(ver), 1, f);
    fwrite(&raft.current_term, sizeof(raft.current_term), 1, f);
    fwrite(raft.voted_for, sizeof(raft.voted_for), 1, f);
    fwrite(&raft.snapshot_index, sizeof(raft.snapshot_index), 1, f);
    fwrite(&raft.snapshot_term, sizeof(raft.snapshot_term), 1, f);
    fwrite(&n, sizeof(n), 1, f);
    for (i = 0; i < raft.log_size; i++) {
        fwrite(&raft.log[i].term, sizeof(uint64_t), 1, f);
        fwrite(&raft.log[i].index, sizeof(uint64_t), 1, f);
        fwrite(raft.log[i].command, sizeof(raft.log[i].command), 1, f);
    }
    fflush(f);
    fd = fileno(f);
    if (fd >= 0)
        fsync(fd);
    fclose(f);
    rename(tmp, raft_state_path);   /* atomic replace */
}

/* Load persisted state at startup (no lock needed — pre-thread). */
static void
raft_load(void)
{
    FILE *f;
    uint32_t magic = 0, ver = 0, n = 0, i;

    if (raft_state_path[0] == '\0')
        return;
    f = fopen(raft_state_path, "rb");
    if (f == NULL)
        return;
    if (fread(&magic, sizeof(magic), 1, f) != 1 ||
        magic != RAFT_PERSIST_MAGIC) {
        fclose(f);
        return;
    }
    if (fread(&ver, sizeof(ver), 1, f) != 1 ||
        fread(&raft.current_term, sizeof(raft.current_term), 1, f) != 1 ||
        fread(raft.voted_for, sizeof(raft.voted_for), 1, f) != 1) {
        fclose(f);
        return;
    }
    if (ver >= 2) {     /* snapshot boundary (absent in v1) */
        if (fread(&raft.snapshot_index, sizeof(raft.snapshot_index), 1, f)
            != 1 ||
            fread(&raft.snapshot_term, sizeof(raft.snapshot_term), 1, f)
            != 1) {
            fclose(f);
            return;
        }
    }
    if (fread(&n, sizeof(n), 1, f) != 1) {
        fclose(f);
        return;
    }
    raft.voted_for[sizeof(raft.voted_for) - 1] = '\0';
    if (n > 0 && n < 1000000) {
        raft.log = calloc(n, sizeof(struct raft_log_entry));
        if (raft.log != NULL) {
            raft.log_capacity = (int)n;
            for (i = 0; i < n; i++) {
                if (fread(&raft.log[i].term, sizeof(uint64_t), 1, f) != 1 ||
                    fread(&raft.log[i].index, sizeof(uint64_t), 1, f) != 1 ||
                    fread(raft.log[i].command,
                        sizeof(raft.log[i].command), 1, f) != 1)
                    break;
                raft.log[i].command[sizeof(raft.log[i].command) - 1] = '\0';
                raft.log_size++;
            }
        }
    }
    fclose(f);
}

int
raft_init(void)
{
    const char *p;

    memset(&raft, 0, sizeof(raft));
    raft.state = RAFT_FOLLOWER;
    raft.current_term = 0;
    raft.commit_index = 0;      /* volatile: relearned from the leader */
    raft.last_applied = 0;
    raft.cp = cp_new();         /* replicated control-plane state */

    p = getenv("OCIFBSD_RAFT_STATE");
    if (p != NULL && p[0] != '\0')
        strlcpy(raft_state_path, p, sizeof(raft_state_path));
    raft_load();                /* restore currentTerm/votedFor/log if present */

    return (0);
}

/* ------------------------------------------------------------------ */
/* Raft election (RFC 8555-style RequestVote / AppendEntries)         */
/*                                                                    */
/* RequestVote and AppendEntries (heartbeat) ride the gossip UDP      */
/* transport; msg->source_id identifies the peer. Wire payloads use   */
/* host byte order — the cluster is homogeneous amd64. Log            */
/* replication, commit advancement, and on-disk persistence of        */
/* (currentTerm, votedFor, log) are layered on in later stages; this  */
/* stage delivers safe leader election with randomized timeouts.      */
/* ------------------------------------------------------------------ */

#define RAFT_ELECTION_MIN_MS   1500
#define RAFT_ELECTION_MAX_MS   3000
#define RAFT_HEARTBEAT_MS       400
#define RAFT_TICK_MS             50

static int      raft_running;
static pthread_t raft_thread;
static int      raft_votes_granted;
static int64_t  raft_election_deadline_ms;
static int64_t  raft_last_hb_ms;

static int64_t
raft_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* Must be called with cluster_lock held. */
static void
raft_reset_election_timer(void)
{
    raft_election_deadline_ms = raft_now_ms() + RAFT_ELECTION_MIN_MS +
        (int64_t)(random() % (RAFT_ELECTION_MAX_MS - RAFT_ELECTION_MIN_MS + 1));
}

/* Total cluster size = registered peers + self. Never call while holding
 * cluster_lock (cluster_nodes_list takes node_registry_lock). */
static int
raft_cluster_size(void)
{
    int count = 0;
    struct cluster_node **nodes = cluster_nodes_list(&count);

    free(nodes);
    return (count + 1);
}

/* Log tips — call with cluster_lock held. Indices are absolute; the log array
 * begins at absolute index snapshot_index + 1. */
static uint64_t
raft_last_log_index(void)
{
    return (raft.snapshot_index + (uint64_t)raft.log_size);
}
static uint64_t
raft_last_log_term(void)
{
    return (raft.log_size > 0 ? raft.log[raft.log_size - 1].term :
        raft.snapshot_term);
}
/* Term of the entry at absolute index idx: the snapshot term at the boundary,
 * the log entry's term if present, or 0 if unknown (compacted below the
 * snapshot, or beyond the log). */
static uint64_t
raft_term_at(uint64_t idx)
{
    if (idx == raft.snapshot_index)
        return (raft.snapshot_term);
    if (idx > raft.snapshot_index &&
        idx <= raft.snapshot_index + (uint64_t)raft.log_size)
        return (raft.log[idx - raft.snapshot_index - 1].term);
    return (0);
}

static void *
raft_loop(void *arg)
{
    (void)arg;
    while (__sync_fetch_and_add(&raft_running, 0) != 0) {
        int64_t now = raft_now_ms();
        int do_vote = 0, do_replicate = 0;
        struct raft_vote_req vreq;

        memset(&vreq, 0, sizeof(vreq));

        pthread_mutex_lock(&cluster_lock);
        if (raft.state == RAFT_LEADER) {
            if (now - raft_last_hb_ms >= RAFT_HEARTBEAT_MS) {
                raft_last_hb_ms = now;
                do_replicate = 1;
            }
        } else if (now >= raft_election_deadline_ms) {
            /* Election timeout — start a new election. */
            raft.state = RAFT_CANDIDATE;
            raft.current_term++;
            strlcpy(raft.voted_for, local_node->node_id,
                sizeof(raft.voted_for));
            raft_votes_granted = 1;             /* vote for self */
            raft_reset_election_timer();
            vreq.term = raft.current_term;
            vreq.last_log_index = raft_last_log_index();
            vreq.last_log_term = raft_last_log_term();
            raft_persist();     /* term++ and self-vote before soliciting */
            do_vote = 1;
        }
        pthread_mutex_unlock(&cluster_lock);

        if (do_replicate) {
            raft_replicate_to_peers();
            /* Re-evaluate commit each tick so a leader that is itself a
             * majority (e.g. a single-node cluster, where no AppendEntries
             * replies arrive) still advances its commit index. */
            raft_advance_commit();
        }
        if (do_vote) {
            gossip_broadcast(GOSSIP_MSG_VOTE_REQ, &vreq, sizeof(vreq));
            /*
             * A candidate's self-vote may already be a majority (e.g. a
             * single-node cluster), in which case no vote responses will ever
             * arrive to trigger promotion — so check for a win right here.
             */
            int majority = raft_cluster_size() / 2 + 1;
            int won = 0;

            pthread_mutex_lock(&cluster_lock);
            if (raft.state == RAFT_CANDIDATE &&
                raft_votes_granted >= majority) {
                raft.state = RAFT_LEADER;
                strlcpy(raft.leader_id, local_node->node_id,
                    sizeof(raft.leader_id));
                raft_last_hb_ms = 0;
                won = 1;
            }
            pthread_mutex_unlock(&cluster_lock);
            if (won)
                cluster_emit_event(6, local_node->node_id, "became leader");
        }

        usleep(RAFT_TICK_MS * 1000);
    }
    return (NULL);
}

/* Handle an inbound RequestVote. */
static void
raft_on_vote_req(const char *from, const struct raft_vote_req *r)
{
    struct raft_vote_resp resp;
    uint8_t grant = 0;
    uint64_t t0;

    memset(&resp, 0, sizeof(resp));
    pthread_mutex_lock(&cluster_lock);
    t0 = raft.current_term;
    if (r->term > raft.current_term) {      /* newer term: step down */
        raft.current_term = r->term;
        raft.state = RAFT_FOLLOWER;
        raft.voted_for[0] = '\0';
    }
    if (r->term == raft.current_term &&
        (raft.voted_for[0] == '\0' || strcmp(raft.voted_for, from) == 0) &&
        (r->last_log_term > raft_last_log_term() ||
         (r->last_log_term == raft_last_log_term() &&
          r->last_log_index >= raft_last_log_index()))) {
        grant = 1;
        strlcpy(raft.voted_for, from, sizeof(raft.voted_for));
        raft_reset_election_timer();
    }
    resp.term = raft.current_term;
    resp.granted = grant;
    /* Persist only when currentTerm or votedFor actually changed — avoids an
     * fsync on every rejected/duplicate RequestVote. */
    if (grant || raft.current_term != t0)
        raft_persist();
    pthread_mutex_unlock(&cluster_lock);

    gossip_send_message(from, GOSSIP_MSG_VOTE_RESP, &resp, sizeof(resp));
}

/* Handle a RequestVote reply. */
static void
raft_on_vote_resp(const char *from, const struct raft_vote_resp *r)
{
    int majority = raft_cluster_size() / 2 + 1;
    uint8_t became = 0;

    (void)from;
    pthread_mutex_lock(&cluster_lock);
    if (r->term > raft.current_term) {
        raft.current_term = r->term;
        raft.state = RAFT_FOLLOWER;
        raft.voted_for[0] = '\0';
        raft_reset_election_timer();
        raft_persist();
    } else if (raft.state == RAFT_CANDIDATE && r->term == raft.current_term &&
        r->granted) {
        raft_votes_granted++;
        if (raft_votes_granted >= majority) {
            raft.state = RAFT_LEADER;
            strlcpy(raft.leader_id, local_node->node_id,
                sizeof(raft.leader_id));
            raft_last_hb_ms = 0;        /* send a heartbeat immediately */
            became = 1;
        }
    }
    pthread_mutex_unlock(&cluster_lock);

    if (became) {
        /* Initialize per-peer replication cursors: nextIndex = leader's
         * last log index + 1, matchIndex = 0 (Raft §5.3). */
        struct cluster_node **peers;
        int count, i;

        peers = cluster_nodes_list(&count);
        if (peers != NULL) {
            pthread_mutex_lock(&cluster_lock);
            for (i = 0; i < count; i++) {
                peers[i]->raft_next_index = raft_last_log_index() + 1;
                peers[i]->raft_match_index = 0;
            }
            pthread_mutex_unlock(&cluster_lock);
            free(peers);
        }
        cluster_emit_event(6, local_node->node_id, "became leader");
    }
}

/*
 * Handle an inbound AppendEntries: term check, log-consistency check against
 * (prev_log_index, prev_log_term), append/overwrite the carried entries, and
 * advance the follower commit index to leader_commit. The caller has already
 * bounded n_entries and verified the payload carries that many wire entries.
 */
static void
raft_on_append_req(const char *from, const struct raft_append_req *r)
{
    const struct raft_wire_entry *we =
        (const struct raft_wire_entry *)((const uint8_t *)r + sizeof(*r));
    struct raft_append_resp resp;
    uint32_t k;
    uint64_t t0;
    int appended = 0;

    memset(&resp, 0, sizeof(resp));
    pthread_mutex_lock(&cluster_lock);
    t0 = raft.current_term;

    if (r->term < raft.current_term) {          /* stale leader: reject */
        resp.term = raft.current_term;
        resp.success = 0;
        resp.match_index = raft_last_log_index();
        goto out;
    }
    /* Valid leader for this term: (re)establish follower state. */
    raft.current_term = r->term;
    raft.state = RAFT_FOLLOWER;
    strlcpy(raft.leader_id, from, sizeof(raft.leader_id));
    raft_reset_election_timer();

    /*
     * Log matching: the entry preceding the new ones must agree. Anything at
     * or below our snapshot boundary is already committed, so only verify when
     * prev_log_index is above it.
     */
    if (r->prev_log_index > raft.snapshot_index &&
        (r->prev_log_index > raft_last_log_index() ||
         raft_term_at(r->prev_log_index) != r->prev_log_term)) {
        resp.term = raft.current_term;
        resp.success = 0;
        resp.match_index = raft_last_log_index();
        if (raft.current_term != t0)
            raft_persist();     /* only if currentTerm advanced above */
        goto out;
    }

    /* Append entries, truncating on the first conflicting term (Raft §5.3).
     * Indices are absolute; array position is idx - snapshot_index - 1. */
    for (k = 0; k < r->n_entries; k++) {
        uint64_t idx = r->prev_log_index + 1 + k;
        uint64_t pos;

        if (idx <= raft.snapshot_index)
            continue;                          /* already compacted/committed */
        pos = idx - raft.snapshot_index - 1;
        if (pos < (uint64_t)raft.log_size) {
            if (raft.log[pos].term == we[k].term)
                continue;                      /* already present */
            raft.log_size = (int)pos;          /* conflict: truncate */
            appended = 1;
        }
        if (raft.log_size >= raft.log_capacity) {
            int nc = raft.log_capacity ? raft.log_capacity * 2 : 16;
            struct raft_log_entry *g = realloc(raft.log,
                nc * sizeof(struct raft_log_entry));

            if (g == NULL)
                break;
            raft.log = g;
            raft.log_capacity = nc;
        }
        raft.log[raft.log_size].term = we[k].term;
        raft.log[raft.log_size].index = idx;
        strlcpy(raft.log[raft.log_size].command, we[k].command,
            sizeof(raft.log[raft.log_size].command));
        raft.log[raft.log_size].timestamp = time(NULL);
        raft.log_size++;
        appended = 1;
    }

    /* Advance commit index toward the leader's, bounded by our log. */
    if (r->leader_commit > raft.commit_index) {
        uint64_t last = raft_last_log_index();

        raft.commit_index = r->leader_commit < last ? r->leader_commit : last;
        raft_apply_committed();
    }

    /* Persist only when the term advanced or the log changed — a bare
     * heartbeat (no new entries, same term) needs no fsync. */
    if (appended || raft.current_term != t0)
        raft_persist();
    resp.term = raft.current_term;
    resp.success = 1;
    resp.match_index = raft_last_log_index();
out:
    pthread_mutex_unlock(&cluster_lock);
    gossip_send_message(from, GOSSIP_MSG_APPEND_RESP, &resp, sizeof(resp));
    raft_maybe_compact();       /* cap the log once entries commit (no lock) */
}

/* Handle an AppendEntries reply: update the peer's next/match cursors and try
 * to advance the commit index. */
static void
raft_on_append_resp(const char *from, const struct raft_append_resp *r)
{
    struct cluster_node *p;
    int stepped_down = 0;

    pthread_mutex_lock(&cluster_lock);
    if (r->term > raft.current_term) {          /* newer term: step down */
        raft.current_term = r->term;
        raft.state = RAFT_FOLLOWER;
        raft.voted_for[0] = '\0';
        raft_reset_election_timer();
        raft_persist();
        stepped_down = 1;
    }
    if (stepped_down || raft.state != RAFT_LEADER ||
        r->term != raft.current_term) {
        pthread_mutex_unlock(&cluster_lock);
        return;
    }
    pthread_mutex_unlock(&cluster_lock);

    p = cluster_node_get(from);
    if (p == NULL)
        return;
    pthread_mutex_lock(&cluster_lock);
    if (r->success) {
        if (r->match_index + 1 > p->raft_next_index)
            p->raft_next_index = r->match_index + 1;
        if (r->match_index > p->raft_match_index)
            p->raft_match_index = r->match_index;
    } else if (p->raft_next_index > 1) {
        p->raft_next_index--;                   /* back off, retry next tick */
    }
    pthread_mutex_unlock(&cluster_lock);
    cluster_node_put(p);

    raft_advance_commit();
}

/*
 * Leader: replicate to each peer. For every peer, send the entries starting at
 * its nextIndex (capped to RAFT_MAX_BATCH), with the preceding (index, term)
 * for the consistency check and the current commit index. A peer with nothing
 * outstanding still gets a zero-entry AppendEntries, which serves as the
 * heartbeat.
 */
static void
raft_replicate_to_peers(void)
{
    struct cluster_node **peers;
    int count, i;

    peers = cluster_nodes_list(&count);
    if (peers == NULL)
        return;
    for (i = 0; i < count; i++) {
        uint8_t buf[sizeof(struct raft_append_req) +
            RAFT_MAX_BATCH * sizeof(struct raft_wire_entry)];
        struct raft_append_req *req = (struct raft_append_req *)buf;
        struct raft_wire_entry *we =
            (struct raft_wire_entry *)(buf + sizeof(*req));
        char target[256];
        uint32_t nsent = 0;
        uint64_t ni, prev_index, idx;
        size_t len;
        int send_snap = 0;
        struct raft_snap_req snap;

        memset(&snap, 0, sizeof(snap));
        pthread_mutex_lock(&cluster_lock);
        if (raft.state != RAFT_LEADER) {
            pthread_mutex_unlock(&cluster_lock);
            break;
        }
        strlcpy(target, peers[i]->node_id, sizeof(target));
        /* A peer that appeared after this node became leader (a membership
         * change) has an uninitialized (0) cursor — start it optimistically
         * at our last index + 1; the normal back-off backfills it. */
        if (peers[i]->raft_next_index == 0)
            peers[i]->raft_next_index = raft_last_log_index() + 1;
        ni = peers[i]->raft_next_index;
        if (ni < 1)
            ni = 1;
        /*
         * If the entries this peer needs (from prev_index = ni-1) have been
         * compacted away, send an InstallSnapshot to fast-forward it to the
         * snapshot boundary instead of AppendEntries.
         */
        if (ni <= raft.snapshot_index) {
            snap.term = raft.current_term;
            snap.last_index = raft.snapshot_index;
            snap.last_term = raft.snapshot_term;
            send_snap = 1;
            pthread_mutex_unlock(&cluster_lock);
            gossip_send_message(target, GOSSIP_MSG_INSTALL_SNAP, &snap,
                sizeof(snap));
            continue;
        }
        prev_index = ni - 1;
        req->term = raft.current_term;
        req->prev_log_index = prev_index;
        req->prev_log_term = raft_term_at(prev_index);
        req->leader_commit = raft.commit_index;
        for (idx = ni; idx <= raft_last_log_index() &&
            nsent < RAFT_MAX_BATCH; idx++) {
            uint64_t pos = idx - raft.snapshot_index - 1;

            we[nsent].term = raft.log[pos].term;
            we[nsent].index = raft.log[pos].index;
            strlcpy(we[nsent].command, raft.log[pos].command,
                sizeof(we[nsent].command));
            nsent++;
        }
        req->n_entries = nsent;
        pthread_mutex_unlock(&cluster_lock);

        (void)send_snap;
        len = sizeof(*req) + (size_t)nsent * sizeof(struct raft_wire_entry);
        gossip_send_message(target, GOSSIP_MSG_APPEND_REQ, buf, len);
    }
    free(peers);
}

/*
 * Apply newly-committed log entries to the control-plane state machine, in
 * order, advancing last_applied. Idempotent and deterministic: every node
 * applies the same committed command sequence and converges. The caller must
 * hold cluster_lock. Non-control-plane commands are simply ignored by
 * cp_apply(), so ordinary Raft traffic is unaffected.
 */
static void
raft_apply_committed(void)
{
    while (raft.last_applied < raft.commit_index) {
        uint64_t idx = raft.last_applied + 1;
        int pos;

        if (idx <= raft.snapshot_index) {       /* compacted away */
            raft.last_applied = idx;
            continue;
        }
        pos = (int)(idx - raft.snapshot_index - 1);
        if (pos < 0 || pos >= raft.log_size)
            break;                              /* not present yet */
        if (raft.cp != NULL)
            (void)cp_apply(raft.cp, raft.log[pos].command);
        raft.last_applied = idx;
    }
}

/*
 * Leader: advance commit index to the highest N replicated on a majority,
 * restricted to entries from the current term (Raft §5.4.2).
 */
static void
raft_advance_commit(void)
{
    struct cluster_node **peers;
    int count, i;

    peers = cluster_nodes_list(&count);
    pthread_mutex_lock(&cluster_lock);
    if (raft.state == RAFT_LEADER) {
        int majority = (count + 1) / 2 + 1;     /* +1 for the leader */
        uint64_t N;                             /* absolute index */

        for (N = raft_last_log_index(); N > raft.commit_index &&
            N > raft.snapshot_index; N--) {
            int cnt = 1;                        /* leader has entry N */

            if (raft_term_at(N) != raft.current_term)
                continue;
            for (i = 0; peers != NULL && i < count; i++)
                if (peers[i]->raft_match_index >= N)
                    cnt++;
            if (cnt >= majority) {
                raft.commit_index = N;
                raft_apply_committed();
                break;
            }
        }
    }
    pthread_mutex_unlock(&cluster_lock);
    free(peers);

    raft_maybe_compact();       /* leader compacts once entries commit */
}

/*
 * Compact the log once enough committed entries have accumulated: snapshot up
 * to commit_index and discard those entries. Call WITHOUT cluster_lock held.
 * Threshold-gated so compaction is periodic, not per-entry.
 */
#define RAFT_COMPACT_THRESHOLD  32

static void
raft_maybe_compact(void)
{
    pthread_mutex_lock(&cluster_lock);
    if (raft.commit_index > raft.snapshot_index &&
        raft.commit_index - raft.snapshot_index >= RAFT_COMPACT_THRESHOLD) {
        uint64_t upto = raft.commit_index;
        uint64_t newterm = raft_term_at(upto);
        int discard = (int)(upto - raft.snapshot_index);

        if (discard > 0 && discard <= raft.log_size) {
            memmove(raft.log, raft.log + discard,
                (raft.log_size - discard) * sizeof(struct raft_log_entry));
            raft.log_size -= discard;
            raft.snapshot_index = upto;
            raft.snapshot_term = newterm;
            raft_persist();
        }
    }
    pthread_mutex_unlock(&cluster_lock);
}

/*
 * Follower: install a snapshot from the leader. Our state machine is the log,
 * so installing means adopting the (index, term) boundary, discarding the now
 * -superseded log, and advancing commit to the snapshot. A real state machine
 * would also load the shipped state blob here.
 */
static void
raft_on_install_snap(const char *from, const struct raft_snap_req *r)
{
    struct raft_append_resp resp;

    memset(&resp, 0, sizeof(resp));
    pthread_mutex_lock(&cluster_lock);
    if (r->term < raft.current_term) {
        resp.term = raft.current_term;
        resp.success = 0;
        resp.match_index = raft_last_log_index();
        pthread_mutex_unlock(&cluster_lock);
        gossip_send_message(from, GOSSIP_MSG_APPEND_RESP, &resp, sizeof(resp));
        return;
    }
    raft.current_term = r->term;
    raft.state = RAFT_FOLLOWER;
    strlcpy(raft.leader_id, from, sizeof(raft.leader_id));
    raft_reset_election_timer();

    if (r->last_index > raft.snapshot_index) {
        /* Adopt the snapshot: drop the entire in-memory log (any suffix will
         * be re-replicated by AppendEntries from last_index+1). */
        raft.log_size = 0;
        raft.snapshot_index = r->last_index;
        raft.snapshot_term = r->last_term;
        if (raft.commit_index < r->last_index)
            raft.commit_index = r->last_index;
        if (raft.last_applied < r->last_index)
            raft.last_applied = r->last_index;
        raft_persist();
    }
    resp.term = raft.current_term;
    resp.success = 1;
    resp.match_index = raft_last_log_index();
    pthread_mutex_unlock(&cluster_lock);
    gossip_send_message(from, GOSSIP_MSG_APPEND_RESP, &resp, sizeof(resp));
}

int
raft_start(void)
{
    if (__sync_fetch_and_add(&raft_running, 0) != 0)
        return (0);
    /* Seed the PRNG per-process so nodes pick different election timeouts. */
    srandom((unsigned)(time(NULL) ^ (getpid() << 8)));
    pthread_mutex_lock(&cluster_lock);
    raft_reset_election_timer();
    raft_last_hb_ms = 0;
    pthread_mutex_unlock(&cluster_lock);
    __sync_fetch_and_or(&raft_running, 1);
    if (pthread_create(&raft_thread, NULL, raft_loop, NULL) != 0) {
        __sync_fetch_and_and(&raft_running, 0);
        return (-1);
    }
    return (0);
}

int
raft_stop(void)
{
    if (__sync_fetch_and_add(&raft_running, 0) == 0)
        return (0);
    __sync_fetch_and_and(&raft_running, 0);
    pthread_join(raft_thread, NULL);
    cp_free(raft.cp);
    raft.cp = NULL;
    return (0);
}

/*
 * Query the replicated control-plane state. Returns the desired replica count
 * for a service, or -1 if unknown. Thread-safe.
 */
int
cluster_service_replicas(const char *service)
{
    int r;

    pthread_mutex_lock(&cluster_lock);
    r = cp_service_replicas(raft.cp, service);
    pthread_mutex_unlock(&cluster_lock);
    return (r);
}

/*
 * Propose a control-plane command (CREATE/SCALE/DELETE/ASSIGN/UNASSIGN) to the
 * cluster. Only the leader may propose; the command is appended to the Raft
 * log, replicated, and applied on every node once committed. Returns 0 on
 * success, -1 if not the leader or on error.
 */
int
cluster_cp_propose(const char *command)
{
    int is_leader;

    if (command == NULL || command[0] == '\0')
        return (-1);
    pthread_mutex_lock(&cluster_lock);
    is_leader = (raft.state == RAFT_LEADER);
    pthread_mutex_unlock(&cluster_lock);
    if (!is_leader)
        return (-1);
    return (raft_append_entry(command, strlen(command) + 1));
}

/*
 * Leader controller tick: plan placements for the desired state across the
 * given nodes and propose the resulting ASSIGN/UNASSIGN commands into the log.
 * A no-op on a follower. Returns the number of commands proposed. Idempotent:
 * once placements match the desired state the plan is empty.
 */
int
cluster_controller_tick(const char *const *nodes, int nnodes)
{
    char plan[64][256];
    int nplan = 0, proposed = 0, is_leader;

    pthread_mutex_lock(&cluster_lock);
    is_leader = (raft.state == RAFT_LEADER);
    if (is_leader && raft.cp != NULL)
        (void)controller_plan(raft.cp, nodes, nnodes, plan, 64, &nplan);
    pthread_mutex_unlock(&cluster_lock);

    if (!is_leader)
        return (0);
    for (int i = 0; i < nplan; i++)
        if (cluster_cp_propose(plan[i]) == 0)
            proposed++;
    return (proposed);
}

/* Total placements in the replicated state (thread-safe). */
int
cluster_placement_count(void)
{
    int n;

    pthread_mutex_lock(&cluster_lock);
    n = cp_placement_count(raft.cp);
    pthread_mutex_unlock(&cluster_lock);
    return (n);
}

/*
 * The assignment set for a node: the replicas the replicated state places on
 * it, ready for that node's agent. Thread-safe. Returns 0 on success.
 */
int
cluster_node_assignments(const char *node, struct agent_replica *out, int max,
    int *nout)
{
    int rc;

    pthread_mutex_lock(&cluster_lock);
    rc = controller_node_assignments(raft.cp, node, out, max, nout);
    pthread_mutex_unlock(&cluster_lock);
    return (rc);
}

int
raft_role(void)
{
    int s;

    pthread_mutex_lock(&cluster_lock);
    s = raft.state;
    pthread_mutex_unlock(&cluster_lock);
    return (s);
}

uint64_t
raft_term(void)
{
    uint64_t t;

    pthread_mutex_lock(&cluster_lock);
    t = raft.current_term;
    pthread_mutex_unlock(&cluster_lock);
    return (t);
}

uint64_t
raft_commit_index(void)
{
    uint64_t c;

    pthread_mutex_lock(&cluster_lock);
    c = raft.commit_index;
    pthread_mutex_unlock(&cluster_lock);
    return (c);
}

int
raft_log_len(void)
{
    int n;

    pthread_mutex_lock(&cluster_lock);
    n = raft.log_size;
    pthread_mutex_unlock(&cluster_lock);
    return (n);
}

uint64_t
raft_snapshot_index(void)
{
    uint64_t s;

    pthread_mutex_lock(&cluster_lock);
    s = raft.snapshot_index;
    pthread_mutex_unlock(&cluster_lock);
    return (s);
}

/*
 * Become leader
 */
int
raft_become_leader(void)
{
    pthread_mutex_lock(&cluster_lock);
    raft.state = RAFT_LEADER;
    raft.last_heartbeat = time(NULL);
    pthread_mutex_unlock(&cluster_lock);
    
    cluster_emit_event(6, local_node->node_id, "became leader");
    
    return (0);
}

/*
 * Become follower
 */
int
raft_become_follower(const char *leader_id)
{
    pthread_mutex_lock(&cluster_lock);
    raft.state = RAFT_FOLLOWER;
    if (leader_id != NULL)
        strlcpy(raft.leader_id, leader_id, sizeof(raft.leader_id));
    raft.voted_for[0] = '\0';
    pthread_mutex_unlock(&cluster_lock);
    
    return (0);
}

/*
 * Become candidate
 */
int
raft_become_candidate(void)
{
    pthread_mutex_lock(&cluster_lock);
    raft.state = RAFT_CANDIDATE;
    raft.current_term++;
    strlcpy(raft.voted_for, local_node->node_id, sizeof(raft.voted_for));
    pthread_mutex_unlock(&cluster_lock);
    
    return (0);
}

/*
 * Append entry to Raft log
 */
int
raft_append_entry(const char *command, size_t len)
{
    struct raft_log_entry *entry;
    
    pthread_mutex_lock(&cluster_lock);
    
    /* Grow log if needed */
    if (raft.log_size >= raft.log_capacity) {
        int new_cap = raft.log_capacity ? raft.log_capacity * 2 : 16;
        struct raft_log_entry *grown = realloc(raft.log,
            new_cap * sizeof(struct raft_log_entry));
        /* On failure keep the existing log intact rather than leaking it
         * (and losing every prior entry) by overwriting raft.log with NULL. */
        if (grown == NULL) {
            pthread_mutex_unlock(&cluster_lock);
            return (-1);
        }
        raft.log = grown;
        raft.log_capacity = new_cap;
    }
    
    entry = &raft.log[raft.log_size];
    entry->term = raft.current_term;
    entry->index = raft_last_log_index() + 1;   /* absolute (snapshot-aware) */
    strlcpy(entry->command, command, sizeof(entry->command));
    entry->timestamp = time(NULL);

    raft.log_size++;
    raft_persist();     /* durable log before the entry is replicated */

    pthread_mutex_unlock(&cluster_lock);

    return (0);
}

/*
 * Commit log entry
 */
int
raft_commit_log(uint64_t index)
{
    pthread_mutex_lock(&cluster_lock);
    
    if (index <= raft.commit_index || index > (uint64_t)raft.log_size) {
        pthread_mutex_unlock(&cluster_lock);
        return (-1);
    }
    
    raft.commit_index = index;
    raft.last_applied = index;
    
    pthread_mutex_unlock(&cluster_lock);
    
    return (0);
}

/*
 * Get current leader
 */
int
raft_get_leader(char *leader_id, size_t len)
{
    pthread_mutex_lock(&cluster_lock);
    
    if (raft.state == RAFT_LEADER) {
        strlcpy(leader_id, local_node->node_id, len);
    } else {
        strlcpy(leader_id, raft.leader_id, len);
    }
    
    pthread_mutex_unlock(&cluster_lock);
    
    return (raft.state == RAFT_LEADER || raft.leader_id[0] != '\0' ? 0 : -1);
}

/*
 * Set event callback
 */
int
cluster_set_event_callback(cluster_event_cb callback)
{
    event_callback = callback;
    return (0);
}

/*
 * Emit cluster event
 */
int
cluster_emit_event(int event_type, const char *node_id, const char *details)
{
    struct cluster_event event;
    
    if (node_id != NULL)
        strlcpy(event.node_id, node_id, sizeof(event.node_id));
    event.event_type = event_type;
    event.timestamp = time(NULL);
    if (details != NULL)
        strlcpy(event.details, details, sizeof(event.details));
    
    if (event_callback != NULL)
        event_callback(&event);
    
    return (0);
}

/*
 * Get cluster status as JSON
 */
int
cluster_status_json(char **json_out, size_t *json_len)
{
    char *json;
    size_t json_size;
    
    if (json_out == NULL)
        return (-1);
    
    json_size = 4096;
    json = malloc(json_size);
    if (json == NULL)
        return (-1);
    
    snprintf(json, json_size,
        "{\n"
        "  \"cluster_name\": \"%s\",\n"
        "  \"local_node\": \"%s\",\n"
        "  \"raft_state\": \"%s\",\n"
        "  \"raft_term\": %lu,\n"
        "  \"raft_leader\": \"%s\"\n"
        "}",
        cluster_conf.cluster_name,
        local_node ? local_node->node_id : "unknown",
        raft.state == RAFT_LEADER ? "leader" :
        raft.state == RAFT_CANDIDATE ? "candidate" : "follower",
        (unsigned long)raft.current_term,
        raft.leader_id);
    
    *json_out = json;
    if (json_len != NULL)
        *json_len = strlen(json);
    
    return (0);
}

/*
 * Get nodes as JSON
 */
int
cluster_nodes_json(char **json_out, size_t *json_len)
{
    struct cluster_node **nodes;
    char *json, *p;
    size_t json_size;
    int count, i;

    if (json_out == NULL)
        return (-1);
    
    nodes = cluster_nodes_list(&count);
    if (nodes == NULL) {
        *json_out = strdup("{\"nodes\":[]}");
        if (*json_out != NULL && json_len != NULL)
            *json_len = strlen(*json_out);
        return (*json_out != NULL ? 0 : -1);
    }
    
    json_size = 4096 + (size_t)count * 1024;
    json = malloc(json_size);
    if (json == NULL) {
        free(nodes);
        return (-1);
    }

    p = json;
    size_t remaining = json_size;
    int n;

    n = snprintf(p, remaining, "{\n  \"nodes\": [\n");
    if (n < 0 || (size_t)n >= remaining) { free(json); free(nodes); return (-1); }
    p += n; remaining -= (size_t)n;

    for (i = 0; i < count; i++) {
        pthread_mutex_lock(&nodes[i]->lock);
        n = snprintf(p, remaining,
            "    {\"id\":\"%s\",\"ip\":\"%s\",\"role\":%d,\"state\":%d,\"last_seen\":%ld}%s\n",
            nodes[i]->node_id,
            nodes[i]->ip,
            nodes[i]->role,
            nodes[i]->state,
            (long)nodes[i]->last_seen,
            i < count - 1 ? "," : "");
        pthread_mutex_unlock(&nodes[i]->lock);
        if (n < 0 || (size_t)n >= remaining) { free(json); free(nodes); return (-1); }
        p += n; remaining -= (size_t)n;
    }

    n = snprintf(p, remaining, "  ]\n}");
    if (n < 0 || (size_t)n >= remaining) { free(json); free(nodes); return (-1); }

    free(nodes);
    *json_out = json;
    if (json_len != NULL)
        *json_len = strlen(json);

    return (0);
}

/*
 * Get cluster health as JSON
 */
int
cluster_health_json(char **json_out, size_t *json_len)
{
    struct cluster_node **nodes;
    int count, i;
    int healthy = 0, unhealthy = 0, total = 0;
    time_t now = time(NULL);
    
    if (json_out == NULL)
        return (-1);
    
    nodes = cluster_nodes_list(&count);
    
    if (nodes != NULL) {
        for (i = 0; i < count; i++) {
            total++;
            pthread_mutex_lock(&nodes[i]->lock);
            if (nodes[i]->state == NODE_STATE_ACTIVE &&
                (now - nodes[i]->last_seen) < 30) {
                healthy++;
            } else {
                unhealthy++;
            }
            pthread_mutex_unlock(&nodes[i]->lock);
        }
        free(nodes);
    }
    
    char *json;
    asprintf(&json,
        "{\n"
        "  \"healthy\": %d,\n"
        "  \"unhealthy\": %d,\n"
        "  \"total\": %d,\n"
        "  \"status\": \"%s\"\n"
        "}",
        healthy, unhealthy, total,
        unhealthy == 0 ? "healthy" : "degraded");
    
    *json_out = json;
    if (json_len != NULL)
        *json_len = strlen(json);
    
    return (0);
}

/*
 * Register service for discovery
 */
int
cluster_register_service(const char *name, const char *ns, const char *ip, uint16_t port, const char *protocol)
{
    struct service_endpoint *svc;
    
    pthread_mutex_lock(&service_lock);
    
    service_registry = realloc(service_registry,
        (n_services + 1) * sizeof(struct service_endpoint *));
    if (service_registry == NULL) {
        pthread_mutex_unlock(&service_lock);
        return (-1);
    }
    
    svc = calloc(1, sizeof(struct service_endpoint));
    if (svc == NULL) {
        pthread_mutex_unlock(&service_lock);
        return (-1);
    }
    
    strlcpy(svc->service_name, name, sizeof(svc->service_name));
    if (ns != NULL)
        strlcpy(svc->namespace, ns, sizeof(svc->namespace));
    if (ip != NULL)
        strlcpy(svc->ip, ip, sizeof(svc->ip));
    svc->port = port;
    if (protocol != NULL)
        strlcpy(svc->protocol, protocol, sizeof(svc->protocol));
    svc->last_updated = time(NULL);
    
    service_registry[n_services++] = svc;
    
    pthread_mutex_unlock(&service_lock);
    
    /* Broadcast service registration */
    char payload[512];
    snprintf(payload, sizeof(payload), "register:%s:%s:%s:%d",
        name, ns ? ns : "default", ip, port);
    gossip_broadcast(GOSSIP_MSG_USER_DATA, payload, strlen(payload) + 1);
    
    return (0);
}

/*
 * Unregister service
 */
int
cluster_unregister_service(const char *name, const char *ns)
{
    int i, j;
    
    pthread_mutex_lock(&service_lock);
    
    for (i = 0; i < n_services; i++) {
        if (strcmp(service_registry[i]->service_name, name) == 0 &&
            (ns == NULL || strcmp(service_registry[i]->namespace, ns) == 0)) {
            
            free(service_registry[i]);
            for (j = i; j < n_services - 1; j++) {
                service_registry[j] = service_registry[j + 1];
            }
            n_services--;
            i--;
        }
    }
    
    pthread_mutex_unlock(&service_lock);
    
    return (0);
}

/*
 * Find services by name
 */
struct service_endpoint **
cluster_find_services(const char *name, const char *ns, int *count)
{
    struct service_endpoint **result;
    int alloc = 16;
    int n = 0, i;
    
    if (count == NULL)
        return (NULL);
    
    *count = 0;
    result = calloc(alloc, sizeof(struct service_endpoint *));
    if (result == NULL)
        return (NULL);
    
    pthread_mutex_lock(&service_lock);
    
    for (i = 0; i < n_services; i++) {
        if (strcmp(service_registry[i]->service_name, name) == 0 &&
            (ns == NULL || strcmp(service_registry[i]->namespace, ns) == 0)) {
            if (n >= alloc) {
                alloc *= 2;
                result = realloc(result, alloc * sizeof(struct service_endpoint *));
            }
            result[n++] = service_registry[i];
        }
    }
    
    pthread_mutex_unlock(&service_lock);
    
    *count = n;
    return (result);
}

/*
 * Get services as JSON
 */
int
cluster_services_json(char **json_out, size_t *json_len)
{
    char *json, *p;
    size_t json_size;
    int i;
    
    if (json_out == NULL)
        return (-1);
    
    pthread_mutex_lock(&service_lock);
    
    json_size = 4096 + (size_t)n_services * 1024;
    json = malloc(json_size);
    if (json == NULL) {
        pthread_mutex_unlock(&service_lock);
        return (-1);
    }

    p = json;
    size_t remaining = json_size;
    int n;

    n = snprintf(p, remaining, "{\n  \"services\": [\n");
    if (n < 0 || (size_t)n >= remaining) { free(json); pthread_mutex_unlock(&service_lock); return (-1); }
    p += n; remaining -= (size_t)n;

    for (i = 0; i < n_services; i++) {
        n = snprintf(p, remaining,
            "    {\"name\":\"%s\",\"ns\":\"%s\",\"ip\":\"%s\",\"port\":%d}%s\n",
            service_registry[i]->service_name,
            service_registry[i]->namespace,
            service_registry[i]->ip,
            service_registry[i]->port,
            i < n_services - 1 ? "," : "");
        if (n < 0 || (size_t)n >= remaining) { free(json); pthread_mutex_unlock(&service_lock); return (-1); }
        p += n; remaining -= (size_t)n;
    }

    n = snprintf(p, remaining, "  ]\n}");
    if (n < 0 || (size_t)n >= remaining) { free(json); pthread_mutex_unlock(&service_lock); return (-1); }

    pthread_mutex_unlock(&service_lock);

    *json_out = json;
    if (json_len != NULL)
        *json_len = strlen(json);
    
    return (0);
}
