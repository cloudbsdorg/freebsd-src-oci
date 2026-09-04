/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Behavioural test for the health-check scheduler.
 *
 * The question it asks is the only one that matters: after registering a check
 * against a port that nothing is listening on, does the replica actually get
 * marked unhealthy within a bounded time?
 *
 * Against the thread-per-replica scheduler the answer was no. That worker took
 * state->lock and then called process_health_result(), which takes state->lock
 * again -- a self-deadlock on a non-recursive mutex, hit on the FIRST due
 * check. The thread wedged, the status never changed, and because nothing
 * joined or polled it the failure was completely silent. This test is the
 * witness for that, and the acceptance test for the single-worker replacement.
 *
 * It also checks the properties the redesign is FOR: that N registered checks
 * cost one thread rather than N, and that unregistering while a probe is in
 * flight neither leaks the state nor frees it under the worker.
 */

#include <sys/param.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <sys/user.h>
#include <sys/wait.h>

#include <netinet/in.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "orchestration.h"

static int tests_run, tests_failed;

static void
check(const char *what, int cond)
{

	tests_run++;
	if (cond) {
		printf("  ok   %s\n", what);
		return;
	}
	tests_failed++;
	printf("  FAIL %s\n", what);
}

/*
 * Build a service with one replica whose TCP health check points at a port
 * that is closed, so every probe fails.
 */
static struct service *
make_service(const char *name, int nreplicas, int port, int period)
{
	struct service *svc;
	int i;

	svc = calloc(1, sizeof(*svc));
	svc->spec = calloc(1, sizeof(*svc->spec));
	svc->status = calloc(1, sizeof(*svc->status));
	svc->replicas = calloc((size_t)nreplicas, sizeof(*svc->replicas));
	if (svc == NULL || svc->spec == NULL || svc->status == NULL ||
	    svc->replicas == NULL)
		exit(2);

	strlcpy(svc->name, name, sizeof(svc->name));
	strlcpy(svc->namespace, "default", sizeof(svc->namespace));
	svc->nreplicas = nreplicas;
	for (i = 0; i < nreplicas; i++) {
		snprintf(svc->replicas[i].name, sizeof(svc->replicas[i].name),
		    "%s-replica-%d", name, i);
		strlcpy(svc->replicas[i].pod_ip, "127.0.0.1",
		    sizeof(svc->replicas[i].pod_ip));
		svc->replicas[i].state = REPLICA_STATE_RUNNING;
	}

	svc->spec->health_check.type = HEALTH_CHECK_TCP;
	svc->spec->health_check.port = port;
	svc->spec->health_check.period = period;
	svc->spec->health_check.initial_delay = 0;
	svc->spec->health_check.failure_threshold = 1;
	svc->spec->health_check.success_threshold = 1;
	return (svc);
}

static void
free_service(struct service *svc)
{

	free(svc->replicas);
	free(svc->status);
	free(svc->spec);
	free(svc);
}

/*
 * This process's thread count, via kern.proc.pid. The whole point of the
 * redesign is that this stays flat as replicas scale, so assert it rather
 * than merely believing it.
 */
static int
my_threads(void)
{
	struct kinfo_proc kp;
	size_t len = sizeof(kp);
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, 0 };

	mib[3] = (int)getpid();
	if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0)
		return (-1);
	return (kp.ki_numthreads);
}

/* A port with nothing bound to it, so connect() is refused. */
static int
closed_port(void)
{
	int s, port;
	struct sockaddr_in a;
	socklen_t len = sizeof(a);

	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		exit(2);
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	a.sin_port = 0;
	if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0)
		exit(2);
	if (getsockname(s, (struct sockaddr *)&a, &len) != 0)
		exit(2);
	port = ntohs(a.sin_port);
	close(s);			/* now nothing is listening there */
	return (port);
}

/*
 * The core property: a failing check must actually be OBSERVED within a
 * bounded time. Poll rather than sleeping a fixed span so a working scheduler
 * finishes fast and a wedged one still terminates the test.
 */
static int
wait_unhealthy(struct service *svc, const char *replica, int timeout_s)
{
	time_t deadline = time(NULL) + timeout_s;

	while (time(NULL) < deadline) {
		if (health_check_get_status(svc, replica) != 0)
			return (1);
		usleep(100000);
	}
	return (0);
}

int
main(void)
{
	struct service *svc, *big;
	int port, i;

	/*
	 * Unbuffered: if the scheduler wedges, the harness kills this process
	 * and a buffered stdout would take the evidence with it -- exactly the
	 * case this test exists to diagnose.
	 */
	setvbuf(stdout, NULL, _IONBF, 0);

	port = closed_port();
	printf("health_sched_test: probing closed port %d\n", port);

	if (health_checker_init() != 0) {
		printf("FAIL health_checker_init\n");
		return (1);
	}

	/* 1. A failing check must mark the replica unhealthy. */
	svc = make_service("hsvc", 1, port, 1);
	check("health_check_start accepted the service",
	    health_check_start(svc) == 0);
	check("failing check is OBSERVED within 10s "
	    "(deadlocked scheduler never gets here)",
	    wait_unhealthy(svc, "hsvc-replica-0", 10));

	/* 2. Stop must remove it; status then reads healthy again. */
	check("health_check_stop succeeded", health_check_stop(svc) == 0);
	check("status after stop is the no-check default",
	    health_check_get_status(svc, "hsvc-replica-0") == 0);

	/* 3. Start/stop cycles must not exhaust the table. */
	for (i = 0; i < 40; i++) {
		if (health_check_start(svc) != 0) {
			printf("  (table exhausted after %d cycles)\n", i);
			break;
		}
		health_check_stop(svc);
	}
	check("40 start/stop cycles do not exhaust the table", i == 40);

	/*
	 * 4. Many replicas must not mean many threads. Registering 200 checks
	 *    under the old model created 200 threads, each polling once a
	 *    second forever; the redesign is one worker for all of them.
	 */
	{
		int t_before, t_after;

		t_before = my_threads();
		big = make_service("bigsvc", 200, port, 30);
		check("200 replicas register", health_check_start(big) == 0);
		t_after = my_threads();
		printf("       threads: %d before, %d with 200 checks "
		    "registered\n", t_before, t_after);
		/*
		 * Thread-per-replica added ~200 here. One worker serves them
		 * all, so the count must not move at all.
		 */
		check("200 registered checks add NO threads",
		    t_after >= 0 && t_before >= 0 && t_after == t_before);
		check("200 failing checks are still observed",
		    wait_unhealthy(big, "bigsvc-replica-0", 15));

		/*
		 * Teardown used to be a serialized join per check, about a
		 * second each. Require the whole batch to unregister promptly.
		 */
		{
			time_t t0 = time(NULL);

			check("stopping 200 checks succeeds",
			    health_check_stop(big) == 0);
			printf("       teardown of 200 checks took %llds\n",
			    (long long)(time(NULL) - t0));
			check("stopping 200 checks takes under 5s "
			    "(was ~1s per check, serialized)",
			    time(NULL) - t0 < 5);
		}
	}

	/*
	 * 5. Stop while a probe is IN FLIGHT.
	 *
	 * The worker runs probes with no lock held, so a stop arriving mid
	 * probe must not free the state under it. A TCP probe to a
	 * non-routable address blocks until the socket timeout, which gives a
	 * reliable window to stop inside. The entry is marked doomed and
	 * released by the worker when the probe returns; if that handoff were
	 * wrong this is where a use-after-free would show up under ASan.
	 */
	{
		struct service *slow;
		time_t t0;

		slow = make_service("slowsvc", 4, 9, 1);
		/* 10.255.255.1 is non-routable here: connect() hangs. */
		for (i = 0; i < slow->nreplicas; i++)
			strlcpy(slow->replicas[i].pod_ip, "10.255.255.1",
			    sizeof(slow->replicas[i].pod_ip));
		check("slow-probe service registers",
		    health_check_start(slow) == 0);
		sleep(1);			/* let a probe get in flight */
		t0 = time(NULL);
		check("stop during an in-flight probe returns",
		    health_check_stop(slow) == 0);
		printf("       stop during in-flight probe took %llds\n",
		    (long long)(time(NULL) - t0));
		check("stop during an in-flight probe does not block on it",
		    time(NULL) - t0 < 3);
		/* Give the worker time to reap the doomed entry. */
		sleep(7);
		check("status of a stopped replica is the default",
		    health_check_get_status(slow, "slowsvc-replica-0") == 0);
		free_service(slow);
	}

	/* 6. Repeated churn: registration and release must not leak slots. */
	for (i = 0; i < 100; i++) {
		if (health_check_start(big) != 0)
			break;
		if (health_check_stop(big) != 0)
			break;
	}
	check("100 x 200-replica start/stop cycles keep working", i == 100);

	health_checker_shutdown();
	free_service(svc);
	free_service(big);

	printf("health_sched_test: %d checks, %d failed\n", tests_run,
	    tests_failed);
	return (tests_failed == 0 ? 0 : 1);
}
