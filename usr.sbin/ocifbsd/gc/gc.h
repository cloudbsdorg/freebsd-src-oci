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
 * Garbage collection header
 */

#ifndef _OCIFBSD_GC_H
#define _OCIFBSD_GC_H

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* GC types */
#define GC_TYPE_CONTAINER    0
#define GC_TYPE_IMAGE       1
#define GC_TYPE_VOLUME      2
#define GC_TYPE_NETWORK     3
#define GC_TYPE_NODE        4
#define GC_TYPE_ALL         5

/* GC reasons */
#define GC_REASON_STOPPED       0   /* Container stopped */
#define GC_REASON_FINISHED      1   /* Job/pod finished */
#define GC_REASON_EXPIRED       2   /* TTL expired */
#define GC_REASON_ORPHANED      3   /* No owner reference */
#define GC_REASON_NODE_LEFT     4   /* Node left cluster */
#define GC_REASON_USER_REQUEST  5   /* Manual delete */
#define GC_REASON_SPACE_LOW     6   /* Disk space low */
#define GC_REASON_DUPLICATE     7   /* Duplicate/unused resource */

/* GC priorities */
#define GC_PRIORITY_LOW     0
#define GC_PRIORITY_NORMAL  1
#define GC_PRIORITY_HIGH    2
#define GC_PRIORITY_CRITICAL 3

/* GC item status */
#define GC_STATUS_PENDING   0
#define GC_STATUS_RUNNING   1
#define GC_STATUS_COMPLETE  2
#define GC_STATUS_FAILED    3
#define GC_STATUS_SKIPPED   4

/* Orphaned resource */
struct gc_orphan {
    int             type;           /* GC_TYPE_* */
    char            name[256];
    char            namespace[128];
    char            node[256];
    time_t          detected;
    time_t          last_heartbeat;/* For container orphans */
    int             reason;        /* GC_REASON_* */
    int             priority;
    bool            is_protected;   /* Don't delete */
    RB_ENTRY(gc_orphan) entry;
};
RB_HEAD(orphan_tree, gc_orphan);
RB_PROTOTYPE(orphan_tree, gc_orphan, entry, orphan_compare);

/* GC item */
struct gc_item {
    uint64_t        id;
    int             type;
    char            name[256];
    char            namespace[128];
    char            owner_ref[512];  /* Owner reference */
    time_t          created;
    time_t          last_used;
    time_t          scheduled;
    int             status;
    int             priority;
    int             reason;
    int             attempts;
    char            error[512];
    char            *metadata;
    TAILQ_ENTRY(gc_item) next;
};
TAILQ_HEAD(gc_queue, gc_item);

/* GC configuration */
struct gc_config {
    /* Container GC */
    int             container_ttl;           /* seconds */
    int             stopped_container_ttl;  /* seconds */
    int             max_containers;
    bool            gc_stopped_containers;
    bool            gc_finished_pods;

    /* Image GC */
    int             image_ttl;              /* seconds */
    uint64_t        image_min_free_space;   /* bytes */
    uint64_t        image_high_threshold;   /* percent */
    uint64_t        image_low_threshold;    /* percent */

    /* Volume GC */
    int             volume_ttl;
    bool            gc_orphaned_volumes;
    bool            gc_released_pvc;

    /* Network GC */
    bool            gc_orphaned_networks;
    bool            gc_unused_networks;

    /* Cluster GC */
    int             node_grace_period;      /* seconds */
    bool            gc_orphaned_pods;
    bool            gc_failed_jobs;

    /* General */
    int             gc_interval;            /* seconds */
    int             gc_batch_size;
    int             gc_concurrency;
    bool            dry_run;
    bool            verbose;
};

/* GC statistics */
struct gc_stats {
    uint64_t        items_scanned;
    uint64_t        items_collected;
    uint64_t        items_failed;
    uint64_t        space_reclaimed;
    uint64_t        orphans_detected;
    uint64_t        orphans_resolved;
    time_t          last_run;
    time_t          last_full_gc;
};

/* Ownership graph entry */
struct owner_ref {
    char            resource_type[64];
    char            resource_name[256];
    char            namespace[128];
    char            uid[64];
    int             block_deletion;          /* Finalizer count */
    bool            is_deleted;
    LIST_ENTRY(owner_ref) refs;
};
LIST_HEAD(owner_list, owner_ref);

/* GC daemon functions */
int     gc_init(struct gc_config *config);
void    gc_shutdown(void);
struct gc_config *gc_get_config(void);
int     gc_set_config(struct gc_config *config);

int     gc_run_now(int type);
int     gc_run_full(void);
int     gc_schedule(int type, time_t when);

/* Orphan detection */
int     gc_detect_orphans(void);
struct gc_orphan **gc_list_orphans(int *count);
int     gc_resolve_orphan(const char *name, int type);
int     gc_protect_orphan(const char *name, int type);
int     gc_unprotect_orphan(const char *name, int type);

/* Container GC */
int     gc_container_orphans(void);
int     gc_stopped_containers(int ttl_seconds);
int     gc_finished_pods(int ttl_seconds);

/* Image GC */
int     gc_unused_images(int ttl_seconds);
int     gc_image_space(int high_percent, int low_percent);
int     gc_image_by_name(const char *pattern);
int     gc_image_pinned(const char *name);

/* Volume GC */
int     gc_orphaned_volumes(void);
int     gc_released_pvc(void);
int     gc_unused_volumes(int ttl_seconds);

/* Network GC */
int     gc_orphaned_networks(void);
int     gc_unused_networks(int ttl_seconds);
int     gc_orphan_bridge(void);
int     gc_orphan_epair(void);
int     gc_orphan_vxlan(void);

/* Cluster GC */
int     gc_node_resources(const char *node);
int     gc_dead_node_resources(void);
int     gc_stale_registrations(void);

/* ZFS cleanup */
int     gc_zfs_cleanup(const char *dataset);
int     gc_zfs_snapshots(int ttl_seconds);
int     gc_zfs_destroy_dataset(const char *dataset);

/* Firewall cleanup */
int     gc_pf_anchors(const char *anchor);
int     gc_pf_rules(const char *table);

/* Statistics */
int     gc_stats_get(struct gc_stats *stats);
int     gc_stats_json(char **json_out);
int     gc_stats_reset(void);

/* Logging */
int     gc_log_start(int type);
int     gc_log_finish(int type, int collected, int failed);
int     gc_log_item(int type, const char *name, const char *action);

#endif /* _OCIFBSD_GC_H */
