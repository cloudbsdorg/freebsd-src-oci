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
 * Log aggregation daemon header
 */

#ifndef _OCIFBSD_LOGD_H
#define _OCIFBSD_LOGD_H

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

/* Log severity levels */
#define LOG_EMERG     0   /* System is unusable */
#define LOG_ALERT     1   /* Action must be taken immediately */
#define LOG_CRIT      2   /* Critical conditions */
#define LOG_ERR       3   /* Error conditions */
#define LOG_WARNING   4   /* Warning conditions */
#define LOG_NOTICE    5   /* Normal but significant condition */
#define LOG_INFO      6   /* Informational */
#define LOG_DEBUG     7   /* Debug-level messages */
#define LOG_TRACE     8   /* Trace-level (very verbose) */

#define NUM_LOG_LEVELS 9

/* Log source types */
#define LOG_SOURCE_JAIL      0
#define LOG_SOURCE_HOST      1
#define LOG_SOURCE_CONTAINER 2
#define LOG_SOURCE_SERVICE   3
#define LOG_SOURCE_API       4

/* Log format */
#define LOG_FORMAT_JSON      0
#define LOG_FORMAT_TEXT      1
#define LOG_FORMAT_SYSLOG    2

/* Log retention tiers */
#define RETENTION_HOT   0   /* Last 24 hours */
#define RETENTION_WARM   1   /* Last 7 days */
#define RETENTION_COLD   2   /* Last 30 days */
#define RETENTION_ARCHIVE 3  /* Archived */

/* Log entry structure */
struct log_entry {
    uint64_t        id;                 /* Unique log ID */
    time_t          timestamp;          /* Log timestamp */
    int             severity;            /* LOG_* level */
    int             source;             /* LOG_SOURCE_* */
    char            source_name[256];    /* Container/jail name */
    char            message[4096];       /* Log message */
    char            fields[2048];        /* Structured fields (JSON) */
    char            hostname[256];       /* Origin hostname */
    char            namespace[128];      /* orchestration namespace */
    char            pod_name[256];       /* Pod name */
    char            container_name[128]; /* Container name */
    uint32_t        pid;                /* Process ID */
    uint32_t        tid;                /* Thread ID */
    char            username[64];        /* User if applicable */
    uint64_t        size;               /* Entry size in bytes */

    /* Links for queue/ring buffer */
    TAILQ_ENTRY(log_entry) next;
    LIST_ENTRY(log_entry) hash_next;
};

/* Ring buffer for in-memory log storage */
struct log_ringbuf {
    struct log_entry *entries;      /* Circular buffer */
    uint64_t         *ids;           /* Slot status (0=free, ID=used) */
    uint64_t         size;           /* Buffer size (max entries) */
    uint64_t         head;           /* Next write position */
    uint64_t         tail;           /* Oldest entry position */
    uint64_t         count;          /* Current entry count */
    uint64_t         total_written;  /* Total entries written (monotonic) */
    bool             entries_mmapped;/* true: entries via mmap; false: calloc */
    size_t           entries_bytes;  /* byte size of the entries mapping */
    pthread_mutex_t  lock;
};

/* Forwarder destination */
struct log_forwarder {
    char            name[128];       /* Forwarder name */
    int             type;            /* syslog/fluentd/elk/splunk/custom */
    char            endpoint[512];   /* Destination URL/address */
    int             protocol;        /* TCP/UDP/TCP+TLS */
    int             format;          /* LOG_FORMAT_* */
    bool            enabled;
    int             retry_count;
    time_t          last_retry;
    pthread_mutex_t lock;
    LIST_ENTRY(log_forwarder) next;
};
LIST_HEAD(forwarder_list, log_forwarder);

/* Alert rule */
struct alert_rule {
    char            name[128];       /* Rule name */
    char            description[512];
    int             severity;        /* Minimum severity to match */
    int             source;          /* Source filter (-1 = any) */
    char            *match_pattern;  /* Regex to match message */
    char            *namespace;      /* Namespace filter */
    char            *source_name;     /* Source name filter */
    int             count_threshold; /* Trigger after N matches */
    int             count_window;    /* Time window in seconds */
    time_t          last_triggered;
    int             current_count;
    bool            enabled;
    bool            silenced;        /* Temporarily disabled */
    time_t          silenced_until;  /* auto-unsilence at this time (0 = manual) */

    /* Actions */
    bool            notify_webhook;
    char            webhook_url[512];
    bool            notify_email;
    char            email_to[256];
    bool            execute_command;
    char            command[1024];

    pthread_mutex_t lock;
    RB_ENTRY(alert_rule) entry;
};
RB_HEAD(alert_rule_tree, alert_rule);
RB_PROTOTYPE(alert_rule_tree, alert_rule, entry, alert_compare);

/* Log query parameters */
struct log_query {
    time_t          start_time;
    time_t          end_time;
    int             severity_min;
    int             severity_max;
    int             *sources;        /* NULL = all sources */
    int             num_sources;
    char            *source_name;    /* Glob pattern */
    char            *message_pattern;/* Regex pattern */
    char            *namespace;
    char            *hostname;
    char            *username;
    uint64_t        limit;           /* Max results */
    uint64_t        offset;
    bool            descending;       /* Newest first */
    char            **fields;        /* Fields to return */
    int             num_fields;
};

/* Log statistics */
struct log_stats {
    uint64_t        entries_total;
    uint64_t        entries_written;
    uint64_t        entries_read;
    uint64_t        entries_dropped;
    uint64_t        entries_forwarded;
    uint64_t        alerts_triggered;
    uint64_t        bytes_written;
    uint64_t        bytes_read;
    time_t          oldest_entry;
    time_t          newest_entry;
};

/* Log daemon configuration */
struct logd_config {
    int             log_level;          /* Minimum log level to capture */
    uint64_t        ringbuf_size;        /* Max entries in memory */
    int             retention_hot;       /* Hot retention in days */
    int             retention_warm;      /* Warm retention in days */
    int             retention_cold;      /* Cold retention in days */
    int             retention_archive;   /* Archive retention in days */
    char            *storage_path;      /* ZFS dataset path */
    int             forwarder_count;
    struct log_forwarder *forwarders;
    int             alert_rule_count;
    struct alert_rule *alert_rules;
    bool            enable_local_syslog;
    bool            enable_journald;
    bool            enable_prometheus;
    int             prometheus_port;
    int             query_timeout;       /* Query timeout in seconds */
    int             forward_batch_size;  /* Batch size for forwarding */
    int             forward_interval;    /* Forward interval in ms */
};

/* Event types for event stream */
#define EVENT_POD_CREATED       1
#define EVENT_POD_STARTED       2
#define EVENT_POD_STOPPED       3
#define EVENT_POD_DELETED       4
#define EVENT_POD_RESTARTED     5
#define EVENT_CONTAINER_CREATED 6
#define EVENT_CONTAINER_STARTED 7
#define EVENT_CONTAINER_STOPPED 8
#define EVENT_NODE_JOINED       9
#define EVENT_NODE_LEFT         10
#define EVENT_CLUSTER_CHANGED   11
#define EVENT_SECRET_UPDATED    12
#define EVENT_CONFIG_CHANGED    13

/* Event entry (lightweight) */
struct event_entry {
    uint64_t        id;
    time_t          timestamp;
    int             type;           /* EVENT_* */
    char            source[256];   /* Who generated it */
    char            subject[512];  /* What it happened to */
    char            *data;         /* JSON additional data */
    char            *user;         /* User who triggered (if applicable) */
    RB_ENTRY(event_entry) entry;
};
RB_HEAD(event_tree, event_entry);
RB_PROTOTYPE(event_tree, event_entry, entry, event_compare);

/* Function declarations */

/* Log daemon core */
int     logd_init(struct logd_config *config);
void    logd_shutdown(void);
int     logd_reload_config(const char *config_path);
struct logd_config *logd_get_config(void);

/* Ring buffer operations */
struct log_ringbuf *ringbuf_create(uint64_t size);
void    ringbuf_destroy(struct log_ringbuf *rb);
int     ringbuf_write(struct log_ringbuf *rb, struct log_entry *entry);
struct log_entry *ringbuf_read(struct log_ringbuf *rb, uint64_t id);
struct log_entry *ringbuf_iterate(struct log_ringbuf *rb, uint64_t *cursor);
int     ringbuf_query(struct log_ringbuf *rb, struct log_query *query,
            struct log_entry ***results, uint64_t *count);
uint64_t ringbuf_oldest_id(struct log_ringbuf *rb);
uint64_t ringbuf_newest_id(struct log_ringbuf *rb);

/* Log ingestion */
int     log_write(int severity, int source, const char *source_name,
            const char *fmt, ...) __printflike(4, 5);
int     log_write_structured(int severity, int source, const char *source_name,
            const char *fields_json, const char *fmt, ...) __printflike(5, 6);
int     log_write_from_jail(const char *jail_name, int severity,
            const char *message);
int     log_capture_fd(int fd, const char *source_name);

/* Log forwarding */
int     logd_http_post(const char *url, const char *body,
	    const char *content_type, const char *extra_header);
int     forwarder_add(struct log_forwarder *fw);
int     forwarder_remove(const char *name);
int     forwarder_enable(const char *name);
int     forwarder_disable(const char *name);
struct log_forwarder **forwarder_list(int *count);
int     forwarder_send(struct log_entry *entry);
int     forwarder_flush(void);

/* Alerting */
bool    alert_rule_active(struct alert_rule *rule, time_t now);
int     alert_rule_add(struct alert_rule *rule);
int     alert_rule_remove(const char *name);
int     alert_rule_update(struct alert_rule *rule);
struct alert_rule **alert_rule_list(int *count);
int     alert_trigger(struct alert_rule *rule, struct log_entry *entry);
int     alert_silence(const char *name, time_t until);
int     alert_unsilence(const char *name);
bool    alert_is_silenced(const char *name);
int     alert_process_entry(struct log_entry *entry);

/* Event stream */
int     event_publish(int type, const char *source, const char *subject,
            const char *data);
struct event_entry **event_query(time_t start, time_t end,
            int *types, int num_types, uint64_t limit, uint64_t *count);
int     event_subscribe(int *types, int num_types,
            void (*callback)(struct event_entry *));
int     event_webhook_register(const char *name, const char *url,
            int *types, int num_types);
int     event_webhook_unregister(const char *name);

/* Log query */
struct log_entry **log_query_exec(struct log_query *query, uint64_t *count);
int     log_query_export(struct log_query *query, const char *format,
            FILE *fp);
char    *log_format_entry(struct log_entry *entry, int format);

/* Log statistics */
int     log_stats_get(struct log_stats *stats);
int     log_stats_json(char **json_out);
int     log_stats_prometheus(FILE *fp);

/* Log rotation and archival */
int     log_rotate(void);
int     log_archive_oldest(void);
int     log_compact(struct log_query *keep_query);
int     log_retention_apply(void);

/* Webhook delivery */
struct webhook_delivery {
    char            url[512];
    char            *payload;
    int             attempts;
    time_t          next_retry;
    STAILQ_ENTRY(webhook_delivery) next;
};
STAILQ_HEAD(webhook_queue, webhook_delivery);

int     webhook_enqueue(const char *url, const char *payload);
int     webhook_process(void);

#endif /* _OCIFBSD_LOGD_H */
