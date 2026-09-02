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
 * Metrics collection implementation
 */

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/resource.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/sockopt.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <netinet/in.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <devstat.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <kvm.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <pthread.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "metrics.h"
#include "../include/ocifbsd.h"

/* Global metrics registry */
static struct metric_tree metrics_registry;
static int metrics_initialized = 0;
static pthread_mutex_t metrics_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t metrics_thread;
static int metrics_shutdown_requested = 0;
static struct metrics_config metrics_conf;

/* Thresholds */
static struct metrics_threshold *thresholds = NULL;
static int n_thresholds = 0;
static pthread_mutex_t threshold_lock = PTHREAD_MUTEX_INITIALIZER;

/* Alert list */
static char **active_alerts = NULL;
static int n_alerts = 0;
static pthread_mutex_t alert_lock = PTHREAD_MUTEX_INITIALIZER;

/* Default histogram buckets */
static double default_histogram_bounds[] = {
	0.005, 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 1.0, 2.5, 5.0, 10.0
};

/*
 * Compare metrics by name
 */
int
metric_compare(struct metric *a, struct metric *b)
{
	return (strcmp(a->name, b->name));
}

/*
 * Initialize metrics system
 */
void
metrics_init(struct metrics_config *config)
{
	if (__sync_fetch_and_add(&metrics_initialized, 0) == 0) {
		pthread_mutex_lock(&metrics_lock);
		if (metrics_initialized == 0) {
			RB_INIT(&metrics_registry);

			if (config != NULL) {
				memcpy(&metrics_conf, config, sizeof(struct metrics_config));
			} else {
				/* Default configuration */
				memset(&metrics_conf, 0, sizeof(struct metrics_config));
				metrics_conf.collection_interval = 15;
				metrics_conf.retention_period = 3600;
				metrics_conf.export_interval = 60;
				metrics_conf.enable_host_metrics = true;
				metrics_conf.enable_pod_metrics = true;
				metrics_conf.enable_service_metrics = true;
				metrics_conf.histogram_buckets = 11;
			}

			__sync_fetch_and_add(&metrics_initialized, 1);
		}
		pthread_mutex_unlock(&metrics_lock);

		/* Start collection thread */
		pthread_create(&metrics_thread, NULL, metrics_collector_thread, NULL);
	}
}

/*
 * Shutdown metrics system
 */
void
metrics_shutdown(void)
{
	__sync_fetch_and_or(&metrics_shutdown_requested, 1);
	pthread_join(metrics_thread, NULL);
	__sync_fetch_and_add(&metrics_initialized, 0);
}

/*
 * Metrics collection thread
 */
static void *
metrics_collector_thread(void *arg)
{
	struct node_metrics node_metrics;

	(void)arg;

	while (!__sync_fetch_and_add(&metrics_shutdown_requested, 0)) {
		/* Collect node metrics */
		if (metrics_conf.enable_host_metrics) {
			node_metrics_collect(&node_metrics);
		}

		/* Sleep until next collection */
		sleep(metrics_conf.collection_interval);
	}

	return (NULL);
}

/*
 * Register a metric
 */
struct metric *
metrics_register(const char *name, const char *help, int type)
{
	struct metric *m, *existing;

	if (name == NULL)
		return (NULL);

	pthread_mutex_lock(&metrics_lock);

	m = calloc(1, sizeof(struct metric));
	if (m == NULL) {
		pthread_mutex_unlock(&metrics_lock);
		return (NULL);
	}

	strlcpy(m->name, name, sizeof(m->name));
	if (help != NULL)
		strlcpy(m->help, help, sizeof(m->help));
	m->type = type;
	m->timestamp = time(NULL);

	/* Initialize histogram */
	if (type == METRIC_TYPE_HISTOGRAM) {
		m->histogram = calloc(1, sizeof(struct metric_histogram));
		if (m->histogram != NULL) {
			m->histogram->nbuckets = metrics_conf.histogram_buckets;
			m->histogram->bounds = default_histogram_bounds;
			m->histogram->count = 0;
			m->histogram->sum = 0;
		}
	}

	existing = RB_FIND(metric_tree, &metrics_registry, m);
	if (existing != NULL) {
		free(m);
		pthread_mutex_unlock(&metrics_lock);
		return (existing);
	}

	RB_INSERT(metric_tree, &metrics_registry, m);
	pthread_mutex_unlock(&metrics_lock);

	return (m);
}

/*
 * Unregister a metric
 */
int
metrics_unregister(const char *name)
{
	struct metric m_find, *m;

	if (name == NULL)
		return (-1);

	strlcpy(m_find.name, name, sizeof(m_find.name));

	pthread_mutex_lock(&metrics_lock);
	m = RB_FIND(metric_tree, &metrics_registry, &m_find);
	if (m != NULL) {
		RB_REMOVE(metric_tree, &metrics_registry, m);
		if (m->histogram != NULL)
			free(m->histogram);
		free(m);
	}
	pthread_mutex_unlock(&metrics_lock);

	return (m != NULL ? 0 : -1);
}

/*
 * Set gauge value
 */
int
metrics_set_gauge(const char *name, double value)
{
	struct metric *m;

	m = metrics_query(name);
	if (m == NULL) {
		m = metrics_register(name, NULL, METRIC_TYPE_GAUGE);
		if (m == NULL)
			return (-1);
	}

	pthread_mutex_lock(&metrics_lock);
	m->value = value;
	m->timestamp = time(NULL);
	pthread_mutex_unlock(&metrics_lock);

	return (0);
}

/*
 * Increment counter
 */
int
metrics_inc_counter(const char *name, double delta)
{
	struct metric *m;

	m = metrics_query(name);
	if (m == NULL) {
		m = metrics_register(name, NULL, METRIC_TYPE_COUNTER);
		if (m == NULL)
			return (-1);
	}

	pthread_mutex_lock(&metrics_lock);
	m->value += delta;
	m->timestamp = time(NULL);
	pthread_mutex_unlock(&metrics_lock);

	return (0);
}

/*
 * Observe histogram value
 */
int
metrics_observe_histogram(const char *name, double value)
{
	struct metric *m;
	int i;

	m = metrics_query(name);
	if (m == NULL) {
		m = metrics_register(name, NULL, METRIC_TYPE_HISTOGRAM);
		if (m == NULL)
			return (-1);
	}

	pthread_mutex_lock(&metrics_lock);

	if (m->histogram != NULL) {
		m->histogram->count++;
		m->histogram->sum += value;
		m->histogram->sum_squared += value * value;

		/* Update bucket counts */
		for (i = 0; i < m->histogram->nbuckets; i++) {
			if (value <= m->histogram->bounds[i]) {
				/* Bucket would be incremented */
				break;
			}
		}
	}

	m->timestamp = time(NULL);
	pthread_mutex_unlock(&metrics_lock);

	return (0);
}

/*
 * Query a metric
 */
struct metric *
metrics_query(const char *name)
{
	struct metric m_find;

	if (name == NULL)
		return (NULL);

	strlcpy(m_find.name, name, sizeof(m_find.name));

	pthread_mutex_lock(&metrics_lock);
	struct metric *m = RB_FIND(metric_tree, &metrics_registry, &m_find);
	pthread_mutex_unlock(&metrics_lock);

	return (m);
}

/*
 * List all metrics
 */
struct metric **
metrics_list(int *count)
{
	struct metric **result;
	struct metric *m;
	int alloc = 16;
	int n = 0;

	*count = 0;
	result = calloc(alloc, sizeof(struct metric *));
	if (result == NULL)
		return (NULL);

	pthread_mutex_lock(&metrics_lock);
	RB_FOREACH(m, metric_tree, &metrics_registry) {
		if (n >= alloc) {
			alloc *= 2;
			void *_new = realloc(result, alloc * sizeof(struct metric *));
			if (_new == NULL) {
				pthread_mutex_unlock(&metrics_lock);
				*count = 0;
				return (NULL);
			}
			result = _new;
		}
		result[n++] = m;
	}
	pthread_mutex_unlock(&metrics_lock);

	*count = n;
	return (result);
}

/*
 * Collect node metrics
 */
int
node_metrics_collect(struct node_metrics *metrics)
{
	int mib[4];
	size_t len;
	struct utsname uts;
	struct ifaddrs *ifaddrs, *ifa;
	uint64_t rx_bytes = 0, tx_bytes = 0;

	if (metrics == NULL)
		return (-1);

	memset(metrics, 0, sizeof(struct node_metrics));

	/* Get hostname */
	uname(&uts);
	strlcpy(metrics->hostname, uts.nodename, sizeof(metrics->hostname));

	/*
	 * CPU metrics. kern.cp_time is a long[CPUSTATES] of cumulative ticks per
	 * state; report each state as a percentage of the total since boot.
	 * (The old code sized the buffer as one double — too small, so the read
	 * failed — and never computed anything, leaving cpu_* at 0.)
	 */
	{
		long cp_time[CPUSTATES];

		len = sizeof(cp_time);
		if (sysctlbyname("kern.cp_time", cp_time, &len, NULL, 0) == 0) {
			long total = 0;
			int i;

			for (i = 0; i < CPUSTATES; i++)
				total += cp_time[i];
			if (total > 0) {
				metrics->cpu_user = 100.0 *
					(cp_time[CP_USER] + cp_time[CP_NICE]) / total;
				metrics->cpu_system = 100.0 * cp_time[CP_SYS] / total;
				metrics->cpu_interrupt = 100.0 * cp_time[CP_INTR] / total;
				metrics->cpu_idle = 100.0 * cp_time[CP_IDLE] / total;
			}
		}
	}

	/* Get per-CPU idle time */
	int ncpu;
	len = sizeof(ncpu);
	if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0) {
		metrics->cpu_threads = ncpu;
	}

	/* Load average */
	/*
	 * getloadavg(3) fills a double[] with the load averages directly; the old
	 * code passed a `struct loadavg *` (wrong type) and then divided by an
	 * uninitialized fscale, yielding garbage. Use the documented interface.
	 */
	{
		double lavg[3];

		if (getloadavg(lavg, 3) == 3) {
			metrics->loadavg_1m = lavg[0];
			metrics->loadavg_5m = lavg[1];
			metrics->loadavg_15m = lavg[2];
		}
	}

	/* Memory metrics */
	len = sizeof(uint64_t);
	sysctlbyname("hw.physmem", &metrics->mem_total, &len, NULL, 0);

	int page_size = getpagesize();
	len = sizeof(uint64_t);
	sysctlbyname("vm.stats.vm.v_free_count", &metrics->mem_free, &len, NULL, 0);
	metrics->mem_free *= page_size;

	len = sizeof(uint64_t);
	sysctlbyname("vm.stats.vm.v_active_count", &metrics->mem_available, &len, NULL, 0);
	metrics->mem_available *= page_size;

	len = sizeof(uint64_t);
	sysctlbyname("vm.stats.vm.v_cache_count", &metrics->mem_cached, &len, NULL, 0);
	metrics->mem_cached *= page_size;

	/* Network metrics - aggregate all interfaces */
	if (getifaddrs(&ifaddrs) == 0) {
		for (ifa = ifaddrs; ifa != NULL; ifa = ifa->ifa_next) {
			if (ifa->ifa_addr == NULL)
				continue;
			if (ifa->ifa_addr->sa_family != AF_LINK)
				continue;

			struct if_data *ifdata = (struct if_data *)ifa->ifa_data;
			if (ifdata != NULL) {
				rx_bytes += ifdata->ifi_ibytes;
				tx_bytes += ifdata->ifi_obytes;
				metrics->net_rx_packets += ifdata->ifi_ipackets;
				metrics->net_tx_packets += ifdata->ifi_opackets;
				metrics->net_rx_errors += ifdata->ifi_ierrors;
				metrics->net_tx_errors += ifdata->ifi_oerrors;
			}
		}
		freeifaddrs(ifaddrs);
	}

	metrics->net_rx_bytes = rx_bytes;
	metrics->net_tx_bytes = tx_bytes;

	/* Disk metrics */
	struct statvfs fs;
	if (statvfs("/", &fs) == 0) {
		metrics->disk_total = fs.f_blocks * fs.f_frsize;
		metrics->disk_free = fs.f_bfree * fs.f_frsize;
		metrics->disk_inodes_total = fs.f_files;
		metrics->disk_inodes_free = fs.f_ffree;
	}

	/* Swap */
	len = sizeof(uint64_t);
	sysctlbyname("vm.swap_total", &metrics->mem_swap_total, &len, NULL, 0);

	/* Timestamp */
	metrics->timestamp = time(NULL);

	/* Update gauge metrics */
	metrics_set_gauge("node_cpu_usage_percent", metrics->cpu_user);
	metrics_set_gauge("node_memory_total_bytes", metrics->mem_total);
	metrics_set_gauge("node_memory_free_bytes", metrics->mem_free);
	metrics_set_gauge("node_memory_available_bytes", metrics->mem_available);
	metrics_set_gauge("node_loadavg_1m", metrics->loadavg_1m);
	metrics_set_gauge("node_loadavg_5m", metrics->loadavg_5m);
	metrics_set_gauge("node_loadavg_15m", metrics->loadavg_15m);
	metrics_set_gauge("node_network_rx_bytes_total", metrics->net_rx_bytes);
	metrics_set_gauge("node_network_tx_bytes_total", metrics->net_tx_bytes);
	metrics_set_gauge("node_filesystem_total_bytes", metrics->disk_total);
	metrics_set_gauge("node_filesystem_free_bytes", metrics->disk_free);

	return (0);
}

/*
 * Export node metrics as JSON
 */
int
node_metrics_export_json(FILE *fp)
{
	struct node_metrics metrics;

	if (fp == NULL)
		return (-1);

	if (node_metrics_collect(&metrics) != 0)
		return (-1);

	fprintf(fp, "{\n");
	fprintf(fp, "  \"hostname\": \"%s\",\n", metrics.hostname);
	fprintf(fp, "  \"timestamp\": %lu,\n", metrics.timestamp);
	fprintf(fp, "  \"cpu\": {\n");
	fprintf(fp, "    \"threads\": %lu,\n", metrics.cpu_threads);
	fprintf(fp, "    \"user\": %.2f,\n", metrics.cpu_user);
	fprintf(fp, "    \"system\": %.2f,\n", metrics.cpu_system);
	fprintf(fp, "    \"idle\": %.2f\n", metrics.cpu_idle);
	fprintf(fp, "  },\n");
	fprintf(fp, "  \"memory\": {\n");
	fprintf(fp, "    \"total\": %lu,\n", metrics.mem_total);
	fprintf(fp, "    \"free\": %lu,\n", metrics.mem_free);
	fprintf(fp, "    \"available\": %lu,\n", metrics.mem_available);
	fprintf(fp, "    \"cached\": %lu\n", metrics.mem_cached);
	fprintf(fp, "  },\n");
	fprintf(fp, "  \"network\": {\n");
	fprintf(fp, "    \"rx_bytes\": %lu,\n", metrics.net_rx_bytes);
	fprintf(fp, "    \"tx_bytes\": %lu,\n", metrics.net_tx_bytes);
	fprintf(fp, "    \"rx_packets\": %lu,\n", metrics.net_rx_packets);
	fprintf(fp, "    \"tx_packets\": %lu\n", metrics.net_tx_packets);
	fprintf(fp, "  },\n");
	fprintf(fp, "  \"loadavg\": {\n");
	fprintf(fp, "    \"1m\": %.2f,\n", metrics.loadavg_1m);
	fprintf(fp, "    \"5m\": %.2f,\n", metrics.loadavg_5m);
	fprintf(fp, "    \"15m\": %.2f\n", metrics.loadavg_15m);
	fprintf(fp, "  }\n");
	fprintf(fp, "}\n");

	return (0);
}

/*
 * Export node metrics as Prometheus text format
 */
int
node_metrics_export_prometheus(FILE *fp)
{
	struct node_metrics metrics;

	if (fp == NULL)
		return (-1);

	if (node_metrics_collect(&metrics) != 0)
		return (-1);

	fprintf(fp, "# HELP node_cpu_threads Number of CPU threads\n");
	fprintf(fp, "# TYPE node_cpu_threads gauge\n");
	fprintf(fp, "node_cpu_threads %.0f\n", (double)metrics.cpu_threads);

	fprintf(fp, "# HELP node_memory_total_bytes Total memory in bytes\n");
	fprintf(fp, "# TYPE node_memory_total_bytes gauge\n");
	fprintf(fp, "node_memory_total_bytes %.0f\n", (double)metrics.mem_total);

	fprintf(fp, "# HELP node_memory_free_bytes Free memory in bytes\n");
	fprintf(fp, "# TYPE node_memory_free_bytes gauge\n");
	fprintf(fp, "node_memory_free_bytes %.0f\n", (double)metrics.mem_free);

	fprintf(fp, "# HELP node_memory_available_bytes Available memory in bytes\n");
	fprintf(fp, "# TYPE node_memory_available_bytes gauge\n");
	fprintf(fp, "node_memory_available_bytes %.0f\n", (double)metrics.mem_available);

	fprintf(fp, "# HELP node_loadavg_1m 1-minute load average\n");
	fprintf(fp, "# TYPE node_loadavg_1m gauge\n");
	fprintf(fp, "node_loadavg_1m %.2f\n", metrics.loadavg_1m);

	fprintf(fp, "# HELP node_network_rx_bytes_total Network receive bytes total\n");
	fprintf(fp, "# TYPE node_network_rx_bytes_total counter\n");
	fprintf(fp, "node_network_rx_bytes_total %.0f\n", (double)metrics.net_rx_bytes);

	fprintf(fp, "# HELP node_network_tx_bytes_total Network transmit bytes total\n");
	fprintf(fp, "# TYPE node_network_tx_bytes_total counter\n");
	fprintf(fp, "node_network_tx_bytes_total %.0f\n", (double)metrics.net_tx_bytes);

	return (0);
}

/*
 * A pod name is interpolated into popen() shell commands below, so it must be
 * restricted to a safe label charset — an unchecked name like "x;reboot" or
 * "$(...)" is arbitrary command execution as the root collector. (readdir of
 * the pods state dir also feeds this function.)
 */
static bool
metric_pod_name_is_safe(const char *s)
{
	size_t i, len;

	if (s == NULL)
		return (false);
	len = strlen(s);
	if (len == 0 || len > 63)
		return (false);
	for (i = 0; i < len; i++) {
		char c = s[i];
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
			(c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
			return (false);
	}
	return (true);
}

/*
 * Collect pod metrics
 */
int
pod_metrics_collect(const char *pod_name, struct pod_metrics *metrics)
{
	char cmd[256];
	FILE *fp;
	char buf[256];

	if (pod_name == NULL || metrics == NULL)
		return (-1);
	if (!metric_pod_name_is_safe(pod_name))
		return (-1);

	memset(metrics, 0, sizeof(struct pod_metrics));
	strlcpy(metrics->pod_name, pod_name, sizeof(metrics->pod_name));

	/* Get jail metrics using rctl */
	snprintf(cmd, sizeof(cmd), "rctl -h jail:%s 2>/dev/null", pod_name);
	fp = popen(cmd, "r");

	if (fp != NULL) {
		while (fgets(buf, sizeof(buf), fp) != NULL) {
			if (strstr(buf, "vmemoryuse")) {
				uint64_t val;
				if (sscanf(buf, "%*s %*s %lu", &val) == 1) {
					metrics->mem_usage = val;
				}
			} else if (strstr(buf, "pcpu")) {
				double val;
				if (sscanf(buf, "%*s %*s %lf", &val) == 1) {
					metrics->cpu_usage = val;
				}
			} else if (strstr(buf, "nproc")) {
				uint64_t val;
				if (sscanf(buf, "%*s %*s %lu", &val) == 1) {
					metrics->process_count = val;
				}
			} else if (strstr(buf, "openfiles")) {
				uint64_t val;
				if (sscanf(buf, "%*s %*s %lu", &val) == 1) {
					metrics->file_descriptors = val;
				}
			}
		}
		pclose(fp);
	}

	/* Get disk usage from ZFS */
	snprintf(cmd, sizeof(cmd), "zfs get -H -p used ocifbsd/pods/%s 2>/dev/null | "
		"awk '{print $3}'", pod_name);
	fp = popen(cmd, "r");
	if (fp != NULL) {
		if (fgets(buf, sizeof(buf), fp) != NULL) {
			metrics->disk_usage = strtoull(buf, NULL, 10);
		}
		pclose(fp);
	}

	/* Update gauge metrics */
	char metric_name[512];
	snprintf(metric_name, sizeof(metric_name), "pod_memory_usage_bytes{pod=\"%s\"}",
		pod_name);
	metrics_set_gauge(metric_name, metrics->mem_usage);

	snprintf(metric_name, sizeof(metric_name), "pod_cpu_usage_ratio{pod=\"%s\"}",
		pod_name);
	metrics_set_gauge(metric_name, metrics->cpu_usage / 100.0);

	metrics->timestamp = time(NULL);

	return (0);
}

/*
 * List all pod metrics
 */
int
pod_metrics_list(struct pod_metrics **metrics, int *count)
{
	char **pods;
	int pod_count;
	struct pod_metrics *result;
	int alloc = 16;
	int n = 0;
	DIR *dir;
	struct dirent *ent;

	if (metrics == NULL || count == NULL)
		return (-1);

	*metrics = NULL;
	*count = 0;

	result = calloc(alloc, sizeof(struct pod_metrics));
	if (result == NULL)
		return (-1);

	/* List pods from state directory */
	char pod_dir[PATH_MAX];
	snprintf(pod_dir, sizeof(pod_dir), "%s/pods", OCIFBSD_VAR_DIR);

	dir = opendir(pod_dir);
	if (dir == NULL) {
		free(result);
		return (-1);
	}

	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_type != DT_DIR)
			continue;
		if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
			continue;

		if (n >= alloc) {
			alloc *= 2;
			void *_new = realloc(result, alloc * sizeof(struct pod_metrics));
			if (_new == NULL) {
				closedir(dir);
				return (-1);
			}
			result = _new;
		}

		if (pod_metrics_collect(ent->d_name, &result[n]) == 0) {
			n++;
		}
	}

	closedir(dir);

	*metrics = result;
	*count = n;

	return (0);
}

/*
 * Export pod metrics as JSON
 */
int
pod_metrics_export_json(FILE *fp)
{
	struct pod_metrics *metrics;
	int count;
	int i;

	if (fp == NULL)
		return (-1);

	if (pod_metrics_list(&metrics, &count) != 0)
		return (-1);

	fprintf(fp, "{\n");
	fprintf(fp, "  \"pods\": [\n");

	for (i = 0; i < count; i++) {
		fprintf(fp, "    {\n");
		fprintf(fp, "      \"name\": \"%s\",\n", metrics[i].pod_name);
		fprintf(fp, "      \"namespace\": \"%s\",\n", metrics[i].namespace);
		fprintf(fp, "      \"node\": \"%s\",\n", metrics[i].node);
		fprintf(fp, "      \"memory_usage\": %lu,\n", metrics[i].mem_usage);
		fprintf(fp, "      \"memory_limit\": %lu,\n", metrics[i].mem_limit);
		fprintf(fp, "      \"cpu_usage\": %.4f,\n", metrics[i].cpu_usage);
		fprintf(fp, "      \"cpu_limit\": %.4f,\n", metrics[i].cpu_limit);
		fprintf(fp, "      \"process_count\": %lu,\n", metrics[i].process_count);
		fprintf(fp, "      \"file_descriptors\": %lu,\n", metrics[i].file_descriptors);
		fprintf(fp, "      \"timestamp\": %lu\n", metrics[i].timestamp);
		fprintf(fp, "    }%s\n", i < count - 1 ? "," : "");
	}

	fprintf(fp, "  ]\n");
	fprintf(fp, "}\n");

	free(metrics);
	return (0);
}

/*
 * Export pod metrics as Prometheus text format
 */
int
pod_metrics_export_prometheus(FILE *fp)
{
	struct pod_metrics *metrics;
	int count;
	int i;

	if (fp == NULL)
		return (-1);

	if (pod_metrics_list(&metrics, &count) != 0)
		return (-1);

	for (i = 0; i < count; i++) {
		fprintf(fp, "# HELP pod_memory_usage_bytes Memory usage in bytes\n");
		fprintf(fp, "# TYPE pod_memory_usage_bytes gauge\n");
		fprintf(fp, "pod_memory_usage_bytes{pod=\"%s\",namespace=\"%s\",node=\"%s\"} %.0f\n",
			metrics[i].pod_name, metrics[i].namespace, metrics[i].node,
			(double)metrics[i].mem_usage);

		fprintf(fp, "# HELP pod_cpu_usage_ratio CPU usage ratio (0-1)\n");
		fprintf(fp, "# TYPE pod_cpu_usage_ratio gauge\n");
		fprintf(fp, "pod_cpu_usage_ratio{pod=\"%s\",namespace=\"%s\",node=\"%s\"} %.4f\n",
			metrics[i].pod_name, metrics[i].namespace, metrics[i].node,
			metrics[i].cpu_usage / 100.0);

		fprintf(fp, "# HELP pod_process_count Number of processes\n");
		fprintf(fp, "# TYPE pod_process_count gauge\n");
		fprintf(fp, "pod_process_count{pod=\"%s\",namespace=\"%s\"} %lu\n",
			metrics[i].pod_name, metrics[i].namespace, metrics[i].process_count);
	}

	free(metrics);
	return (0);
}

/*
 * Serialize metrics to JSON
 */
char *
metrics_serialize_json(void)
{
	struct metric **list;
	char *json, *p;
	int count, i, n;
	size_t json_size, remaining;

	list = metrics_list(&count);
	if (list == NULL)
		return (NULL);

	json_size = 256 + (size_t)count * 512;
	json = malloc(json_size);
	if (json == NULL) {
		free(list);
		return (NULL);
	}

	p = json;
	remaining = json_size;

	n = snprintf(p, remaining, "{\n  \"metrics\": [\n");
	if (n < 0 || (size_t)n >= remaining) { free(json); free(list); return (NULL); }
	p += n; remaining -= (size_t)n;

	for (i = 0; i < count; i++) {
		n = snprintf(p, remaining,
			"    {\"name\": \"%s\", \"type\": %d, \"value\": %g}%s\n",
			list[i]->name, list[i]->type, list[i]->value,
			i < count - 1 ? "," : "");
		if (n < 0 || (size_t)n >= remaining) { free(json); free(list); return (NULL); }
		p += n; remaining -= (size_t)n;
	}

	n = snprintf(p, remaining, "  ]\n}\n");
	if (n < 0 || (size_t)n >= remaining) { free(json); free(list); return (NULL); }

	free(list);
	return (json);
}

/*
 * Serialize metrics to Prometheus text format
 */
char *
metrics_serialize_prometheus(void)
{
	struct metric **list;
	char *text, *p;
	int count, i, n;
	size_t text_size, remaining;

	list = metrics_list(&count);
	if (list == NULL)
		return (NULL);

	text_size = 256 + (size_t)count * 512;
	text = malloc(text_size);
	if (text == NULL) {
		free(list);
		return (NULL);
	}

	p = text;
	remaining = text_size;

	for (i = 0; i < count; i++) {
		const char *type_str;
		switch (list[i]->type) {
			case METRIC_TYPE_GAUGE: type_str = "gauge"; break;
			case METRIC_TYPE_COUNTER: type_str = "counter"; break;
			case METRIC_TYPE_HISTOGRAM: type_str = "histogram"; break;
			case METRIC_TYPE_SUMMARY: type_str = "summary"; break;
			default: type_str = "untyped";
		}

		n = snprintf(p, remaining, "# HELP %s %s\n", list[i]->name,
			list[i]->help[0] ? list[i]->help : list[i]->name);
		if (n < 0 || (size_t)n >= remaining) { free(text); free(list); return (NULL); }
		p += n; remaining -= (size_t)n;

		n = snprintf(p, remaining, "# TYPE %s %s\n", list[i]->name, type_str);
		if (n < 0 || (size_t)n >= remaining) { free(text); free(list); return (NULL); }
		p += n; remaining -= (size_t)n;

		n = snprintf(p, remaining, "%s %g\n", list[i]->name, list[i]->value);
		if (n < 0 || (size_t)n >= remaining) { free(text); free(list); return (NULL); }
		p += n; remaining -= (size_t)n;
	}

	free(list);
	return (text);
}

/*
 * Set alert threshold
 */
int
metrics_set_threshold(const char *name, double warning, double critical, int comparison)
{
	pthread_mutex_lock(&threshold_lock);

	void *_new = realloc(thresholds, (n_thresholds + 1) * sizeof(struct metrics_threshold));
	if (_new == NULL) {
		pthread_mutex_unlock(&threshold_lock);
		return (-1);
	}
	thresholds = _new;

	strlcpy(thresholds[n_thresholds].metric_name, name,
		sizeof(thresholds[n_thresholds].metric_name));
	thresholds[n_thresholds].warning = warning;
	thresholds[n_thresholds].critical = critical;
	thresholds[n_thresholds].comparison = comparison;
	n_thresholds++;

	pthread_mutex_unlock(&threshold_lock);

	return (0);
}

/*
 * Check thresholds and generate alerts
 */
int
metrics_check_thresholds(void)
{
	int i;

	pthread_mutex_lock(&threshold_lock);

	for (i = 0; i < n_thresholds; i++) {
		struct metric *m = metrics_query(thresholds[i].metric_name);
		if (m == NULL)
			continue;

		double value = m->value;
		int violated = 0;
		int severity = 0;  /* 0 = ok, 1 = warning, 2 = critical */

		if (thresholds[i].comparison == 0) {  /* above */
			if (value >= thresholds[i].critical)
				severity = 2;
			else if (value >= thresholds[i].warning)
				severity = 1;
		} else {  /* below */
			if (value <= thresholds[i].critical)
				severity = 2;
			else if (value <= thresholds[i].warning)
				severity = 1;
		}

		if (severity > 0) {
			/* Add alert */
			pthread_mutex_lock(&alert_lock);

			if (ocifbsd_realloc_grow((void **)&active_alerts,
				(n_alerts + 1) * sizeof(char *)) == 0) {
				char alert[512];
				snprintf(alert, sizeof(alert),
					"ALERT %s %s threshold: value=%.2f %s warning=%.2f critical=%.2f",
					severity == 2 ? "CRITICAL" : "WARNING",
					thresholds[i].metric_name,
					value,
					thresholds[i].comparison == 0 ? ">=" : "<=",
					thresholds[i].warning,
					thresholds[i].critical);

				active_alerts[n_alerts] = strdup(alert);
				if (active_alerts[n_alerts] != NULL)
					n_alerts++;
			}

			pthread_mutex_unlock(&alert_lock);
		}
	}

	pthread_mutex_unlock(&threshold_lock);

	return (0);
}

/*
 * Get active alerts as JSON
 */
char *
metrics_get_alerts_json(void)
{
	char *json, *p;
	int i;
	size_t json_size;

	pthread_mutex_lock(&alert_lock);

	json_size = 256 + (size_t)n_alerts * 512;
	json = malloc(json_size);
	if (json == NULL) {
		pthread_mutex_unlock(&alert_lock);
		return (NULL);
	}

	p = json;
	size_t remaining = json_size;
	int n;

	n = snprintf(p, remaining, "{\n  \"alerts\": [\n");
	if (n < 0 || (size_t)n >= remaining) { free(json); pthread_mutex_unlock(&alert_lock); return (NULL); }
	p += n; remaining -= (size_t)n;

	for (i = 0; i < n_alerts; i++) {
		char emsg[1024];

		n = snprintf(p, remaining, "    {\"message\": \"%s\"}%s\n",
			ocifbsd_json_escape(active_alerts[i], emsg, sizeof(emsg)),
			i < n_alerts - 1 ? "," : "");
		if (n < 0 || (size_t)n >= remaining) { free(json); pthread_mutex_unlock(&alert_lock); return (NULL); }
		p += n; remaining -= (size_t)n;
	}

	n = snprintf(p, remaining, "  ]\n}\n");
	if (n < 0 || (size_t)n >= remaining) { free(json); pthread_mutex_unlock(&alert_lock); return (NULL); }

	pthread_mutex_unlock(&alert_lock);

	return (json);
}
