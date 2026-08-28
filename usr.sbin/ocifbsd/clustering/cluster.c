/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by Klara, Inc. under sponsorship
 * from the FreeBSD Foundation.
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sha256.h>

#include "cluster.h"
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
    struct gossip_message msg;
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
            
        default:
            break;
    }
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
int
raft_init(void)
{
    memset(&raft, 0, sizeof(raft));
    raft.state = RAFT_FOLLOWER;
    raft.current_term = 0;
    raft.commit_index = 0;
    raft.last_applied = 0;
    
    return (0);
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
    entry->index = raft.log_size + 1;
    strlcpy(entry->command, command, sizeof(entry->command));
    entry->timestamp = time(NULL);
    
    raft.log_size++;
    
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
    size_t json_size, written;
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
