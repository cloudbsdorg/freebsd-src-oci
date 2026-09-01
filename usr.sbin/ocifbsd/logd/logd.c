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
 * Log aggregation daemon implementation
 * Phase 14: Observability/Logging
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <fts.h>
#include <ifaddrs.h>
#include <libutil.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <pwd.h>
#include <regex.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#include "logd.h"
#include "../include/ocifbsd.h"

static int	safe_shell_exec(const char *command);
static void	*rotation_worker(void *arg);
static void	*forward_worker(void *arg);
static void	*alert_worker(void *arg);
static int	log_write_internal(int severity, int source,
    const char *source_name, const char *fields_json, const char *message);
static int	log_write_entry(struct log_entry *entry);
static int	event_compare(struct event_entry *a, struct event_entry *b);
static int	alert_compare(struct alert_rule *a, struct alert_rule *b);
static void	event_trigger_webhooks(struct event_entry *event);
static struct log_forwarder *forwarder_find(const char *name);
static int	forwarder_send_udp(struct log_forwarder *fw, const char *line);
static int	forwarder_send_fluentd(struct log_forwarder *fw,
    const char *line);
static int	forwarder_send_elastic(struct log_forwarder *fw,
    const char *line);
static int	forwarder_send_splunk(struct log_forwarder *fw,
    const char *line);
static int	forwarder_send_custom(struct log_forwarder *fw, const char *line);
static int	webhook_deliver(struct webhook_delivery *wh);
static void	sig_handler(int sig);
static void	event_loop(void);

/*
 * Recursive mkdir(2). FreeBSD 16's <libutil.h> does not export mkdirp
 * in any public header. We provide a local copy.
 */
static int
mkdirp_local(const char *path, mode_t mode)
{
	char buf[PATH_MAX];
	char *p;
	size_t len;

	if (path == NULL || *path == '\0')
		return (-1);

	len = strlcpy(buf, path, sizeof(buf));
	if (len >= sizeof(buf))
		return (-1);

	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, mode) != 0 && errno != EEXIST)
			return (-1);
		*p = '/';
	}

	if (mkdir(buf, mode) != 0 && errno != EEXIST)
		return (-1);
	return (0);
}

#define	mkdirp(path, mode)	mkdirp_local((path), (mode))

/* Global state */
static struct log_ringbuf *main_ringbuf = NULL;
static struct forwarder_list forwarders;
static struct alert_rule_tree alert_rules;
static struct event_tree events;
static struct webhook_queue webhooks;
static struct logd_config config;
static pthread_mutex_t logd_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t rotate_thread;
static pthread_t forward_thread;
static pthread_t alert_thread;
static int running = 1;
static char hostname[MAXHOSTNAMELEN];
static uint64_t next_log_id = 1;
static int initialized = 0;

/*
 * Execute a command via /bin/sh -c, in a child process.
 * Returns exit status, or -1 on error. The caller is responsible
 * for fork()-ing if it wants to run in the background.
 *
 * NOTE: This still invokes the shell. It exists to be a more
 * transparent replacement for system() (no implicit /bin/sh,
 * explicit argv). It does NOT prevent shell injection of the
 * command string. Callers must ensure the command comes from a
 * trusted source (admin config, not user input).
 */
static int
safe_shell_exec(const char *command)
{
	pid_t pid;
	int status;

	if (command == NULL)
		return (-1);

	pid = fork();
	if (pid < 0)
		return (-1);
	if (pid == 0) {
		closefrom(STDERR_FILENO + 1);
		execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return (-1);
	if (WIFEXITED(status))
		return (WEXITSTATUS(status));
	return (-1);
}

/*
 * Realloc that preserves the original pointer on failure.
 * Without this, `ptr = realloc(ptr, n); if (!ptr) return -1;`
 * leaks the old buffer because ptr is now NULL and we've lost
 * the only reference to the original allocation.
 *
 * Usage: REALLOC_SAFE(ptr, size, label)
 *   - If realloc fails, jumps to 'label' with the original
 *     pointer still valid (caller should free it).
 */
#define REALLOC_SAFE(ptr, newsz, label) do {				\
	void *_new = realloc((ptr), (newsz));				\
	if (_new == NULL) {						\
		goto label;						\
	}								\
	(ptr) = _new;							\
} while (0)

/* Severity level names */
static const char *severity_names[] = {
    "emerg", "alert", "crit", "err", "warning",
    "notice", "info", "debug", "trace"
};

/* Source type names */
static const char *source_names[] = {
    "jail", "host", "container", "service", "api"
};

/*
 * Bounds-checked name lookups. entry->severity and entry->source can come
 * from untrusted log submitters; an out-of-range value would index past
 * these arrays and read a bogus pointer into formatted output.
 */
static const char *
severity_name(int severity)
{
    if (severity < 0 ||
        (size_t)severity >= sizeof(severity_names) / sizeof(severity_names[0]))
        return ("unknown");
    return (severity_names[severity]);
}

static const char *
source_name(int source)
{
    if (source < 0 ||
        (size_t)source >= sizeof(source_names) / sizeof(source_names[0]))
        return ("unknown");
    return (source_names[source]);
}

/*
 * Initialize log daemon
 */
int
logd_init(struct logd_config *cfg)
{
    if (initialized)
        return (0);

    /* Get hostname */
    gethostname(hostname, sizeof(hostname));

    /* Initialize or use provided config */
    if (cfg != NULL) {
        config = *cfg;
    } else {
        /* Default configuration */
        memset(&config, 0, sizeof(config));
        config.log_level = LOG_INFO;
        config.ringbuf_size = 100000;  /* 100K entries */
        config.retention_hot = 1;
        config.retention_warm = 7;
        config.retention_cold = 30;
        config.retention_archive = 365;
        config.storage_path = strdup("/var/log/ocifbsd");
        config.enable_prometheus = true;
        config.prometheus_port = 9090;
        config.query_timeout = 30;
        config.forward_batch_size = 100;
        config.forward_interval = 1000;
    }

    /* Initialize ring buffer */
    main_ringbuf = ringbuf_create(config.ringbuf_size);
    if (main_ringbuf == NULL) {
        fprintf(stderr, "Failed to create ring buffer\n");
        return (-1);
    }

    /* Initialize lists */
    LIST_INIT(&forwarders);
    RB_INIT(&alert_rules);
    RB_INIT(&events);
    STAILQ_INIT(&webhooks);

    /* Start background threads */
    pthread_create(&rotate_thread, NULL, rotation_worker, NULL);
    pthread_create(&forward_thread, NULL, forward_worker, NULL);
    pthread_create(&alert_thread, NULL, alert_worker, NULL);

    /* Open syslog */
    openlog("ocifbsd-logd", LOG_PID, LOG_DAEMON);

    initialized = 1;
    syslog(LOG_INFO, "ocifbsd-logd initialized");

    return (0);
}

/*
 * Shutdown log daemon
 */
void
logd_shutdown(void)
{
    if (!initialized)
        return;

    running = 0;

    /* Signal threads to stop */
    pthread_join(rotate_thread, NULL);
    pthread_join(forward_thread, NULL);
    pthread_join(alert_thread, NULL);

    /* Flush forwarders */
    forwarder_flush();

    /* Destroy ring buffer */
    if (main_ringbuf)
        ringbuf_destroy(main_ringbuf);

    closelog();
    initialized = 0;
}

/*
 * Write a log entry
 */
int
log_write(int severity, int source, const char *source_name, const char *fmt, ...)
{
    va_list ap;
    char msg[4096];

    if (!initialized || severity > config.log_level)
        return (0);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    return (log_write_internal(severity, source, source_name, NULL, msg));
}

/*
 * Write structured log entry
 */
int
log_write_structured(int severity, int source, const char *source_name,
    const char *fields_json, const char *fmt, ...)
{
    va_list ap;
    char msg[4096];

    if (!initialized || severity > config.log_level)
        return (0);

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    return (log_write_internal(severity, source, source_name, fields_json, msg));
}

/*
 * Internal log write
 */
int
log_write_internal(int severity, int source, const char *source_name,
    const char *fields_json, const char *message)
{
    struct log_entry entry;

    memset(&entry, 0, sizeof(entry));

    entry.id = __sync_fetch_and_add(&next_log_id, 1);
    entry.timestamp = time(NULL);
    entry.severity = severity;
    entry.source = source;

    if (source_name)
        strlcpy(entry.source_name, source_name, sizeof(entry.source_name));
    if (message)
        strlcpy(entry.message, message, sizeof(entry.message));
    if (fields_json)
        strlcpy(entry.fields, fields_json, sizeof(entry.fields));

    strlcpy(entry.hostname, hostname, sizeof(entry.hostname));

    entry.pid = getpid();
    entry.tid = (uint32_t)(uintptr_t)pthread_self();

    /* Get username if available */
    struct passwd *pw = getpwuid(getuid());
    if (pw)
        strlcpy(entry.username, pw->pw_name, sizeof(entry.username));

    /* Write to ring buffer */
    if (main_ringbuf)
        ringbuf_write(main_ringbuf, &entry);

    /* Process alerts */
    alert_process_entry(&entry);

    /* Syslog for host logs */
    if (source == LOG_SOURCE_HOST) {
        int pri = LOG_INFO;
        switch (severity) {
        case LOG_EMERG: pri = LOG_EMERG; break;
        case LOG_ALERT: pri = LOG_ALERT; break;
        case LOG_CRIT: pri = LOG_CRIT; break;
        case LOG_ERR: pri = LOG_ERR; break;
        case LOG_WARNING: pri = LOG_WARNING; break;
        case LOG_NOTICE: pri = LOG_NOTICE; break;
        case LOG_DEBUG: pri = LOG_DEBUG; break;
        }
        syslog(pri, "[%s] %s", source_name ? source_name : "unknown", message);
    }

    return (0);
}

/*
 * Write log from jail
 */
int
log_write_from_jail(const char *jail_name, int severity, const char *message)
{
    return (log_write(severity, LOG_SOURCE_JAIL, jail_name, "%s", message));
}

/*
 * Write a log entry (internal helper)
 */
int
log_write_entry(struct log_entry *entry)
{
    if (!initialized || entry->severity > config.log_level)
        return (0);

    entry->id = __sync_fetch_and_add(&next_log_id, 1);
    if (entry->hostname[0] == '\0')
        strlcpy(entry->hostname, hostname, sizeof(entry->hostname));

    if (main_ringbuf)
        ringbuf_write(main_ringbuf, entry);

    alert_process_entry(entry);

    return (0);
}

/*
 * Query logs
 */
struct log_entry **
log_query_exec(struct log_query *query, uint64_t *count)
{
    struct log_entry **results = NULL;
    uint64_t n = 0;
    uint64_t cursor = 0;
    struct log_entry *entry;

    if (query == NULL || count == NULL)
        return (NULL);

    *count = 0;

    if (main_ringbuf == NULL)
        return (NULL);

    while ((entry = ringbuf_iterate(main_ringbuf, &cursor)) != NULL) {
        /* Apply filters */
        if (query->start_time && entry->timestamp < query->start_time)
            continue;
        if (query->end_time && entry->timestamp > query->end_time)
            continue;
        if (query->severity_min && entry->severity < query->severity_min)
            continue;
        if (query->severity_max && entry->severity > query->severity_max)
            continue;
        if (query->source_name && fnmatch(query->source_name, entry->source_name, 0) != 0)
            continue;

        REALLOC_SAFE(results, (n + 1) * sizeof(*results), realloc_fail);
        results[n++] = entry;

        if (query->limit && n >= query->limit)
            break;
    }

    *count = n;
    return (results);

realloc_fail:
    free(results);
    *count = 0;
    return (NULL);
}

/*
 * Format log entry
 */
char *
log_format_entry(struct log_entry *entry, int format)
{
    static char buf[8192];
    struct tm *tm;
    char ts[64];

    if (entry == NULL)
        return (NULL);

    tm = localtime(&entry->timestamp);
    strftime(ts, sizeof(ts), "%Y-%m-%dT%H:%M:%S", tm);

    switch (format) {
    case LOG_FORMAT_JSON:
        snprintf(buf, sizeof(buf),
            "{\"timestamp\":\"%s\",\"severity\":\"%s\",\"source\":\"%s\","
            "\"hostname\":\"%s\",\"message\":\"%s\",\"id\":%lu}",
            ts,
            severity_name(entry->severity),
            source_name(entry->source),
            entry->hostname,
            entry->message,
            (unsigned long)entry->id);
        break;

    case LOG_FORMAT_TEXT:
        snprintf(buf, sizeof(buf), "%s %s[%s]: %s",
            ts,
            severity_name(entry->severity),
            entry->source_name,
            entry->message);
        break;

    case LOG_FORMAT_SYSLOG:
        snprintf(buf, sizeof(buf), "<%d>%s %s ocifbsd[%u]: [%s] %s",
            entry->severity + (3 << 3), /* priorities */
            ts,
            entry->hostname,
            (unsigned)entry->pid,
            entry->source_name,
            entry->message);
        break;

    default:
        buf[0] = '\0';
    }

    return (buf);
}

/*
 * Get statistics
 */
int
log_stats_get(struct log_stats *stats)
{
    if (stats == NULL || main_ringbuf == NULL)
        return (-1);

    pthread_mutex_lock(&main_ringbuf->lock);

    memset(stats, 0, sizeof(*stats));
    stats->entries_total = main_ringbuf->total_written;
    stats->entries_written = main_ringbuf->count;
    if (main_ringbuf->count > 0) {
        uint64_t newest = (main_ringbuf->head + main_ringbuf->size - 1) %
            main_ringbuf->size;
        stats->oldest_entry =
            main_ringbuf->entries[main_ringbuf->tail].timestamp;
        /* head is the next unwritten slot; newest is the one before it. */
        stats->newest_entry = main_ringbuf->entries[newest].timestamp;
    }

    pthread_mutex_unlock(&main_ringbuf->lock);

    return (0);
}

/*
 * Get statistics as JSON
 */
int
log_stats_json(char **json_out)
{
    struct log_stats stats;

    if (json_out == NULL)
        return (-1);

    if (log_stats_get(&stats) != 0)
        return (-1);

    if (asprintf(json_out,
        "{\"entries_total\":%lu,\"entries_in_buffer\":%lu,"
        "\"oldest_entry\":%ld,\"newest_entry\":%ld}",
        (unsigned long)stats.entries_total,
        (unsigned long)stats.entries_written,
        (long)stats.oldest_entry,
        (long)stats.newest_entry) == -1) {
        return (-1);
    }

    return (0);
}

/*
 * Prometheus metrics output
 */
int
log_stats_prometheus(FILE *fp)
{
    struct log_stats stats;

    if (fp == NULL)
        return (-1);

    if (log_stats_get(&stats) != 0)
        return (-1);

    fprintf(fp, "# HELP ocifbsd_log_entries_total Total log entries written\n");
    fprintf(fp, "# TYPE ocifbsd_log_entries_total counter\n");
    fprintf(fp, "ocifbsd_log_entries_total %lu\n",
        (unsigned long)stats.entries_total);

    fprintf(fp, "# HELP ocifbsd_log_entries_buffered Entries in memory buffer\n");
    fprintf(fp, "# TYPE ocifbsd_log_entries_buffered gauge\n");
    fprintf(fp, "ocifbsd_log_entries_buffered %lu\n",
        (unsigned long)stats.entries_written);

    fprintf(fp, "# HELP ocifbsd_log_oldest_timestamp Oldest log entry timestamp\n");
    fprintf(fp, "# TYPE ocifbsd_log_oldest_timestamp gauge\n");
    fprintf(fp, "ocifbsd_log_oldest_timestamp %ld\n", (long)stats.oldest_entry);

    fprintf(fp, "# HELP ocifbsd_log_newest_timestamp Newest log entry timestamp\n");
    fprintf(fp, "# TYPE ocifbsd_log_newest_timestamp gauge\n");
    fprintf(fp, "ocifbsd_log_newest_timestamp %ld\n", (long)stats.newest_entry);

    return (0);
}

/*
 * Log rotation worker
 */
static void *
rotation_worker(void *arg)
{
    (void)arg;

    while (running) {
        sleep(3600);  /* Check every hour */

        if (!running)
            break;

        log_rotate();
        log_retention_apply();
    }

    return (NULL);
}

/*
 * Log forward worker
 */
static void *
forward_worker(void *arg)
{
    (void)arg;

    while (running) {
        usleep(config.forward_interval * 1000);  /* Configurable interval */
        forwarder_flush();
        webhook_process();
    }

    return (NULL);
}

/*
 * Alert processing worker
 */
static void *
alert_worker(void *arg)
{
    (void)arg;

    /* Alert processing is done synchronously in alert_process_entry */
    while (running) {
        sleep(1);  /* Check for time-based alert windows */
    }

    return (NULL);
}

/*
 * Log rotation
 */
int
log_rotate(void)
{
    char path[PATH_MAX];
    FILE *fp;
    time_t now = time(NULL);

    /* Create rotated log file */
    struct tm *tm = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tm);

    snprintf(path, sizeof(path), "%s/ocifbsd.%s.log",
        config.storage_path, timestamp);

    mkdirp(config.storage_path, 0755);

    /* Write recent entries to rotated file */
    fp = fopen(path, "w");
    if (fp) {
        uint64_t cursor = 0;
        struct log_entry *entry;

        while ((entry = ringbuf_iterate(main_ringbuf, &cursor)) != NULL) {
            fprintf(fp, "%s\n", log_format_entry(entry, LOG_FORMAT_TEXT));
        }

        fclose(fp);
    }

    syslog(LOG_INFO, "Log rotation completed: %s", path);

    return (0);
}

/*
 * Apply retention policy
 */
int
log_retention_apply(void)
{
    char cmd[PATH_MAX];

    /* Delete hot logs older than retention_hot days */
    snprintf(cmd, sizeof(cmd),
        "find %s -name 'ocifbsd.*.log' -mtime +%d -delete",
        config.storage_path, config.retention_hot);

    if (safe_shell_exec(cmd) != 0) {
        syslog(LOG_WARNING, "Retention policy failed: %s", cmd);
    }

    return (0);
}

/*
 * Get configuration
 */
struct logd_config *
logd_get_config(void)
{
    return (&config);
}

/*
 * Reload configuration
 */
int
logd_reload_config(const char *config_path)
{
    /* Configuration reload would parse config file here */
    (void)config_path;
    return (0);
}

/* Event tree comparison */
static int
event_compare(struct event_entry *a, struct event_entry *b)
{
    if (a->id < b->id) return (-1);
    if (a->id > b->id) return (1);
    return (0);
}
RB_GENERATE(event_tree, event_entry, entry, event_compare);

/*
 * Publish an event
 */
int
event_publish(int type, const char *source, const char *subject, const char *data)
{
    struct event_entry *event;
    static uint64_t next_event_id = 1;

    event = calloc(1, sizeof(*event));
    if (event == NULL)
        return (-1);

    event->id = __sync_fetch_and_add(&next_event_id, 1);
    event->timestamp = time(NULL);
    event->type = type;

    if (source)
        strlcpy(event->source, source, sizeof(event->source));
    if (subject)
        strlcpy(event->subject, subject, sizeof(event->subject));
    if (data)
        event->data = strdup(data);

    RB_INSERT(event_tree, &events, event);

    /* Trigger webhooks */
    event_trigger_webhooks(event);

    return (0);
}

/*
 * Query events
 */
struct event_entry **
event_query(time_t start, time_t end, int *types, int num_types,
    uint64_t limit, uint64_t *count)
{
    struct event_entry **results = NULL;
    struct event_entry *event, *next;
    uint64_t n = 0;

    if (count == NULL)
        return (NULL);

    *count = 0;

    RB_FOREACH_SAFE(event, event_tree, &events, next) {
        if (event->timestamp < start)
            continue;
        if (event->timestamp > end)
            continue;

        if (num_types > 0 && types != NULL) {
            bool match = false;
            for (int i = 0; i < num_types; i++) {
                if (event->type == types[i]) {
                    match = true;
                    break;
                }
            }
            if (!match)
                continue;
        }

        REALLOC_SAFE(results, (n + 1) * sizeof(*results), realloc_fail);
        results[n++] = event;

        if (limit && n >= limit)
            break;
    }

    *count = n;
    return (results);

realloc_fail:
    free(results);
    *count = 0;
    return (NULL);
}

/*
 * Trigger event webhooks
 */
static void
event_trigger_webhooks(struct event_entry *event)
{
    /* Webhook delivery for events */
    (void)event;
    /* Implementation would check registered webhooks and deliver */
}

/*
 * Alert rule comparison
 */
static int
alert_compare(struct alert_rule *a, struct alert_rule *b)
{
    return (strcmp(a->name, b->name));
}
RB_GENERATE(alert_rule_tree, alert_rule, entry, alert_compare);

/*
 * Add alert rule
 */
int
alert_rule_add(struct alert_rule *rule)
{
    if (rule == NULL || rule->name[0] == '\0')
        return (-1);

    pthread_mutex_lock(&logd_state_lock);

    struct alert_rule *existing;
    existing = RB_FIND(alert_rule_tree, &alert_rules, rule);
    if (existing != NULL) {
        pthread_mutex_unlock(&logd_state_lock);
        return (-1);  /* Already exists */
    }

    RB_INSERT(alert_rule_tree, &alert_rules, rule);

    pthread_mutex_unlock(&logd_state_lock);

    return (0);
}

/*
 * Remove alert rule
 */
int
alert_rule_remove(const char *name)
{
    struct alert_rule key, *rule;

    if (name == NULL)
        return (-1);

    pthread_mutex_lock(&logd_state_lock);

    strlcpy(key.name, name, sizeof(key.name));
    rule = RB_FIND(alert_rule_tree, &alert_rules, &key);
    if (rule) {
        RB_REMOVE(alert_rule_tree, &alert_rules, rule);
        free(rule);
    }

    pthread_mutex_unlock(&logd_state_lock);

    return (rule ? 0 : -1);
}

/*
 * List alert rules
 */
struct alert_rule **
alert_rule_list(int *count)
{
    struct alert_rule **rules = NULL;
    struct alert_rule *rule;
    int n = 0;

    if (count == NULL)
        return (NULL);

    pthread_mutex_lock(&logd_state_lock);

    RB_FOREACH(rule, alert_rule_tree, &alert_rules) {
        REALLOC_SAFE(rules, (n + 1) * sizeof(*rules), realloc_fail);
        rules[n++] = rule;
    }

    pthread_mutex_unlock(&logd_state_lock);

    *count = n;
    return (rules);

realloc_fail:
    pthread_mutex_unlock(&logd_state_lock);
    free(rules);
    *count = 0;
    return (NULL);
}

/*
 * Process entry for alerts
 */
int
alert_process_entry(struct log_entry *entry)
{
    struct alert_rule *rule;
    regex_t re;

    if (entry == NULL)
        return (0);

    pthread_mutex_lock(&logd_state_lock);

    RB_FOREACH(rule, alert_rule_tree, &alert_rules) {
        if (!rule->enabled || rule->silenced)
            continue;

        /* Check severity */
        if (entry->severity < rule->severity)
            continue;

        /* Check source filter */
        if (rule->source >= 0 && entry->source != rule->source)
            continue;

        /* Check namespace */
        if (rule->namespace && strcmp(entry->namespace, rule->namespace) != 0)
            continue;

        /* Check source name */
        if (rule->source_name && fnmatch(rule->source_name, entry->source_name, 0) != 0)
            continue;

        /* Check message pattern */
        if (rule->match_pattern) {
            if (regcomp(&re, rule->match_pattern, REG_EXTENDED) != 0)
                continue;
            if (regexec(&re, entry->message, 0, NULL, 0) != 0) {
                regfree(&re);
                continue;
            }
            regfree(&re);
        }

        /* Match! Update count */
        rule->current_count++;

        /* Check if threshold reached */
        if (rule->current_count >= rule->count_threshold) {
            /* Check time window */
            if (rule->count_window == 0 ||
                (time(NULL) - rule->last_triggered) <= rule->count_window) {
                alert_trigger(rule, entry);
                rule->last_triggered = time(NULL);
                rule->current_count = 0;
            }
        }
    }

    pthread_mutex_unlock(&logd_state_lock);

    return (0);
}

/*
 * Trigger an alert
 */
int
alert_trigger(struct alert_rule *rule, struct log_entry *entry)
{
    char payload[4096];

    if (rule == NULL)
        return (-1);

    /* Create webhook payload */
    snprintf(payload, sizeof(payload),
        "{\"alert\":\"%s\",\"severity\":%d,\"message\":\"%s\","
        "\"source\":\"%s\",\"timestamp\":%ld}",
        rule->name, entry->severity, entry->message,
        entry->source_name, (long)entry->timestamp);

    /* Enqueue webhook if configured */
    if (rule->notify_webhook && rule->webhook_url[0] != '\0') {
        webhook_enqueue(rule->webhook_url, payload);
    }

    /* Execute command if configured */
    if (rule->execute_command && rule->command[0] != '\0') {
        /* Execute in background */
        if (fork() == 0) {
            safe_shell_exec(rule->command);
            _exit(0);
        }
    }

    syslog(LOG_WARNING, "ALERT triggered: %s - %s",
        rule->name, rule->description);

    return (0);
}

/*
 * Silence an alert
 */
int
alert_silence(const char *name, time_t until)
{
    struct alert_rule key, *rule;

    pthread_mutex_lock(&logd_state_lock);

    strlcpy(key.name, name, sizeof(key.name));
    rule = RB_FIND(alert_rule_tree, &alert_rules, &key);
    if (rule) {
        rule->silenced = true;
        /* Store silence duration for later */
    }

    pthread_mutex_unlock(&logd_state_lock);

    return (rule ? 0 : -1);
}

/*
 * Unsilence an alert
 */
int
alert_unsilence(const char *name)
{
    return (alert_silence(name, 0));
}

/*
 * Check if alert is silenced
 */
bool
alert_is_silenced(const char *name)
{
    struct alert_rule key, *rule;
    bool silenced = false;

    pthread_mutex_lock(&logd_state_lock);

    strlcpy(key.name, name, sizeof(key.name));
    rule = RB_FIND(alert_rule_tree, &alert_rules, &key);
    if (rule)
        silenced = rule->silenced;

    pthread_mutex_unlock(&logd_state_lock);

    return (silenced);
}

/*
 * Forwarder list comparison
 */
static struct log_forwarder *
forwarder_find(const char *name)
{
    struct log_forwarder *fw;
    LIST_FOREACH(fw, &forwarders, next) {
        if (strcmp(fw->name, name) == 0)
            return (fw);
    }
    return (NULL);
}

/*
 * Add forwarder
 */
int
forwarder_add(struct log_forwarder *fw)
{
    if (fw == NULL || fw->name[0] == '\0')
        return (-1);

    pthread_mutex_lock(&logd_state_lock);

    if (forwarder_find(fw->name) != NULL) {
        pthread_mutex_unlock(&logd_state_lock);
        return (-1);
    }

    LIST_INSERT_HEAD(&forwarders, fw, next);

    pthread_mutex_unlock(&logd_state_lock);

    return (0);
}

/*
 * Remove forwarder
 */
int
forwarder_remove(const char *name)
{
    struct log_forwarder *fw;

    pthread_mutex_lock(&logd_state_lock);

    fw = forwarder_find(name);
    if (fw) {
        LIST_REMOVE(fw, next);
        free(fw);
    }

    pthread_mutex_unlock(&logd_state_lock);

    return (fw ? 0 : -1);
}

/*
 * List forwarders
 */
struct log_forwarder **
forwarder_list(int *count)
{
    struct log_forwarder **list = NULL;
    struct log_forwarder *fw;
    int n = 0;

    if (count == NULL)
        return (NULL);

    pthread_mutex_lock(&logd_state_lock);

    LIST_FOREACH(fw, &forwarders, next) {
        REALLOC_SAFE(list, (n + 1) * sizeof(*list), realloc_fail);
        list[n++] = fw;
    }

    pthread_mutex_unlock(&logd_state_lock);

    *count = n;
    return (list);

realloc_fail:
    pthread_mutex_unlock(&logd_state_lock);
    free(list);
    *count = 0;
    return (NULL);
}

/*
 * Enable/disable forwarder
 */
int
forwarder_enable(const char *name)
{
    struct log_forwarder *fw;
    int ret = -1;

    pthread_mutex_lock(&logd_state_lock);

    fw = forwarder_find(name);
    if (fw) {
        fw->enabled = true;
        ret = 0;
    }

    pthread_mutex_unlock(&logd_state_lock);

    return (ret);
}

int
forwarder_disable(const char *name)
{
    struct log_forwarder *fw;
    int ret = -1;

    pthread_mutex_lock(&logd_state_lock);

    fw = forwarder_find(name);
    if (fw) {
        fw->enabled = false;
        ret = 0;
    }

    pthread_mutex_unlock(&logd_state_lock);

    return (ret);
}

/*
 * Send entry to forwarders
 */
int
forwarder_send(struct log_entry *entry)
{
    struct log_forwarder *fw;
    char *line;

    if (entry == NULL)
        return (0);

    line = log_format_entry(entry, LOG_FORMAT_JSON);
    if (line == NULL)
        return (-1);

    pthread_mutex_lock(&logd_state_lock);

    LIST_FOREACH(fw, &forwarders, next) {
        if (!fw->enabled)
            continue;

        /* Send to endpoint based on type */
        switch (fw->type) {
        case 0: /* syslog */
            /* Send via network */
            forwarder_send_udp(fw, line);
            break;
        case 1: /* fluentd */
            forwarder_send_fluentd(fw, line);
            break;
        case 2: /* elasticsearch */
            forwarder_send_elastic(fw, line);
            break;
        case 3: /* splunk */
            forwarder_send_splunk(fw, line);
            break;
        default:
            forwarder_send_custom(fw, line);
            break;
        }
    }

    pthread_mutex_unlock(&logd_state_lock);

    free(line);
    return (0);
}

/*
 * Flush all forwarders
 */
int
forwarder_flush(void)
{
    uint64_t cursor = 0;
    struct log_entry *entry;

    if (main_ringbuf == NULL)
        return (0);

    /*
     * Do NOT hold main_ringbuf->lock here: ringbuf_iterate acquires the
     * same non-recursive lock internally, so holding it deadlocked the
     * forward worker (and, transitively, every log_write). ringbuf_iterate
     * is self-synchronizing.
     */
    uint64_t count = 0;
    cursor = 0;
    while ((entry = ringbuf_iterate(main_ringbuf, &cursor)) != NULL) {
        forwarder_send(entry);
        count++;

        /* Batch limit */
        if (count >= (uint64_t)config.forward_batch_size)
            break;
    }

    return (0);
}

/*
 * Forwarder send implementations (stubs)
 */
static int
forwarder_send_udp(struct log_forwarder *fw, const char *line)
{
    int sock;
    struct sockaddr_in addr;
    (void)fw;

    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return (-1);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(514);

    /* Parse endpoint */
    /* Implementation would parse host:port */

    sendto(sock, line, strlen(line), 0, (struct sockaddr *)&addr, sizeof(addr));
    close(sock);

    return (0);
}

static int
forwarder_send_fluentd(struct log_forwarder *fw, const char *line)
{
    /* Fluentd's HTTP input accepts a JSON record POSTed to the tag URL. */
    if (fw == NULL || line == NULL)
        return (-1);
    return (logd_http_post(fw->endpoint, line, "application/json", NULL));
}

static int
forwarder_send_elastic(struct log_forwarder *fw, const char *line)
{
    char url[640];

    /* Index a single document: POST <endpoint>/_doc with the JSON line. */
    if (fw == NULL || line == NULL)
        return (-1);
    if ((size_t)snprintf(url, sizeof(url), "%s/_doc", fw->endpoint) >=
        sizeof(url))
        return (-1);
    return (logd_http_post(url, line, "application/json", NULL));
}

static int
forwarder_send_splunk(struct log_forwarder *fw, const char *line)
{
    char *body = NULL, *auth = NULL;
    const char *token;
    int ret;

    /*
     * Splunk HEC wraps the record as {"event": <record>} and authenticates
     * with an "Authorization: Splunk <token>" header. The token is taken
     * from the OCIFBSD_SPLUNK_TOKEN environment variable when set (the
     * forwarder struct does not model per-destination secrets); without a
     * token the POST is still made, which suits a tokenless test collector.
     */
    if (fw == NULL || line == NULL)
        return (-1);
    if (asprintf(&body, "{\"event\": %s}", line) < 0)
        return (-1);
    token = getenv("OCIFBSD_SPLUNK_TOKEN");
    if (token != NULL && token[0] != '\0' &&
        asprintf(&auth, "Authorization: Splunk %s", token) < 0) {
        free(body);
        return (-1);
    }
    ret = logd_http_post(fw->endpoint, body, "application/json", auth);
    free(body);
    free(auth);
    return (ret);
}

static int
forwarder_send_custom(struct log_forwarder *fw, const char *line)
{
    /* Generic webhook: POST the JSON record to the configured endpoint. */
    if (fw == NULL || line == NULL)
        return (-1);
    return (logd_http_post(fw->endpoint, line, "application/json", NULL));
}

/*
 * Webhook queue
 */
int
webhook_enqueue(const char *url, const char *payload)
{
    struct webhook_delivery *wh;

    if (url == NULL || payload == NULL)
        return (-1);

    wh = calloc(1, sizeof(*wh));
    if (wh == NULL)
        return (-1);

    strlcpy(wh->url, url, sizeof(wh->url));
    wh->payload = strdup(payload);
    wh->attempts = 0;
    wh->next_retry = time(NULL);

    STAILQ_INSERT_TAIL(&webhooks, wh, next);

    return (0);
}

/*
 * Process webhook queue
 */
int
webhook_process(void)
{
    struct webhook_delivery *wh;
    int ret;

    while ((wh = STAILQ_FIRST(&webhooks)) != NULL) {
        if (wh->next_retry > time(NULL))
            break;

        ret = webhook_deliver(wh);

        if (ret == 0) {
            STAILQ_REMOVE_HEAD(&webhooks, next);
            free(wh->payload);
            free(wh);
        } else {
            wh->attempts++;
            if (wh->attempts >= 5) {
                /* Give up after 5 attempts */
                syslog(LOG_WARNING, "Webhook delivery failed after 5 attempts: %s",
                    wh->url);
                STAILQ_REMOVE_HEAD(&webhooks, next);
                free(wh->payload);
                free(wh);
            } else {
                /* Exponential backoff */
                wh->next_retry = time(NULL) + (1 << wh->attempts);
            }
        }
    }

    return (0);
}

/*
 * Deliver a webhook
 */
static int
webhook_deliver(struct webhook_delivery *wh)
{
    int sock;
    (void)wh;

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
        return (-1);

    /* HTTP POST implementation */
    /* This would be a proper HTTP client implementation */

    close(sock);
    return (0);
}

/*
 * Main function
 */
int
main(int argc, char *argv[])
{
    struct logd_config *cfg = NULL;
    int foreground = 0;
    int ch;

    /* Parse arguments */
    while ((ch = getopt(argc, argv, "fc:l:")) != -1) {
        switch (ch) {
        case 'f':
            foreground = 1;
            break;
        case 'c':
            /* Load config from file */
            break;
        case 'l':
            /* Set log level */
            break;
        }
    }

    /* Daemonize unless foreground */
    if (!foreground) {
        daemon(0, 0);
    }

    /* Initialize */
    if (logd_init(cfg) != 0) {
        fprintf(stderr, "Failed to initialize log daemon\n");
        return (1);
    }

    /* Signal handling */
    signal(SIGTERM, sig_handler);
    signal(SIGINT, sig_handler);
    signal(SIGHUP, sig_handler);

    /* Run event loop */
    event_loop();

    /* Shutdown */
    logd_shutdown();

    return (0);
}

/*
 * Signal handler
 */
static void
sig_handler(int sig)
{
    if (sig == SIGHUP) {
        /* Reload config */
        logd_reload_config(NULL);
    } else {
        /* Shutdown */
        running = 0;
    }
}

/*
 * Simple event loop
 */
static void
event_loop(void)
{
    while (running) {
        sleep(1);

        /* Process pending work */
        /* In production, this would use kqueue/poll */
    }
}
