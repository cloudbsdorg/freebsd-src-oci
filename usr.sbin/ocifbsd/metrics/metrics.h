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
 * Metrics collection header
 */

#ifndef _OCIFBSD_METRICS_H
#define _OCIFBSD_METRICS_H

#include <sys/tree.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Metric types */
#define METRIC_TYPE_GAUGE     0
#define METRIC_TYPE_COUNTER   1
#define METRIC_TYPE_HISTOGRAM 2
#define METRIC_TYPE_SUMMARY   3

/* Metric value */
struct metric_value {
    double value;
    uint64_t timestamp;
};

/* Histogram bucket */
struct histogram_bucket {
    double le;          /* less than or equal */
    uint64_t count;
};

/* Histogram */
struct metric_histogram {
    double *bounds;
    int nbuckets;
    uint64_t count;
    double sum;
    double sum_squared;
};

/* Summary */
struct metric_summary {
    double quantile;
    double value;
};

/* Single metric */
struct metric {
    char name[256];
    char help[512];
    int type;               /* METRIC_TYPE_* */

    /* Current value for gauge/counter */
    double value;
    uint64_t last_update;

    /* Labels */
    char **label_names;
    char **label_values;
    int nlabels;

    /* Histogram data */
    struct metric_histogram *histogram;

    /* Summary data */
    struct metric_summary *quantiles;
    int nquantiles;

    /* Tree entry */
    RB_ENTRY(metric) entry;
};

/* Metrics registry */
RB_HEAD(metric_tree, metric);
RB_PROTOTYPE(metric_tree, metric, entry, metric_compare);

/* Node metrics */
struct node_metrics {
    /* CPU */
    double cpu_user;
    double cpu_system;
    double cpu_idle;
    double cpu_interrupt;
    uint64_t cpu_threads;

    /* Memory */
    uint64_t mem_total;
    uint64_t mem_free;
    uint64_t mem_available;
    uint64_t mem_cached;
    uint64_t mem_buffers;
    uint64_t mem_swap_total;
    uint64_t mem_swap_used;

    /* Disk */
    uint64_t disk_total;
    uint64_t disk_free;
    uint64_t disk_inodes_total;
    uint64_t disk_inodes_free;

    /* Network */
    uint64_t net_rx_bytes;
    uint64_t net_tx_bytes;
    uint64_t net_rx_packets;
    uint64_t net_tx_packets;
    uint64_t net_rx_errors;
    uint64_t net_tx_errors;

    /* Load */
    double loadavg_1m;
    double loadavg_5m;
    double loadavg_15m;

    /* Time */
    uint64_t timestamp;
};

/* Pod metrics */
struct pod_metrics {
    char pod_name[256];
    char namespace[256];
    char node[256];

    /* CPU */
    double cpu_usage;
    double cpu_limit;
    double cpu_request;

    /* Memory */
    uint64_t mem_usage;
    uint64_t mem_limit;
    uint64_t mem_request;

    /* Network */
    uint64_t net_rx_bytes;
    uint64_t net_tx_bytes;

    /* Disk */
    uint64_t disk_usage;

    /* Processes */
    uint64_t process_count;
    uint64_t thread_count;
    uint64_t file_descriptors;

    /* Time */
    uint64_t timestamp;
};

/* Service metrics */
struct service_metrics {
    char service_name[256];
    char namespace[256];

    /* Replicas */
    int replicas_desired;
    int replicas_available;
    int replicas_unavailable;

    /* Requests */
    uint64_t requests_total;
    uint64_t requests_success;
    uint64_t requests_error;
    uint64_t requests_timeout;
    double request_duration_ms_avg;
    double request_duration_ms_p99;

    /* Time */
    uint64_t timestamp;
};

/* Metrics collector configuration */
struct metrics_config {
    int collection_interval;    /* seconds */
    int retention_period;       /* seconds */
    int export_interval;        /* seconds */
    char *export_endpoint;      /* Prometheus endpoint */
    bool enable_host_metrics;
    bool enable_pod_metrics;
    bool enable_service_metrics;
    bool enable_network_metrics;
    int histogram_buckets;      /* number of histogram buckets */
};

/* Metrics functions */
void metrics_init(struct metrics_config *config);
void metrics_shutdown(void);

/* Metric registration */
struct metric *metrics_register(const char *name, const char *help, int type);
int metrics_unregister(const char *name);

/* Metric operations */
int metrics_set_gauge(const char *name, double value);
int metrics_inc_counter(const char *name, double delta);
int metrics_observe_histogram(const char *name, double value);

/* Node metrics */
int node_metrics_collect(struct node_metrics *metrics);
int node_metrics_export_json(FILE *fp);
int node_metrics_export_prometheus(FILE *fp);

/* Pod metrics */
int pod_metrics_collect(const char *pod_name, struct pod_metrics *metrics);
int pod_metrics_list(struct pod_metrics **metrics, int *count);
int pod_metrics_export_json(FILE *fp);
int pod_metrics_export_prometheus(FILE *fp);

/* Service metrics */
int service_metrics_collect(const char *service_name, struct service_metrics *metrics);
int service_metrics_export_json(FILE *fp);
int service_metrics_export_prometheus(FILE *fp);

/* Query interface */
struct metric *metrics_query(const char *name);
struct metric **metrics_list(int *count);
char *metrics_serialize_json(void);
char *metrics_serialize_prometheus(void);

/* Alerting */
struct metrics_threshold {
    char metric_name[256];
    double warning;
    double critical;
    int comparison;     /* 0 = above, 1 = below */
};

int metrics_set_threshold(const char *name, double warning, double critical, int comparison);
int metrics_check_thresholds(void);
char *metrics_get_alerts_json(void);

#endif /* _OCIFBSD_METRICS_H */
