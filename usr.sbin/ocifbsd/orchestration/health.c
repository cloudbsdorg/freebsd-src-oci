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
 * Health checker implementation - liveness and readiness probes
 */

#include <sys/param.h>
#include <sys/jail.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include <sys/wait.h>

#include "orchestration.h"
#include "loadbalancer.h"	/* service_lb_apply: re-drive pf on health change */

#define MAX_HEALTH_CHECKS 1024

/*
 * A probe that could not be attempted at all. Distinct from success (0) and
 * from failure (-1): it must not move the success or failure counters, because
 * "we could not look" is not evidence that a replica is unhealthy.
 */
#define	HEALTH_PROBE_SKIPPED	(-2)

/*
 * Run a health-check command and return its exit status (0 = healthy).
 *
 * EXEC probes are run via fork/exec of a tokenized argument vector — no
 * shell — matching exec-probe semantics (a command + args, not
 * a shell string) and removing the shell-injection surface that system()
 * exposed. SHELL probes still use /bin/sh -c because a shell is their
 * explicit purpose.
 *
 * NOTE: these run in the host context. Running probes inside the target
 * container (via jexec into its jail) requires the jail id to be threaded
 * through here and is left as a follow-up; callers must not pass
 * attacker-controlled SHELL probe strings until then.
 */
static int
run_health_command(const char *command, bool shell, int jid)
{
	pid_t pid;
	int status;

	if (command == NULL || command[0] == '\0')
		return (-1);

	/*
	 * Health-check commands come verbatim from the (untrusted) manifest and
	 * this runs as root. Two hard rules close the arbitrary-root-exec sink:
	 *   - SHELL probes are refused outright: a manifest-supplied shell string
	 *     run through /bin/sh -c as root is a command-injection vector with no
	 *     safe form. Use an EXEC probe with an absolute command instead.
	 *   - EXEC probes run ONLY inside the target container's jail, never in
	 *     the host context. Without a jail id to confine to, fail the probe
	 *     closed rather than exec a manifest command as root on the host.
	 */
	if (shell) {
		fprintf(stderr, "health: refusing SHELL probe (arbitrary root "
		    "exec); use an EXEC probe with an absolute command\n");
		return (-1);
	}
	if (jid <= 0) {
		fprintf(stderr, "health: refusing EXEC probe with no jail to "
		    "confine it to (would run as root on the host)\n");
		return (-1);
	}

	pid = fork();
	if (pid < 0)
		return (-1);
	if (pid == 0) {
		char *buf, *argv[64], *save = NULL, *tok;
		int argc = 0;

		/* Enter the container first; everything below runs confined. */
		if (jail_attach(jid) != 0)
			_exit(127);
		buf = strdup(command);
		if (buf == NULL)
			_exit(127);
		for (tok = strtok_r(buf, " \t", &save);
		    tok != NULL && argc < 63;
		    tok = strtok_r(NULL, " \t", &save))
			argv[argc++] = tok;
		argv[argc] = NULL;
		/*
		 * Absolute path only, and execv (not execvp): never resolve the
		 * program through $PATH, which an attacker could steer.
		 */
		if (argc == 0 || argv[0][0] != '/')
			_exit(127);
		execv(argv[0], argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return (-1);
	return (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

/*
 * Health check state for a replica
 */
struct health_check_state {
	char			replica_name[256];
	char			service_name[256];
	char			namespace[128];
	health_check_type_t	type;
	int			jid;	/* jail to confine EXEC probes to; 0 = none */
	int			period;			/* seconds between checks */
	int			timeout;		/* seconds per probe */
	int			success_threshold;	/* successes to become healthy */
	int			failure_threshold;	/* failures to become unhealthy */
	time_t			next_check;
	int			consecutive_failures;
	int			consecutive_successes;
	bool			healthy;
	bool			active;
	/*
	 * Scheduling bookkeeping. heap_idx is this entry's position in the
	 * timer heap, or -1 when it is not scheduled (either in flight or
	 * stopped). in_flight marks a probe running outside every lock;
	 * doomed asks the worker to free the entry when that probe returns,
	 * so a stop during a probe never frees memory out from under it.
	 */
	int			heap_idx;
	bool			in_flight;
	bool			doomed;
	pthread_mutex_t		lock;

	/* Check-specific data */
	union {
		struct {
			char		host[256];
			int		port;
		} tcp;
		struct {
			char		url[512];
			int		status_code;
		} http;
		struct {
			char		command[1024];
		} exec;
	} check_data;
};

/*
 * Global state
 */
/*
 * Scheduling.
 *
 * Previously each registered check ran on its own thread that woke once a
 * second, compared the clock against its own deadline, and went back to
 * sleep. A service with 200 replicas therefore held 200 OS threads -- each
 * with a full stack -- and produced 200 wakeups per second to do nothing.
 * Teardown was worse: health_check_stop() cleared one entry's `active` flag
 * and immediately joined that thread before moving to the next, so stopping N
 * checks cost about N seconds, serialized, with the global lock held
 * throughout. Measured on this tree: 200 replicas left 149 threads parked in
 * nanosleep while the caller sat in pthread_join, and teardown had not
 * finished after 45 seconds.
 *
 * The replacement is one worker thread driving a binary min-heap keyed by
 * each check's next-due time. The worker sleeps on a condition variable until
 * the EARLIEST deadline, so an idle system is genuinely idle, and a
 * registration wakes it immediately rather than being noticed up to a second
 * later. Cost per check is O(log N) to schedule and reschedule; cost in
 * threads is O(1) regardless of how many replicas exist.
 *
 * health_checks[] remains the registry (ownership and lookup); the heap holds
 * borrowed pointers to the subset that is currently scheduled.
 */
static struct health_check_state *health_checks[MAX_HEALTH_CHECKS];
static int health_check_count = 0;
static pthread_mutex_t health_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t health_cond = PTHREAD_COND_INITIALIZER;
static bool health_checker_running = false;
static bool health_worker_started = false;
static pthread_t health_worker;

/* Timer heap: heap[0] is always the next check due. */
static struct health_check_state *heap[MAX_HEALTH_CHECKS];
static int heap_n = 0;

static void
heap_swap(int a, int b)
{
	struct health_check_state *t = heap[a];

	heap[a] = heap[b];
	heap[b] = t;
	heap[a]->heap_idx = a;
	heap[b]->heap_idx = b;
}

static void
heap_sift_up(int i)
{

	while (i > 0) {
		int parent = (i - 1) / 2;

		if (heap[parent]->next_check <= heap[i]->next_check)
			break;
		heap_swap(parent, i);
		i = parent;
	}
}

static void
heap_sift_down(int i)
{

	for (;;) {
		int l = 2 * i + 1, r = 2 * i + 2, smallest = i;

		if (l < heap_n &&
		    heap[l]->next_check < heap[smallest]->next_check)
			smallest = l;
		if (r < heap_n &&
		    heap[r]->next_check < heap[smallest]->next_check)
			smallest = r;
		if (smallest == i)
			break;
		heap_swap(i, smallest);
		i = smallest;
	}
}

/* Caller holds health_lock. Returns 0, or -1 if the heap is full. */
static int
heap_push(struct health_check_state *st)
{

	if (heap_n >= MAX_HEALTH_CHECKS)
		return (-1);
	heap[heap_n] = st;
	st->heap_idx = heap_n;
	heap_n++;
	heap_sift_up(heap_n - 1);
	return (0);
}

/* Caller holds health_lock. Removes an entry wherever it sits. */
static void
heap_remove(struct health_check_state *st)
{
	int i = st->heap_idx;

	if (i < 0 || i >= heap_n || heap[i] != st)
		return;
	st->heap_idx = -1;
	heap_n--;
	if (i != heap_n) {
		heap[i] = heap[heap_n];
		heap[i]->heap_idx = i;
		heap_sift_down(i);
		heap_sift_up(i);
	}
	heap[heap_n] = NULL;
}

/* Caller holds health_lock. */
static struct health_check_state *
heap_pop_min(void)
{
	struct health_check_state *st;

	if (heap_n == 0)
		return (NULL);
	st = heap[0];
	heap_remove(st);
	return (st);
}

/* Caller holds health_lock. Drop the entry from the registry and free it. */
static void
health_state_release(struct health_check_state *st)
{
	int i;

	for (i = 0; i < MAX_HEALTH_CHECKS; i++) {
		if (health_checks[i] == st) {
			health_checks[i] = NULL;
			/*
			 * Decrement the live count. The old code nulled the
			 * slot but left the count raised, so the table filled
			 * monotonically and start eventually failed ENOSPC
			 * forever even with every slot free.
			 */
			if (health_check_count > 0)
				health_check_count--;
			break;
		}
	}
	pthread_mutex_destroy(&st->lock);
	free(st);
}

/*
 * TCP health check
 */
static int
health_check_tcp(const char *host, int port)
{
	int sock;
	struct sockaddr_in addr;
	struct timeval tv;

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return (-1);

	tv.tv_sec = 5;
	tv.tv_usec = 0;
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);

	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		/*
		 * Resolve with getaddrinfo, not gethostbyname: the latter
		 * returns a pointer into a single shared static hostent, so
		 * two probes resolving at once (the worker and an on-demand
		 * health_check_run) could read an address another thread was
		 * simultaneously replacing -- connecting to the wrong host, or
		 * copying from freed storage.
		 */
		struct addrinfo hints, *res = NULL;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_INET;
		hints.ai_socktype = SOCK_STREAM;
		if (getaddrinfo(host, NULL, &hints, &res) != 0 || res == NULL) {
			if (res != NULL)
				freeaddrinfo(res);
			close(sock);
			return (-1);
		}
		addr.sin_addr =
		    ((struct sockaddr_in *)(void *)res->ai_addr)->sin_addr;
		freeaddrinfo(res);
	}

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(sock);
		return (-1);
	}

	close(sock);
	return (0);
}

/*
 * HTTP health check
 */
static int
health_check_http(const char *url, int *status_code, int timeout_s)
{
	char host[256];
	char path[512];
	int port = 80;
	char *p;
	int sock;
	struct sockaddr_in addr;
	char response[1024];
	FILE *fp;

	int code = 0;
	struct timeval tv;

	/* Parse URL */
	strlcpy(host, url + 7, sizeof(host));  /* skip http:// */
	p = strchr(host, '/');
	if (p != NULL) {
		strlcpy(path, p, sizeof(path));
		*p = '\0';
	} else {
		strlcpy(path, "/", sizeof(path));
	}

	p = strchr(host, ':');
	if (p != NULL) {
		*p = '\0';
		port = atoi(p + 1);
	}

	/*
	 * The path is written verbatim into the request line, so a CR or LF
	 * in it ends that line and smuggles a second request -- and the path
	 * comes from the manifest. Require a printable absolute path and a
	 * printable host, and refuse anything else rather than emitting it.
	 */
	if (path[0] != '/')
		return (-1);
	for (p = path; *p != '\0'; p++)
		if ((unsigned char)*p <= 0x20 || (unsigned char)*p == 0x7f)
			return (-1);
	for (p = host; *p != '\0'; p++)
		if ((unsigned char)*p <= 0x20 || (unsigned char)*p == 0x7f)
			return (-1);

	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return (-1);

	/*
	 * Bound every blocking operation. With ONE worker serving every
	 * check, an HTTP probe against a host that blackholes SYNs -- or
	 * accepts and never answers -- would otherwise stall every other
	 * replica's check indefinitely. The TCP probe already set timeouts;
	 * the HTTP probe set none and ignored the spec's timeout entirely.
	 */
	tv.tv_sec = (timeout_s > 0) ? timeout_s : 5;
	tv.tv_usec = 0;
	(void)setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	(void)setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	/*
	 * An unchecked inet_pton leaves sin_addr zeroed, so an empty or
	 * non-numeric host silently probed 0.0.0.0 -- and called the replica
	 * healthy if anything happened to be listening there.
	 */
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		close(sock);
		return (-1);
	}

	if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		close(sock);
		return (-1);
	}

	fp = fdopen(sock, "r+");
	if (fp == NULL) {
		close(sock);
		return (-1);
	}

	fprintf(fp, "GET %s HTTP/1.1\r\n", path);
	fprintf(fp, "Host: %s\r\n", host);
	fprintf(fp, "Connection: close\r\n");
	fprintf(fp, "\r\n");
	fflush(fp);

	/*
	 * Judge THIS response only. The status code was previously read from
	 * -- and left in -- the check's persistent state, so a probe whose
	 * response could not be read at all still saw the previous probe's
	 * 200 and counted as a success, keeping a dead backend in the load
	 * balancer forever.
	 */
	if (fgets(response, sizeof(response), fp) != NULL) {
		char *sp = strchr(response, ' ');

		if (sp != NULL)
			code = atoi(sp + 1);
	}

	/*
	 * fclose() closes the underlying descriptor. The extra close(sock)
	 * that followed therefore closed whatever unrelated descriptor had
	 * since been handed that number -- another probe's socket -- as root.
	 */
	fclose(fp);

	if (status_code != NULL)
		*status_code = code;
	return (code >= 200 && code < 400 ? 0 : -1);
}

/*
 * Execute health check
 */
static int
execute_health_check(struct health_check_state *state)
{
	switch (state->type) {
	case HEALTH_CHECK_TCP:
		return health_check_tcp(state->check_data.tcp.host,
		    state->check_data.tcp.port);

	case HEALTH_CHECK_HTTP:
		return health_check_http(state->check_data.http.url,
		    &state->check_data.http.status_code, state->timeout);

	case HEALTH_CHECK_EXEC:
		/*
		 * run_health_command fails closed without a jail, and nothing
		 * yet resolves a replica's jid, so every EXEC probe returned
		 * failure -- which after failure_threshold marked a perfectly
		 * healthy replica FAILED and pulled it out of the load
		 * balancer. Until the jid is wired up, report "no result"
		 * rather than manufacturing a failure. (Task #86.)
		 */
		if (state->jid <= 0)
			return (HEALTH_PROBE_SKIPPED);
		return run_health_command(state->check_data.exec.command,
		    false, state->jid);
	case HEALTH_CHECK_SHELL:
		if (state->jid <= 0)
			return (HEALTH_PROBE_SKIPPED);
		return run_health_command(state->check_data.exec.command,
		    true, state->jid);

	default:
		return (-1);
	}
}

/*
 * Process health check result
 */
static void
process_health_result(struct health_check_state *state, int result)
{
	struct service *svc;

	pthread_mutex_lock(&state->lock);

	if (result == 0) {
		state->consecutive_successes++;
		state->consecutive_failures = 0;

		if (!state->healthy &&
		    state->consecutive_successes >= state->success_threshold) {
			state->healthy = true;

			orch_event_publish("Normal", "HealthCheckPassed",
			    state->namespace,
			    "Health check passed for %s", state->replica_name);

			/*
			 * Reflect recovery in replica state so the load balancer
			 * (which selects REPLICA_STATE_RUNNING backends) starts
			 * routing to it again.
			 */
			svc = service_get(state->service_name, state->namespace);
			if (svc != NULL) {
				for (int i = 0; i < svc->nreplicas; i++) {
					if (strcmp(svc->replicas[i].name,
					    state->replica_name) == 0) {
						svc->replicas[i].state =
						    REPLICA_STATE_RUNNING;
						break;
					}
				}
				(void)service_lb_apply(svc);
			}
		}
	} else {
		state->consecutive_failures++;
		state->consecutive_successes = 0;

		if (state->healthy &&
		    state->consecutive_failures >= state->failure_threshold) {
			state->healthy = false;

			orch_event_publish("Warning", "HealthCheckFailed",
			    state->namespace,
			    "Health check failed for %s (%d failures)",
			    state->replica_name, state->consecutive_failures);

			/*
			 * Mark the replica FAILED and re-apply the load balancer
			 * so pf stops round-robining to a dead backend — the
			 * whole point of a health check.
			 */
			svc = service_get(state->service_name, state->namespace);
			if (svc != NULL) {
				for (int i = 0; i < svc->nreplicas; i++) {
					if (strcmp(svc->replicas[i].name,
					    state->replica_name) == 0) {
						svc->replicas[i].state =
						    REPLICA_STATE_FAILED;
						orch_event_publish("Warning",
						    "ReplicaUnhealthy",
						    state->namespace,
						    "Replica %s is unhealthy",
						    state->replica_name);
						break;
					}
				}
				(void)service_lb_apply(svc);
			}
		}
	}

	pthread_mutex_unlock(&state->lock);
}

/*
 * The single scheduler worker.
 *
 * Sleeps on health_cond until the earliest deadline in the heap (or
 * indefinitely when the heap is empty), then runs the due probe. The probe --
 * which can block for seconds on a TCP connect or an exec -- runs with NO lock
 * held, so a slow or hung backend cannot stall registration, teardown, or a
 * status query. The entry is off the heap while its probe is in flight and is
 * re-armed afterwards, which is also what makes a check unable to overlap
 * itself.
 */
static void *
health_worker_func(void *arg __unused)
{

	pthread_mutex_lock(&health_lock);
	while (health_checker_running) {
		struct health_check_state *st;
		struct timespec ts;
		time_t now;
		int result;

		if (heap_n == 0) {
			/* Nothing scheduled: wait to be woken by a register. */
			pthread_cond_wait(&health_cond, &health_lock);
			continue;
		}
		now = time(NULL);
		if (heap[0]->next_check > now) {
			ts.tv_sec = heap[0]->next_check;
			ts.tv_nsec = 0;
			pthread_cond_timedwait(&health_cond, &health_lock, &ts);
			continue;
		}

		st = heap_pop_min();
		if (st == NULL)
			continue;
		st->in_flight = true;
		pthread_mutex_unlock(&health_lock);

		result = execute_health_check(st);
		if (result != HEALTH_PROBE_SKIPPED)
			process_health_result(st, result);

		pthread_mutex_lock(&health_lock);
		st->in_flight = false;
		if (st->doomed || !st->active || !health_checker_running) {
			/*
			 * Stopped while the probe was running. The stopper
			 * deliberately left the entry alive rather than
			 * freeing memory this thread was still using, so it
			 * is ours to release now.
			 */
			health_state_release(st);
			continue;
		}
		st->next_check = time(NULL) + st->period;
		(void)heap_push(st);
	}
	pthread_mutex_unlock(&health_lock);
	return (NULL);
}

/*
 * Initialize health checker
 */
int
health_checker_init(void)
{
	pthread_mutex_lock(&health_lock);

	if (health_checker_running) {
		pthread_mutex_unlock(&health_lock);
		return (0);
	}

	health_checker_running = true;
	if (pthread_create(&health_worker, NULL, health_worker_func,
	    NULL) != 0) {
		/*
		 * Report the failure instead of swallowing it. The old code
		 * ignored every pthread_create return, so a thread that was
		 * never created still had pthread_join called on it later.
		 */
		health_checker_running = false;
		pthread_mutex_unlock(&health_lock);
		return (-1);
	}
	health_worker_started = true;
	pthread_mutex_unlock(&health_lock);

	return (0);
}

/*
 * Shutdown health checker
 */
void
health_checker_shutdown(void)
{
	pthread_t worker;
	bool started;

	pthread_mutex_lock(&health_lock);
	health_checker_running = false;
	worker = health_worker;
	started = health_worker_started;
	health_worker_started = false;
	/* Wake the worker so it observes the shutdown and returns. */
	pthread_cond_broadcast(&health_cond);
	pthread_mutex_unlock(&health_lock);

	/*
	 * Join OUTSIDE the lock -- the worker needs it to finish -- and join
	 * exactly one thread rather than one per check.
	 */
	if (started)
		pthread_join(worker, NULL);

	pthread_mutex_lock(&health_lock);
	for (int i = 0; i < MAX_HEALTH_CHECKS; i++) {
		if (health_checks[i] != NULL) {
			heap_remove(health_checks[i]);
			pthread_mutex_destroy(&health_checks[i]->lock);
			free(health_checks[i]);
			health_checks[i] = NULL;
		}
	}
	health_check_count = 0;
	heap_n = 0;
	pthread_mutex_unlock(&health_lock);
}

/*
 * Start health checking for a service
 */
int
health_check_start(struct service *service)
{
	struct health_check_state *state;
	struct health_check *hc;

	if (service == NULL || service->spec == NULL)
		return (-1);

	hc = &service->spec->health_check;
	if (hc->type == HEALTH_CHECK_NONE)
		return (0);

	pthread_mutex_lock(&health_lock);

	for (int i = 0; i < service->nreplicas; i++) {
		int slot;

		/*
		 * Take the first FREE slot rather than appending at a
		 * monotonically rising index: stop() releases slots, and the
		 * old append-only scheme could not reuse them.
		 */
		slot = -1;
		for (int k = 0; k < MAX_HEALTH_CHECKS; k++) {
			if (health_checks[k] == NULL) {
				slot = k;
				break;
			}
		}
		if (slot < 0) {
			pthread_mutex_unlock(&health_lock);
			errno = ENOSPC;
			return (-1);
		}
		state = calloc(1, sizeof(struct health_check_state));
		if (state == NULL) {
			pthread_mutex_unlock(&health_lock);
			return (-1);
		}

		strlcpy(state->replica_name, service->replicas[i].name,
		    sizeof(state->replica_name));
		strlcpy(state->service_name, service->name,
		    sizeof(state->service_name));
		strlcpy(state->namespace, service->namespace,
		    sizeof(state->namespace));
		state->type = hc->type;
		state->healthy = true;
		state->active = true;
		/* Honor the spec's tunables, with sane defaults when unset (0). */
		state->period = hc->period > 0 ? hc->period : 10;
		state->timeout = hc->timeout > 0 ? hc->timeout : 5;
		state->success_threshold =
		    hc->success_threshold > 0 ? hc->success_threshold : 1;
		state->failure_threshold =
		    hc->failure_threshold > 0 ? hc->failure_threshold : 3;
		state->next_check = time(NULL) + hc->initial_delay;
		pthread_mutex_init(&state->lock, NULL);

		/* Set up check-specific data */
		switch (hc->type) {
		case HEALTH_CHECK_TCP:
			snprintf(state->check_data.tcp.host, sizeof(state->check_data.tcp.host),
			    "%s", service->replicas[i].pod_ip);
			state->check_data.tcp.port = hc->port;
			break;
		case HEALTH_CHECK_HTTP:
			snprintf(state->check_data.http.url, sizeof(state->check_data.http.url),
			    "http://%s:%d%s",
			    service->replicas[i].pod_ip,
			    hc->port, hc->path);
			break;
		case HEALTH_CHECK_EXEC:
			strlcpy(state->check_data.exec.command, hc->command,
			    sizeof(state->check_data.exec.command));
			break;
		default:
			break;
		}

		state->heap_idx = -1;
		health_checks[slot] = state;
		health_check_count++;
		if (heap_push(state) != 0) {
			health_checks[slot] = NULL;
			health_check_count--;
			pthread_mutex_destroy(&state->lock);
			free(state);
			pthread_mutex_unlock(&health_lock);
			errno = ENOSPC;
			return (-1);
		}
	}

	/*
	 * One wake for the whole batch: the worker re-reads the earliest
	 * deadline, which these registrations may have moved earlier.
	 */
	pthread_cond_broadcast(&health_cond);
	pthread_mutex_unlock(&health_lock);

	return (0);
}

/*
 * Stop health checking for a service
 */
int
health_check_stop(struct service *service)
{
	pthread_mutex_lock(&health_lock);

	/*
	 * Unregistering is now bookkeeping, not thread teardown: unschedule
	 * and free. There is nothing to join, so stopping N checks is O(N)
	 * pointer work instead of roughly N seconds of serialized joins.
	 *
	 * The one entry that cannot be freed here is one whose probe is in
	 * flight on the worker; mark it doomed and let the worker release it
	 * when the probe returns, rather than freeing memory it is using.
	 */
	for (int i = 0; i < MAX_HEALTH_CHECKS; i++) {
		struct health_check_state *st = health_checks[i];

		if (st == NULL ||
		    strcmp(st->service_name, service->name) != 0 ||
		    strcmp(st->namespace, service->namespace) != 0)
			continue;
		st->active = false;
		heap_remove(st);
		if (st->in_flight) {
			st->doomed = true;
			continue;
		}
		health_state_release(st);
	}

	pthread_cond_broadcast(&health_cond);
	pthread_mutex_unlock(&health_lock);

	return (0);
}

/*
 * Run a one-time health check
 */
int
health_check_run(struct service *service, const char *replica_name)
{
	struct health_check_state *state = NULL;
	int result;

	pthread_mutex_lock(&health_lock);

	for (int i = 0; i < MAX_HEALTH_CHECKS; i++) {
		if (health_checks[i] != NULL &&
		    strcmp(health_checks[i]->replica_name, replica_name) == 0) {
			state = health_checks[i];
			break;
		}
	}
	if (state != NULL) {
		/*
		 * Claim the entry under the SAME in-flight protocol the worker
		 * uses before dropping the lock. Without this, an on-demand
		 * run held a bare pointer while health_check_stop() -- seeing
		 * in_flight false -- freed it, and the probe then ran on freed
		 * memory and locked a destroyed mutex.
		 *
		 * An entry already being probed, or one being torn down, is
		 * not runnable now; fall through to the inline path rather
		 * than queue behind it.
		 */
		if (state->in_flight || state->doomed || !state->active) {
			state = NULL;
		} else {
			heap_remove(state);
			state->in_flight = true;
		}
	}

	pthread_mutex_unlock(&health_lock);

	if (state == NULL) {
		/* No active health check - run inline */
		struct health_check *hc = &service->spec->health_check;
		if (hc->type == HEALTH_CHECK_TCP) {
			char host[256];
			snprintf(host, sizeof(host), "localhost");
			result = health_check_tcp(host, hc->port);
		} else if (hc->type == HEALTH_CHECK_HTTP) {
			char url[512];
			snprintf(url, sizeof(url), "http://localhost:%d%s",
			    hc->port, hc->path);
			result = health_check_http(url, &(int){0},
			    hc->timeout);
		} else if (hc->type == HEALTH_CHECK_EXEC) {
			/* No jail context here -> fail closed (no host exec). */
			result = run_health_command(hc->command, false, 0);
		} else {
			result = 0;  /* No check defined */
		}
		return (result == 0 ? 0 : -1);
	}

	result = execute_health_check(state);
	if (result != HEALTH_PROBE_SKIPPED)
		process_health_result(state, result);

	/* Release the claim, honouring a stop that arrived while probing. */
	pthread_mutex_lock(&health_lock);
	state->in_flight = false;
	if (state->doomed || !state->active || !health_checker_running) {
		health_state_release(state);
	} else {
		state->next_check = time(NULL) + state->period;
		(void)heap_push(state);
		pthread_cond_broadcast(&health_cond);
	}
	pthread_mutex_unlock(&health_lock);

	return (result == 0 ? 0 : -1);
}

/*
 * Get health check status
 */
int
health_check_get_status(struct service *service, const char *replica_name)
{
	pthread_mutex_lock(&health_lock);

	for (int i = 0; i < MAX_HEALTH_CHECKS; i++) {
		if (health_checks[i] != NULL &&
		    strcmp(health_checks[i]->replica_name, replica_name) == 0) {
			struct health_check_state *st = health_checks[i];
			int status;

			/*
			 * ->healthy is written by the worker under the entry's
			 * OWN lock, so reading it under health_lock alone
			 * excludes nothing -- two different mutexes are not
			 * mutual exclusion, and ThreadSanitizer flags exactly
			 * this. Take the entry lock for the read.
			 *
			 * Lock order is health_lock -> state->lock, and
			 * nothing acquires them the other way round, so this
			 * cannot invert: the worker holds state->lock only
			 * after releasing health_lock.
			 */
			pthread_mutex_lock(&st->lock);
			status = st->healthy ? 0 : -1;
			pthread_mutex_unlock(&st->lock);
			pthread_mutex_unlock(&health_lock);
			return (status);
		}
	}

	pthread_mutex_unlock(&health_lock);

	/* No health check running - assume healthy */
	return (0);
}
