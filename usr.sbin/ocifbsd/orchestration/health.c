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
 * Health checker implementation - liveness and readiness probes
 */

#include <sys/param.h>
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

#define MAX_HEALTH_CHECKS 1024

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
run_health_command(const char *command, bool shell)
{
	pid_t pid;
	int status;

	if (command == NULL || command[0] == '\0')
		return (-1);

	pid = fork();
	if (pid < 0)
		return (-1);
	if (pid == 0) {
		if (shell) {
			execl("/bin/sh", "sh", "-c", command, (char *)NULL);
		} else {
			/* Tokenize on whitespace into argv (exec-probe form). */
			char *buf = strdup(command);
			char *argv[64];
			int argc = 0;
			char *save = NULL, *tok;

			if (buf == NULL)
				_exit(127);
			for (tok = strtok_r(buf, " \t", &save);
			    tok != NULL && argc < 63;
			    tok = strtok_r(NULL, " \t", &save))
				argv[argc++] = tok;
			argv[argc] = NULL;
			if (argc > 0)
				execvp(argv[0], argv);
		}
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
	time_t			next_check;
	int			consecutive_failures;
	int			consecutive_successes;
	bool			healthy;
	bool			active;
	pthread_t		thread;
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
static struct health_check_state *health_checks[MAX_HEALTH_CHECKS];
static int health_check_count = 0;
static pthread_mutex_t health_lock = PTHREAD_MUTEX_INITIALIZER;
static bool health_checker_running = false;

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
	
	if (inet_pton(AF_INET, host, &addr.sin_addr) <= 0) {
		/* Try DNS resolution */
		struct hostent *he = gethostbyname(host);
		if (he == NULL) {
			close(sock);
			return (-1);
		}
		memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);
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
health_check_http(const char *url, int *status_code)
{
	char host[256];
	char path[512];
	int port = 80;
	char *p;
	int sock;
	struct sockaddr_in addr;
	char response[1024];
	FILE *fp;
	
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
	
	sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0)
		return (-1);
	
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	inet_pton(AF_INET, host, &addr.sin_addr);
	
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
	
	/* Read response */
	if (fgets(response, sizeof(response), fp) != NULL) {
		/* Parse status code */
		char *sp = strchr(response, ' ');
		if (sp != NULL) {
			*status_code = atoi(sp + 1);
		}
	}
	
	fclose(fp);
	close(sock);
	
	return (*status_code >= 200 && *status_code < 400 ? 0 : -1);
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
		    &state->check_data.http.status_code);
	
	case HEALTH_CHECK_EXEC:
		return run_health_command(state->check_data.exec.command,
		    false);
	case HEALTH_CHECK_SHELL:
		return run_health_command(state->check_data.exec.command,
		    true);
	
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
		    state->consecutive_successes >= 1) {
			state->healthy = true;
			
			orch_event_publish("Normal", "HealthCheckPassed",
			    state->namespace,
			    "Health check passed for %s", state->replica_name);
		}
	} else {
		state->consecutive_failures++;
		state->consecutive_successes = 0;
		
		if (state->healthy && 
		    state->consecutive_failures >= 3) {
			state->healthy = false;
			
			orch_event_publish("Warning", "HealthCheckFailed",
			    state->namespace,
			    "Health check failed for %s (%d failures)",
			    state->replica_name, state->consecutive_failures);
			
			/* Get service and mark replica unhealthy */
			svc = service_get(state->service_name, state->namespace);
			if (svc != NULL) {
				for (int i = 0; i < svc->nreplicas; i++) {
					if (strcmp(svc->replicas[i].name,
					    state->replica_name) == 0) {
						/* Trigger failure handling */
						orch_event_publish("Warning",
						    "ReplicaUnhealthy",
						    state->namespace,
						    "Replica %s is unhealthy",
						    state->replica_name);
						break;
					}
				}
			}
		}
	}
	
	pthread_mutex_unlock(&state->lock);
}

/*
 * Health check thread
 */
static void *
health_check_thread_func(void *arg)
{
	struct health_check_state *state = arg;
	struct timespec ts;
	
	while (state->active) {
		clock_gettime(CLOCK_REALTIME, &ts);
		
		pthread_mutex_lock(&state->lock);
		if (ts.tv_sec >= state->next_check && state->active) {
			int result = execute_health_check(state);
			process_health_result(state, result);
			
			/* Schedule next check */
			state->next_check = ts.tv_sec + 10;  /* Default period */
		}
		pthread_mutex_unlock(&state->lock);
		
		sleep(1);
	}
	
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
	pthread_mutex_unlock(&health_lock);
	
	return (0);
}

/*
 * Shutdown health checker
 */
void
health_checker_shutdown(void)
{
	pthread_mutex_lock(&health_lock);
	
	health_checker_running = false;
	
	/* Stop all health checks */
	for (int i = 0; i < MAX_HEALTH_CHECKS; i++) {
		if (health_checks[i] != NULL) {
			health_checks[i]->active = false;
			pthread_join(health_checks[i]->thread, NULL);
			pthread_mutex_destroy(&health_checks[i]->lock);
			free(health_checks[i]);
			health_checks[i] = NULL;
		}
	}
	health_check_count = 0;
	
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
		/* Bound the global table; overflowing it corrupts memory. */
		if (health_check_count >= MAX_HEALTH_CHECKS) {
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
		
		/* Start health check thread */
		pthread_create(&state->thread, NULL, health_check_thread_func, state);
		
		health_checks[health_check_count++] = state;
	}
	
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
	
	for (int i = 0; i < MAX_HEALTH_CHECKS; i++) {
		if (health_checks[i] != NULL &&
		    strcmp(health_checks[i]->service_name, service->name) == 0 &&
		    strcmp(health_checks[i]->namespace, service->namespace) == 0) {
			health_checks[i]->active = false;
			pthread_join(health_checks[i]->thread, NULL);
			pthread_mutex_destroy(&health_checks[i]->lock);
			free(health_checks[i]);
			health_checks[i] = NULL;
		}
	}
	
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
			result = health_check_http(url, &(int){0});
		} else if (hc->type == HEALTH_CHECK_EXEC) {
			result = run_health_command(hc->command, false);
		} else {
			result = 0;  /* No check defined */
		}
		return (result == 0 ? 0 : -1);
	}
	
	result = execute_health_check(state);
	process_health_result(state, result);
	
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
			int status = health_checks[i]->healthy ? 0 : -1;
			pthread_mutex_unlock(&health_lock);
			return (status);
		}
	}
	
	pthread_mutex_unlock(&health_lock);
	
	/* No health check running - assume healthy */
	return (0);
}
