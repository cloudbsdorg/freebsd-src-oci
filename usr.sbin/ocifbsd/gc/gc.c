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
 * Garbage collection daemon implementation
 * Phase 15: Garbage Collection
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#include <dirent.h>

#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <libutil.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "gc.h"
#include "../include/ocifbsd.h"

/*
 * Runtime paths used by the GC worker. These are local to gc.c for
 * now; eventually they should come from the main ocifbsd config.
 */
#ifndef OCIFBSD_BIN
#define OCIFBSD_BIN		"/usr/sbin/ocifbsd"
#endif
#ifndef OCIFBSD_ZFS_POOL
#define OCIFBSD_ZFS_POOL	"tank/ocifbsd"
#endif
#ifndef OCIFBSD_IMAGE_DIR
#define OCIFBSD_IMAGE_DIR	"/var/lib/ocifbsd/images"
#endif

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

static void *gc_worker(void *arg);
static void *gc_timer(void *arg);
static int gc_process_item(struct gc_item *item);
static int gc_queue_item(int type, const char *name, const char *ns,
	int priority);
static int gc_delete_container(const char *name);
static int gc_delete_image(const char *name);
static int gc_delete_volume(const char *name);
static int gc_delete_network(const char *name);
static int gc_is_container_orphaned(const char *name);
static int gc_is_container_stopped(const char *name, time_t cutoff);
static int gc_image_is_pinned(const char *name);
static int gc_is_volume_orphaned(const char *name);
static int gc_is_bridge_orphaned(const char *name);
static void sig_handler(int sig);

/* Global state */
static struct gc_config config;
static struct gc_queue gc_queue;
static struct orphan_tree orphans;
static struct gc_stats stats;
static pthread_mutex_t gc_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t gc_worker_thread;
static pthread_t gc_timer_thread;
static int running = 1;
static int initialized = 0;

/*
 * Safely execute a command without shell interpolation.
 * argv must be NULL-terminated. Returns exit status, or -1 on error.
 */
static int
safe_execv(const char *path, char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return (-1);

	if (pid == 0) {
		closefrom(STDERR_FILENO + 1);
		execv(path, argv);
		_exit(127);
	}

	if (waitpid(pid, &status, 0) < 0)
		return (-1);

	if (WIFEXITED(status))
		return (WEXITSTATUS(status));

	return (-1);
}

/*
 * Initialize GC daemon
 */
int
gc_init(struct gc_config *cfg)
{
	if (initialized)
		return (0);

	/* Initialize configuration */
	if (cfg != NULL) {
		config = *cfg;
	} else {
		memset(&config, 0, sizeof(config));
		config.container_ttl = 300;         /* 5 minutes */
		config.stopped_container_ttl = 600; /* 10 minutes */
		config.max_containers = 1000;
		config.gc_stopped_containers = true;
		config.gc_finished_pods = true;
		config.image_ttl = 86400 * 7;      /* 7 days */
		config.image_high_threshold = 85;
		config.image_low_threshold = 75;
		/*
		 * Volume and network GC are off by default: the runtime has no
		 * standalone volume/network object model to collect (see
		 * gc_delete_volume/gc_delete_network). Enable only when a backend
		 * exists, else GC would queue work it cannot perform.
		 */
		config.gc_orphaned_volumes = false;
		config.gc_orphaned_networks = false;
		config.gc_unused_networks = false;
		config.node_grace_period = 300;     /* 5 minutes */
		config.gc_orphaned_pods = true;
		config.gc_failed_jobs = true;
		config.gc_interval = 300;           /* 5 minutes */
		config.gc_batch_size = 100;
		config.gc_concurrency = 4;
	}

	/* Initialize queues */
	TAILQ_INIT(&gc_queue);
	RB_INIT(&orphans);
	memset(&stats, 0, sizeof(stats));

	/* Start worker threads */
	pthread_create(&gc_worker_thread, NULL, gc_worker, NULL);
	pthread_create(&gc_timer_thread, NULL, gc_timer, NULL);

	/* Open syslog */
	openlog("ocifbsd-gc", LOG_PID, LOG_DAEMON);

	syslog(LOG_INFO, "ocifbsd-gc initialized");

	initialized = 1;
	return (0);
}

/*
 * Shutdown GC daemon
 */
void
gc_shutdown(void)
{
	if (!initialized)
		return;

	running = 0;

	pthread_join(gc_worker_thread, NULL);
	pthread_join(gc_timer_thread, NULL);

	closelog();
	initialized = 0;
}

/*
 * Worker thread
 */
static void *
gc_worker(void *arg)
{
	struct gc_item *item;
	int ret;

	(void)arg;

	while (running) {
		pthread_mutex_lock(&gc_lock);

		item = TAILQ_FIRST(&gc_queue);
		if (item) {
			TAILQ_REMOVE(&gc_queue, item, next);
			pthread_mutex_unlock(&gc_lock);

			/* Process item */
			ret = gc_process_item(item);

			pthread_mutex_lock(&gc_lock);
			if (ret == 0) {
				item->status = GC_STATUS_COMPLETE;
				stats.items_collected++;
				free(item);  /* dequeued + done: was leaked */
			} else {
				item->status = GC_STATUS_FAILED;
				item->attempts++;
				if (item->attempts < 3) {
					/* Requeue for retry */
					TAILQ_INSERT_TAIL(&gc_queue, item, next);
				} else {
					stats.items_failed++;
					free(item);  /* permanently failed: was leaked */
				}
			}
			pthread_mutex_unlock(&gc_lock);
		} else {
			pthread_mutex_unlock(&gc_lock);
			usleep(100000);  /* 100ms */
		}
	}

	return (NULL);
}

/*
 * Timer thread - triggers periodic GC
 */
static void *
gc_timer(void *arg)
{
	(void)arg;

	while (running) {
		sleep(config.gc_interval);

		if (!running)
			break;

		gc_detect_orphans();

		/* Run scheduled GCs */
		gc_run_now(GC_TYPE_ALL);
	}

	return (NULL);
}

/*
 * Process a single GC item
 */
static int
gc_process_item(struct gc_item *item)
{
	int ret = -1;

	if (item == NULL)
		return (-1);

	switch (item->type) {
	case GC_TYPE_CONTAINER:
		ret = gc_delete_container(item->name);
		break;
	case GC_TYPE_IMAGE:
		ret = gc_delete_image(item->name);
		break;
	case GC_TYPE_VOLUME:
		ret = gc_delete_volume(item->name);
		break;
	case GC_TYPE_NETWORK:
		ret = gc_delete_network(item->name);
		break;
	default:
		syslog(LOG_WARNING, "Unknown GC item type: %d", item->type);
	}

	return (ret);
}

/*
 * Delete a container by delegating to the ocifbsd runtime CLI
 * ("ocifbsd delete <name>"); honors dry-run.
 */
static int
gc_delete_container(const char *name)
{
	int ret;

	if (config.dry_run) {
		syslog(LOG_INFO, "[DRY RUN] Would delete container: %s", name);
		return (0);
	}

	char *argv[] = { OCIFBSD_BIN, "delete", (char *)name, NULL };
	ret = safe_execv(OCIFBSD_BIN, argv);

	return (ret == 0 ? 0 : -1);
}

/*
 * Delete an image by delegating to the ocifbsd runtime CLI
 * ("ocifbsd rmi <name>"); honors dry-run.
 */
static int
gc_delete_image(const char *name)
{
	int ret;

	if (config.dry_run) {
		syslog(LOG_INFO, "[DRY RUN] Would delete image: %s", name);
		return (0);
	}

	char *argv[] = { OCIFBSD_BIN, "rmi", (char *)name, NULL };
	ret = safe_execv(OCIFBSD_BIN, argv);

	return (ret == 0 ? 0 : -1);
}

/*
 * Delete a volume by delegating to the ocifbsd runtime CLI
 * ("ocifbsd volume rm <name>"); honors dry-run.
 */
static int
gc_delete_volume(const char *name)
{
	if (config.dry_run) {
		syslog(LOG_INFO, "[DRY RUN] Would delete volume: %s", name);
		return (0);
	}

	/*
	 * The runtime has no standalone volume object model and no "ocifbsd
	 * volume rm" subcommand, so there is nothing to collect. Skip cleanly
	 * (rather than exec a non-existent command that always errors) until a
	 * volume backend exists. Volume GC is disabled by default in gc_init().
	 */
	syslog(LOG_INFO, "volume GC not supported (no volume backend); "
		"skipping %s", name);
	return (0);
}

/*
 * Delete a network. The runtime exposes no "ocifbsd network rm" subcommand
 * (network config is per-container via "network list|set"), so there is no
 * standalone network object to collect. Skip cleanly; honors dry-run.
 * Network GC is disabled by default in gc_init().
 */
static int
gc_delete_network(const char *name)
{
	if (config.dry_run) {
		syslog(LOG_INFO, "[DRY RUN] Would delete network: %s", name);
		return (0);
	}

	syslog(LOG_INFO, "network GC not supported (no standalone network "
		"object); skipping %s", name);
	return (0);
}

/*
 * Get configuration
 */
struct gc_config *
gc_get_config(void)
{
	return (&config);
}

/*
 * Set configuration
 */
int
gc_set_config(struct gc_config *cfg)
{
	if (cfg == NULL)
		return (-1);

	pthread_mutex_lock(&gc_lock);
	config = *cfg;
	pthread_mutex_unlock(&gc_lock);

	return (0);
}

/*
 * Run GC now
 */
int
gc_run_now(int type)
{
	syslog(LOG_INFO, "Running GC: type=%d", type);

	pthread_mutex_lock(&gc_lock);
	gc_log_start(type);

	switch (type) {
	case GC_TYPE_ALL:
		gc_container_orphans();
		gc_stopped_containers(config.stopped_container_ttl);
		gc_finished_pods(config.container_ttl);
		gc_unused_images(config.image_ttl);
		gc_image_space(config.image_high_threshold, config.image_low_threshold);
		gc_orphaned_volumes();
		gc_released_pvc();
		gc_orphaned_networks();
		gc_unused_networks(config.image_ttl);
		gc_dead_node_resources();
		gc_stale_registrations();
		break;
	case GC_TYPE_CONTAINER:
		gc_container_orphans();
		gc_stopped_containers(config.stopped_container_ttl);
		gc_finished_pods(config.container_ttl);
		break;
	case GC_TYPE_IMAGE:
		gc_unused_images(config.image_ttl);
		gc_image_space(config.image_high_threshold, config.image_low_threshold);
		break;
	case GC_TYPE_VOLUME:
		gc_orphaned_volumes();
		gc_released_pvc();
		break;
	case GC_TYPE_NETWORK:
		gc_orphaned_networks();
		gc_unused_networks(config.image_ttl);
		break;
	case GC_TYPE_NODE:
		gc_dead_node_resources();
		gc_stale_registrations();
		break;
	}

	stats.last_run = time(NULL);
	gc_log_finish(type, 0, 0);

	pthread_mutex_unlock(&gc_lock);

	return (0);
}

/*
 * Run full GC
 */
int
gc_run_full(void)
{
	int ret;

	pthread_mutex_lock(&gc_lock);
	stats.last_full_gc = time(NULL);
	pthread_mutex_unlock(&gc_lock);

	ret = gc_run_now(GC_TYPE_ALL);
	ret |= gc_detect_orphans();

	return (ret);
}

/*
 * Schedule GC
 */
int
gc_schedule(int type, time_t when)
{
	struct gc_item *item;
	time_t now = time(NULL);

	if (when <= now)
		return (gc_run_now(type));

	item = calloc(1, sizeof(*item));
	if (item == NULL)
		return (-1);

	item->id = 1;  /* Would use atomic counter */
	item->type = type;
	item->scheduled = when;
	item->status = GC_STATUS_PENDING;
	item->priority = GC_PRIORITY_NORMAL;

	pthread_mutex_lock(&gc_lock);
	TAILQ_INSERT_TAIL(&gc_queue, item, next);
	pthread_mutex_unlock(&gc_lock);

	return (0);
}

/*
 * Queue item for GC
 */
int
gc_queue_item(int type, const char *name, const char *ns, int priority)
{
	struct gc_item *item;

	item = calloc(1, sizeof(*item));
	if (item == NULL)
		return (-1);

	item->id = 1;
	item->type = type;
	item->priority = priority;
	item->created = time(NULL);
	item->scheduled = time(NULL);
	item->status = GC_STATUS_PENDING;

	if (name)
		strlcpy(item->name, name, sizeof(item->name));
	if (ns)
		strlcpy(item->namespace, ns, sizeof(item->namespace));

	pthread_mutex_lock(&gc_lock);
	TAILQ_INSERT_TAIL(&gc_queue, item, next);
	pthread_mutex_unlock(&gc_lock);

	return (0);
}

/*
 * Detect orphans
 */
int
gc_detect_orphans(void)
{
	int count = 0;

	/* Detect container orphans */
	count += gc_container_orphans();

	/* Detect volume orphans */
	count += gc_orphaned_volumes();

	/* Detect network orphans */
	count += gc_orphaned_networks();

	pthread_mutex_lock(&gc_lock);
	stats.orphans_detected += count;
	pthread_mutex_unlock(&gc_lock);

	syslog(LOG_INFO, "Detected %d orphaned resources", count);

	return (0);
}

/*
 * List orphans
 */
struct gc_orphan **
gc_list_orphans(int *count)
{
	struct gc_orphan **list = NULL;
	struct gc_orphan *orphan;
	int n = 0;

	if (count == NULL)
		return (NULL);

	pthread_mutex_lock(&gc_lock);

	RB_FOREACH(orphan, orphan_tree, &orphans) {
		if (ocifbsd_realloc_grow((void **)&list, (n + 1) * sizeof(*list)) != 0)
			break;
		list[n++] = orphan;
	}

	pthread_mutex_unlock(&gc_lock);

	*count = n;
	return (list);
}

/*
 * Resolve orphan
 */
int
gc_resolve_orphan(const char *name, int type)
{
	struct gc_orphan key, *orphan;

	pthread_mutex_lock(&gc_lock);

	strlcpy(key.name, name, sizeof(key.name));
	orphan = RB_FIND(orphan_tree, &orphans, &key);

	if (orphan) {
		RB_REMOVE(orphan_tree, &orphans, orphan);
		free(orphan);
		stats.orphans_resolved++;
	}

	pthread_mutex_unlock(&gc_lock);

	return (orphan ? 0 : -1);
}

/*
 * Protect orphan
 */
int
gc_protect_orphan(const char *name, int type)
{
	struct gc_orphan key, *orphan;

	pthread_mutex_lock(&gc_lock);

	strlcpy(key.name, name, sizeof(key.name));
	orphan = RB_FIND(orphan_tree, &orphans, &key);

	if (orphan)
		orphan->is_protected = true;

	pthread_mutex_unlock(&gc_lock);

	return (orphan ? 0 : -1);
}

/*
 * Unprotect orphan
 */
int
gc_unprotect_orphan(const char *name, int type)
{
	return (gc_protect_orphan(name, type));
}

/*
 * Container orphans
 */
int
gc_container_orphans(void)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *dp;
	int count = 0;

	snprintf(path, sizeof(path), "%s/containers", OCIFBSD_STATE_DIR);
	dir = opendir(path);
	if (dir == NULL)
		return (0);

	while ((dp = readdir(dir)) != NULL) {
		if (dp->d_type != DT_DIR)
			continue;
		if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
			continue;

		/* Check if container is orphaned (no owner reference) */
		if (gc_is_container_orphaned(dp->d_name)) {
			gc_queue_item(GC_TYPE_CONTAINER, dp->d_name, NULL, GC_PRIORITY_HIGH);
			count++;
		}
	}

	closedir(dir);

	if (config.verbose)
		syslog(LOG_INFO, "Found %d orphaned containers", count);

	return (count);
}

/*
 * Check if container is orphaned
 */
static int
gc_is_container_orphaned(const char *name)
{
	char path[PATH_MAX];
	FILE *fp;
	char line[256];
	bool has_owner = false;

	snprintf(path, sizeof(path), "%s/containers/%s/state.json", OCIFBSD_STATE_DIR, name);
	fp = fopen(path, "r");
	if (fp == NULL)
		return (0);  /* Container doesn't exist */

	while (fgets(line, sizeof(line), fp) != NULL) {
		if (strstr(line, "\"owner\":")) {
			has_owner = true;
			break;
		}
	}

	fclose(fp);

	return (!has_owner);
}

/*
 * Stopped containers
 */
int
gc_stopped_containers(int ttl_seconds)
{
	char path[PATH_MAX];
	DIR *dir;
	struct dirent *dp;
	int count = 0;
	time_t cutoff = time(NULL) - ttl_seconds;

	snprintf(path, sizeof(path), "%s/containers", OCIFBSD_STATE_DIR);
	dir = opendir(path);
	if (dir == NULL)
		return (0);

	while ((dp = readdir(dir)) != NULL) {
		if (dp->d_type != DT_DIR)
			continue;
		if (strcmp(dp->d_name, ".") == 0 || strcmp(dp->d_name, "..") == 0)
			continue;

		if (gc_is_container_stopped(dp->d_name, cutoff)) {
			gc_queue_item(GC_TYPE_CONTAINER, dp->d_name, NULL, GC_PRIORITY_NORMAL);
			count++;
		}
	}

	closedir(dir);

	if (config.verbose)
		syslog(LOG_INFO, "Queued %d stopped containers for deletion", count);

	return (count);
}

/*
 * Check if container is stopped
 */
static int
gc_is_container_stopped(const char *name, time_t cutoff)
{
	char path[PATH_MAX];
	FILE *fp;
	char line[256];
	time_t stopped_time = 0;

	snprintf(path, sizeof(path), "%s/containers/%s/state.json", OCIFBSD_STATE_DIR, name);
	fp = fopen(path, "r");
	if (fp == NULL)
		return (0);

	while (fgets(line, sizeof(line), fp) != NULL) {
		if (sscanf(line, "\"stopped_time\": %ld", &stopped_time) == 1)
			break;
	}

	fclose(fp);

	return (stopped_time > 0 && stopped_time < cutoff);
}

/*
 * Finished pods
 */
int
gc_finished_pods(int ttl_seconds)
{
	/* Similar to stopped containers but for pods */
	return (gc_stopped_containers(ttl_seconds));
}

/*
 * Unused images
 */
int
gc_unused_images(int ttl_seconds)
{
	char cmd[PATH_MAX];
	FILE *fp;
	char line[256];
	int count = 0;
	time_t cutoff = time(NULL) - ttl_seconds;

	/* List images from ZFS */
	snprintf(cmd, sizeof(cmd),
		"zfs list -t snapshot -H -o name,creation | grep %s/images | grep -v @",
		OCIFBSD_ZFS_POOL);

	fp = popen(cmd, "r");
	if (fp == NULL)
		return (0);

	while (fgets(line, sizeof(line), fp) != NULL) {
		char name[256];
		time_t created;

		if (sscanf(line, "%s %ld", name, &created) == 2) {
			if (created < cutoff) {
				/* Check if image is pinned */
				if (!gc_image_is_pinned(name)) {
					gc_queue_item(GC_TYPE_IMAGE, name, NULL, GC_PRIORITY_LOW);
					count++;
				}
			}
		}
	}

	pclose(fp);

	if (config.verbose)
		syslog(LOG_INFO, "Queued %d unused images for deletion", count);

	return (count);
}

/*
 * Check if image is pinned
 */
static int
gc_image_is_pinned(const char *name)
{
	char path[PATH_MAX];
	FILE *fp;

	snprintf(path, sizeof(path), "%s/images/%s/pinned", OCIFBSD_STATE_DIR, name);
	fp = fopen(path, "r");
	if (fp != NULL) {
		fclose(fp);
		return (1);
	}

	return (0);
}

/*
 * Pin image
 */
int
gc_image_pinned(const char *name)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/images/%s/pinned", OCIFBSD_STATE_DIR, name);
	FILE *fp = fopen(path, "w");
	if (fp != NULL) {
		fprintf(fp, "1\n");
		fclose(fp);
		return (0);
	}

	return (-1);
}

/*
 * Image space management
 */
int
gc_image_space(int high_percent, int low_percent)
{
	struct statfs fs;
	int used_percent;
	(void)used_percent;
	(void)fs;

	if (statfs(OCIFBSD_IMAGE_DIR, &fs) != 0)
		return (-1);

	/* Guard against a zero block count (div-by-zero) and use f_bfree — true
	 * free blocks — rather than f_bavail, which excludes the superuser
	 * reserve and skews utilization. */
	if (fs.f_blocks == 0)
		used_percent = 0;
	else
		used_percent = 100 -
			(int)((fs.f_bfree * 100) / fs.f_blocks);

	if (used_percent >= high_percent) {
		/* Aggressive GC */
		gc_unused_images(0);  /* Delete all unused */
		syslog(LOG_WARNING, "Disk space critical: %d%% used, running aggressive GC",
			used_percent);
	} else if (used_percent >= low_percent) {
		/* Normal GC */
		gc_unused_images(config.image_ttl);
		syslog(LOG_INFO, "Disk space warning: %d%% used, running normal GC",
			used_percent);
	}

	return (0);
}

/*
 * Image by name pattern
 */
int
gc_image_by_name(const char *pattern)
{
	/* Would use fnmatch to match pattern */
	(void)pattern;
	return (0);
}

/*
 * Orphaned volumes
 */
int
gc_orphaned_volumes(void)
{
	char cmd[PATH_MAX];
	FILE *fp;
	char line[1024];
	int count = 0;

	snprintf(cmd, sizeof(cmd),
		"zfs list -t filesystem -H -o name | grep %s/volumes",
		OCIFBSD_ZFS_POOL);

	fp = popen(cmd, "r");
	if (fp == NULL)
		return (0);

	while (fgets(line, sizeof(line), fp) != NULL) {
		char *name = line;
		char *leaf;
		name[strcspn(name, "\n")] = '\0';

		/*
		 * `zfs list` prints the full dataset path (e.g.
		 * zroot/ocifbsd/volumes/myvol). The owner file and the volume
		 * command both key on the leaf name, so extract the final path
		 * component. Using the full path built a nonexistent owner path
		 * and flagged EVERY volume as orphaned — deleting them all.
		 */
		leaf = strrchr(name, '/');
		leaf = (leaf != NULL) ? leaf + 1 : name;
		if (leaf[0] == '\0')
			continue;

		if (gc_is_volume_orphaned(leaf)) {
			gc_queue_item(GC_TYPE_VOLUME, leaf, NULL, GC_PRIORITY_NORMAL);
			count++;
		}
	}

	pclose(fp);

	return (count);
}

/*
 * Check if volume is orphaned
 */
static int
gc_is_volume_orphaned(const char *name)
{
	char path[PATH_MAX];
	FILE *fp;
	char line[256];
	bool has_owner = false;

	snprintf(path, sizeof(path), "%s/volumes/%s/owner", OCIFBSD_STATE_DIR, name);
	fp = fopen(path, "r");
	if (fp == NULL)
		return (1);  /* No owner file = orphaned */

	if (fgets(line, sizeof(line), fp) != NULL) {
		has_owner = (line[0] != '\0' && line[0] != '\n');
	}

	fclose(fp);

	return (!has_owner);
}

/*
 * Released PVC
 */
int
gc_released_pvc(void)
{
	/* PVCs marked for deletion */
	return (gc_orphaned_volumes());
}

/*
 * Unused volumes
 */
int
gc_unused_volumes(int ttl_seconds)
{
	(void)ttl_seconds;
	/* Would check last access time */
	return (0);
}

/*
 * Orphaned networks
 */
int
gc_orphaned_networks(void)
{
	int count = 0;

	count += gc_orphan_bridge();
	count += gc_orphan_epair();
	count += gc_orphan_vxlan();

	return (count);
}

/*
 * Unused networks
 */
int
gc_unused_networks(int ttl_seconds)
{
	(void)ttl_seconds;
	return (0);
}

/*
 * Orphaned bridges
 */
int
gc_orphan_bridge(void)
{
	(void)0;
	FILE *fp;
	char line[256];
	int count = 0;

	fp = popen("ifconfig -l bridge", "r");
	if (fp == NULL)
		return (0);

	while (fgets(line, sizeof(line), fp) != NULL) {
		char *save = NULL;
		char *tok;

		/*
		 * `ifconfig -l bridge` prints all bridge interfaces on one
		 * whitespace-separated line (bridge0 bridge1 ...). Tokenize and
		 * use each full interface name. The old sscanf("bridge%63s")
		 * stripped the "bridge" prefix (capturing just "0") and only
		 * read the first token.
		 */
		for (tok = strtok_r(line, " \t\n", &save); tok != NULL;
			tok = strtok_r(NULL, " \t\n", &save)) {
			if (strncmp(tok, "bridge", 6) != 0)
				continue;
			if (gc_is_bridge_orphaned(tok)) {
				gc_queue_item(GC_TYPE_NETWORK, tok, NULL,
					GC_PRIORITY_NORMAL);
				count++;
			}
		}
	}

	pclose(fp);

	return (count);
}

/*
 * Check if bridge is orphaned.
 *
 * A bridge is only a GC candidate if ocifbsd actually tracks it (a
 * <STATE_DIR>/networks/<name> directory exists) but it has no live owner.
 * If ocifbsd never created a state directory for the interface, it is a
 * system- or operator-managed bridge and must never be destroyed.
 */
static int
gc_is_bridge_orphaned(const char *name)
{
	char path[PATH_MAX];
	struct stat st;
	FILE *fp;
	bool has_owner;
	char line[256];

	snprintf(path, sizeof(path), "%s/networks/%s", OCIFBSD_STATE_DIR, name);
	if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
		return (0);  /* not tracked by ocifbsd — leave it alone */

	snprintf(path, sizeof(path), "%s/networks/%s/owner", OCIFBSD_STATE_DIR,
		name);
	fp = fopen(path, "r");
	if (fp == NULL)
		return (1);  /* tracked but no owner file = orphaned */

	has_owner = (fgets(line, sizeof(line), fp) != NULL &&
		line[0] != '\0' && line[0] != '\n');
	fclose(fp);
	return (!has_owner);
}

/*
 * Orphaned epair
 */
int
gc_orphan_epair(void)
{
	/* Similar to bridge */
	return (0);
}

/*
 * Orphaned VXLAN
 */
int
gc_orphan_vxlan(void)
{
	/* Check for orphaned VXLAN interfaces */
	return (0);
}

/*
 * Node resources
 */
int
gc_node_resources(const char *node)
{
	(void)node;
	return (0);
}

/*
 * Dead node resources
 */
int
gc_dead_node_resources(void)
{
	char cmd[PATH_MAX];
	FILE *fp;
	char line[256];
	int count = 0;

	snprintf(cmd, sizeof(cmd), "%s cluster nodes --format json", OCIFBSD_BIN);
	fp = popen(cmd, "r");
	if (fp == NULL)
		return (0);

	/* Parse node status and clean up dead nodes */
	while (fgets(line, sizeof(line), fp) != NULL) {
		/* Parse JSON and check node status */
	}

	pclose(fp);

	return (count);
}

/*
 * Stale registrations
 */
int
gc_stale_registrations(void)
{
	return (0);
}

/*
 * ZFS cleanup
 */
int
gc_zfs_cleanup(const char *dataset)
{
	(void)dataset;

	if (config.dry_run) {
		syslog(LOG_INFO, "[DRY RUN] Would cleanup ZFS dataset: %s", dataset);
		return (0);
	}

	char *argv[] = { "zfs", "destroy", "-r", (char *)dataset, NULL };
	return (safe_execv("/sbin/zfs", argv) == 0 ? 0 : -1);
}

/*
 * ZFS snapshots
 */
int
gc_zfs_snapshots(int ttl_seconds)
{
	char cmd[PATH_MAX];
	time_t cutoff = time(NULL) - ttl_seconds;

	if (config.dry_run) {
		syslog(LOG_INFO, "[DRY RUN] Would delete ZFS snapshots older than %d seconds",
			ttl_seconds);
		return (0);
	}

	snprintf(cmd, sizeof(cmd),
		"zfs list -t snapshot -H -o name,creation | "
		"awk '$2 < %ld { print $1 }' | xargs -r zfs destroy",
		(long)cutoff);

	return (system(cmd) == 0 ? 0 : -1);
}

/*
 * ZFS destroy dataset
 */
int
gc_zfs_destroy_dataset(const char *dataset)
{
	if (config.dry_run) {
		syslog(LOG_INFO, "[DRY RUN] Would destroy ZFS dataset: %s", dataset);
		return (0);
	}

	char *argv[] = { "zfs", "destroy", "-r", (char *)dataset, NULL };
	return (safe_execv("/sbin/zfs", argv) == 0 ? 0 : -1);
}

/*
 * PF anchors cleanup
 */
int
gc_pf_anchors(const char *anchor)
{
	const char *a = anchor ? anchor : "ocifbsd";
	char *argv[] = { "pfctl", "-a", (char *)a, "-F", "all", NULL };
	return (safe_execv("/sbin/pfctl", argv) == 0 ? 0 : -1);
}

/*
 * PF rules cleanup
 */
int
gc_pf_rules(const char *table)
{
	const char *t = table ? table : "ocifbsdcontainers";
	char *argv[] = { "pfctl", "-t", (char *)t, "-T", "flush", NULL };
	return (safe_execv("/sbin/pfctl", argv) == 0 ? 0 : -1);
}

/*
 * Get statistics
 */
int
gc_stats_get(struct gc_stats *out)
{
	if (out == NULL)
		return (-1);

	pthread_mutex_lock(&gc_lock);
	*out = stats;
	pthread_mutex_unlock(&gc_lock);

	return (0);
}

/*
 * Statistics as JSON
 */
int
gc_stats_json(char **json_out)
{
	struct gc_stats s;
	char *json;

	if (json_out == NULL)
		return (-1);

	gc_stats_get(&s);

	if (asprintf(&json,
		"{\"items_scanned\":%lu,\"items_collected\":%lu,"
		"\"items_failed\":%lu,\"space_reclaimed\":%lu,"
		"\"orphans_detected\":%lu,\"orphans_resolved\":%lu,"
		"\"last_run\":%ld,\"last_full_gc\":%ld}",
		(unsigned long)s.items_scanned,
		(unsigned long)s.items_collected,
		(unsigned long)s.items_failed,
		(unsigned long)s.space_reclaimed,
		(unsigned long)s.orphans_detected,
		(unsigned long)s.orphans_resolved,
		(long)s.last_run,
		(long)s.last_full_gc) == -1) {
		return (-1);
	}

	*json_out = json;
	return (0);
}

/*
 * Reset statistics
 */
int
gc_stats_reset(void)
{
	pthread_mutex_lock(&gc_lock);
	memset(&stats, 0, sizeof(stats));
	pthread_mutex_unlock(&gc_lock);

	return (0);
}

/*
 * Log GC start
 */
int
gc_log_start(int type)
{
	const char *type_names[] = {
		"container", "image", "volume", "network", "node", "all"
	};

	if (type >= 0 && type <= GC_TYPE_ALL)
		syslog(LOG_INFO, "Starting GC: type=%s", type_names[type]);

	return (0);
}

/*
 * Log GC finish
 */
int
gc_log_finish(int type, int collected, int failed)
{
	pthread_mutex_lock(&gc_lock);
	stats.items_scanned += collected + failed;
	if (failed == 0)
		stats.last_run = time(NULL);
	pthread_mutex_unlock(&gc_lock);

	if (collected > 0 || failed > 0)
		syslog(LOG_INFO, "GC finished: collected=%d failed=%d", collected, failed);

	return (0);
}

/*
 * Log GC item
 */
int
gc_log_item(int type, const char *name, const char *action)
{
	if (config.verbose)
		syslog(LOG_INFO, "GC item: type=%d name=%s action=%s", type, name, action);
	return (0);
}

/*
 * Orphan tree comparison
 */
static int
orphan_compare(struct gc_orphan *a, struct gc_orphan *b)
{
	int cmp = strcmp(a->name, b->name);
	if (cmp != 0)
		return (cmp);
	return (a->type - b->type);
}
RB_GENERATE(orphan_tree, gc_orphan, entry, orphan_compare);

/*
 * Main
 */
int
main(int argc, char *argv[])
{
	int foreground = 0;
	int ch;
	int ret;

	/* Parse arguments */
	while ((ch = getopt(argc, argv, "fvh")) != -1) {
		switch (ch) {
		case 'f':
			foreground = 1;
			break;
		case 'v':
			config.verbose = true;
			break;
		case 'h':
			printf("Usage: %s [-fvh]\n", argv[0]);
			printf("  -f  Run in foreground\n");
			printf("  -v  Verbose output\n");
			printf("  -h  Show this help\n");
			return (0);
		}
	}

	/* Daemonize unless foreground */
	if (!foreground)
		daemon(0, 0);

	/* Initialize */
	ret = gc_init(NULL);
	if (ret != 0) {
		fprintf(stderr, "Failed to initialize GC\n");
		return (1);
	}

	/* Handle signals */
	signal(SIGTERM, sig_handler);
	signal(SIGINT, sig_handler);
	signal(SIGHUP, sig_handler);

	/* Run event loop */
	while (running) {
		sleep(1);
	}

	gc_shutdown();
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
	} else {
		running = 0;
	}
}
