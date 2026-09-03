/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 REVYTECH, Inc.
 *
 * ocifbsd's native L4 (TCP) load balancer / reverse proxy (the `ocifbsd proxy`
 * subcommand) and its circuit breaker. Extracted from the CLI dispatcher into
 * its own module so ~500 lines of network machinery no longer live inside
 * ocifbsd.c.
 */

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/mman.h>

#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../include/ocifbsd.h"

/*
 * ocifbsd proxy — a small L4 (TCP) round-robin load balancer with failover, so
 * a service's replicas can be fronted by a single endpoint without pulling in
 * an external proxy. Connections are distributed across the --backend targets
 * in round-robin order; a backend that will not accept a connection is skipped
 * and the next tried, giving health-based failover. Each accepted connection is
 * handled by a forked child that splices bytes in both directions.
 */
struct proxy_backend {
	char	*host;
	char	*port;
};

/*
 * Circuit-breaker health, shared across the pre-forked accept workers and their
 * per-connection children via anonymous shared memory. After
 * PROXY_CB_THRESHOLD consecutive dial failures a backend is tripped "open" and
 * skipped entirely until dead_until, so a downed replica costs one failure to
 * detect and then zero — no per-connection dial timeout — instead of stalling
 * every connection that happens to land on it. Once the cooldown elapses a
 * single connection is allowed through (half-open); if it connects the backend
 * is reset to healthy, if it fails the breaker re-trips immediately.
 */
#define	PROXY_CB_THRESHOLD	3	/* consecutive fails before tripping */
#define	PROXY_CB_COOLDOWN	10	/* seconds a tripped backend is skipped */

struct proxy_health {
	int	fails;		/* consecutive dial failures */
	time_t	dead_until;	/* skip until this wall-clock time (0 = healthy) */
};

/* Load-balancing algorithms. */
enum proxy_algo {
	ALGO_ROUNDROBIN,	/* even rotation (default) */
	ALGO_RANDOM,		/* uniformly random backend */
	ALGO_SOURCEHASH,	/* hash(client IP) -> sticky affinity */
	ALGO_LEASTCONN		/* fewest active connections */
};

static enum proxy_algo
proxy_algo_parse(const char *s)
{
	if (strcmp(s, "random") == 0)
		return (ALGO_RANDOM);
	if (strcmp(s, "source-hash") == 0 || strcmp(s, "source") == 0 ||
	    strcmp(s, "iphash") == 0)
		return (ALGO_SOURCEHASH);
	if (strcmp(s, "least-conn") == 0 || strcmp(s, "leastconn") == 0)
		return (ALGO_LEASTCONN);
	return (ALGO_ROUNDROBIN);
}

/*
 * FNV-1a hash of a client's IP address (NOT the port), for source-hash
 * affinity: every connection from the same client sticks to the same backend,
 * which is how a stateful session survives without a shared store.
 */
static unsigned long
proxy_addr_hash(const struct sockaddr *sa, socklen_t slen)
{
	const unsigned char *p;
	size_t n;
	unsigned long h = 1469598103934665603UL;
	size_t i;

	if (sa->sa_family == AF_INET) {
		p = (const unsigned char *)
		    &((const struct sockaddr_in *)(const void *)sa)->sin_addr;
		n = sizeof(struct in_addr);
	} else if (sa->sa_family == AF_INET6) {
		p = (const unsigned char *)
		    &((const struct sockaddr_in6 *)(const void *)sa)->sin6_addr;
		n = sizeof(struct in6_addr);
	} else {
		p = (const unsigned char *)sa;
		n = slen;
	}
	for (i = 0; i < n; i++) {
		h ^= p[i];
		h *= 1099511628211UL;
	}
	return (h);
}

/* Connect to host:port with a bounded timeout; returns a blocking fd or -1. */
static int
proxy_dial(const char *host, const char *port, int timeout_ms)
{
	struct addrinfo hints, *res, *ai;
	int fd = -1;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res) != 0)
		return (-1);
	for (ai = res; ai != NULL; ai = ai->ai_next) {
		int fl;

		fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
		if (fd < 0)
			continue;
		fl = fcntl(fd, F_GETFL, 0);
		(void)fcntl(fd, F_SETFL, fl | O_NONBLOCK);
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
			(void)fcntl(fd, F_SETFL, fl);
			break;
		}
		if (errno == EINPROGRESS) {
			struct pollfd pfd = { fd, POLLOUT, 0 };
			int e = 0;
			socklen_t el = sizeof(e);

			if (poll(&pfd, 1, timeout_ms) == 1 &&
			    getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) == 0 &&
			    e == 0) {
				(void)fcntl(fd, F_SETFL, fl);
				break;
			}
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	return (fd);
}

/*
 * Splice bytes both ways with correct half-close semantics: when one side
 * sends EOF we half-close only the *other* side's write end and keep relaying
 * the opposite direction until it too closes. Tearing both sides down on the
 * first EOF (as a naive pump does) truncates an in-flight response whenever the
 * client half-closes after sending its request — which HTTP clients routinely
 * do, and which surfaces as intermittent upstream 5xx.
 */
static void
proxy_pump(int a, int b)
{
	struct pollfd pfd[2];
	char buf[65536];
	int a_readable = 1, b_readable = 1;

	for (;;) {
		if (!a_readable && !b_readable)
			return;
		pfd[0].fd = a;
		pfd[0].events = a_readable ? POLLIN : 0;
		pfd[0].revents = 0;
		pfd[1].fd = b;
		pfd[1].events = b_readable ? POLLIN : 0;
		pfd[1].revents = 0;
		if (poll(pfd, 2, -1) < 0) {
			if (errno == EINTR)
				continue;
			return;
		}
		for (int i = 0; i < 2; i++) {
			if (pfd[i].revents & (POLLIN | POLLHUP | POLLERR)) {
				int from = pfd[i].fd, to = pfd[1 - i].fd;
				ssize_t n = read(from, buf, sizeof(buf));

				if (n > 0) {
					ssize_t off = 0;

					while (off < n) {
						ssize_t w = write(to, buf + off,
						    (size_t)(n - off));
						if (w <= 0)
							return;
						off += w;
					}
				} else {
					/* EOF/error on `from`: half-close the
					 * peer's write side, stop reading here,
					 * but keep draining the other way. */
					(void)shutdown(to, SHUT_WR);
					if (i == 0)
						a_readable = 0;
					else
						b_readable = 0;
				}
			}
		}
	}
}

static void
proxy_reap(int sig)
{
	(void)sig;
	while (waitpid(-1, NULL, WNOHANG) > 0)
		;
}

/*
 * One accept worker. Multiple of these run as pre-forked processes sharing the
 * same listening socket (the kernel load-balances accept across them, using all
 * cores), which is how the balancer scales past a single accept loop. Each
 * accepted connection is still handed to a forked child; the backend dial
 * happens there so accept never blocks.
 */
static void
proxy_accept_loop(int lfd, struct proxy_backend *backends, int nbackends,
    enum proxy_algo algo, int *conns, struct proxy_health *health,
    int timeout_ms)
{
	int rr = 0;

	signal(SIGCHLD, proxy_reap);
	signal(SIGPIPE, SIG_IGN);
	/* Distinct RNG stream per pre-forked worker for the random algo. */
	srand((unsigned)(getpid() ^ (int)time(NULL)));
	for (;;) {
		struct sockaddr_storage ss;
		socklen_t sslen = sizeof(ss);
		int cfd = accept(lfd, (struct sockaddr *)&ss, &sslen);
		int start;
		time_t now;
		pid_t pid;

		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		now = time(NULL);
		switch (algo) {
		case ALGO_RANDOM:
			start = (int)((unsigned)rand() % (unsigned)nbackends);
			break;
		case ALGO_SOURCEHASH:
			start = (int)(proxy_addr_hash((struct sockaddr *)&ss,
			    sslen) % (unsigned long)nbackends);
			break;
		case ALGO_LEASTCONN: {
			int m = -1;

			/* Fewest active connections among live backends. */
			for (int i = 0; i < nbackends; i++) {
				if (health != NULL && health[i].dead_until > now)
					continue;
				if (m < 0 || conns[i] < conns[m])
					m = i;
			}
			start = (m >= 0) ? m : 0;
			break;
		}
		case ALGO_ROUNDROBIN:
		default:
			start = rr;
			break;
		}
		rr = (start + 1) % nbackends;

		pid = fork();
		if (pid == 0) {
			int bfd = -1, tries, chosen = -1;

			/*
			 * Phase 1: honor the circuit breaker. Skip any backend
			 * currently tripped open — no dial, no timeout — so a
			 * dead replica is short-circuited after the first
			 * failure detects it. Update shared health as we go: a
			 * connect resets the backend to healthy, a failure
			 * counts toward tripping it.
			 */
			for (tries = 0; tries < nbackends; tries++) {
				int idx = (start + tries) % nbackends;

				if (health != NULL && health[idx].dead_until > now)
					continue;
				bfd = proxy_dial(backends[idx].host,
				    backends[idx].port, timeout_ms);
				if (bfd >= 0) {
					if (health != NULL) {
						health[idx].fails = 0;
						health[idx].dead_until = 0;
					}
					chosen = idx;
					break;
				}
				if (health != NULL &&
				    ++health[idx].fails >= PROXY_CB_THRESHOLD)
					health[idx].dead_until =
					    now + PROXY_CB_COOLDOWN;
			}
			/*
			 * Phase 2: every backend was tripped. Rather than drop
			 * the client, make one last-resort pass that ignores the
			 * breaker — a half-open probe across all of them.
			 */
			if (bfd < 0 && health != NULL) {
				for (tries = 0; tries < nbackends; tries++) {
					int idx = (start + tries) % nbackends;

					bfd = proxy_dial(backends[idx].host,
					    backends[idx].port, timeout_ms);
					if (bfd >= 0) {
						health[idx].fails = 0;
						health[idx].dead_until = 0;
						chosen = idx;
						break;
					}
				}
			}
			if (bfd < 0)
				_exit(1);
			if (conns != NULL)
				conns[chosen]++;
			proxy_pump(cfd, bfd);
			if (conns != NULL)
				conns[chosen]--;
			_exit(0);
		}
		close(cfd);
	}
}

/* Local usage for the proxy subcommand (the CLI's usage() is private to
 * ocifbsd.c; this module prints its own so it stays self-contained). */
static void
proxy_usage(void)
{
	fprintf(stderr, "usage: ocifbsd proxy --listen [addr:]port "
	    "--backend host:port [--backend ...] "
	    "[--algo round-robin|random|source-hash|least-conn] "
	    "[--timeout ms] [--workers N]\n");
}

int
cmd_proxy(int argc, char **argv)
{
	const char *listen_spec = NULL;
	struct proxy_backend backends[64];
	int nbackends = 0, ch, timeout_ms = 2000, lfd;
	char lhost[256], lport[16];
	enum proxy_algo algo = ALGO_ROUNDROBIN;
	int *conns = NULL;	/* shared active-connection counts (least-conn) */
	struct proxy_health *health = NULL;	/* shared circuit-breaker state */
	int workers = 0;	/* accept workers; 0 => one per CPU */

	static struct option longopts[] = {
		{ "listen",	required_argument,	NULL, 'l' },
		{ "backend",	required_argument,	NULL, 'b' },
		{ "timeout",	required_argument,	NULL, 't' },
		{ "algo",	required_argument,	NULL, 'a' },
		{ "workers",	required_argument,	NULL, 'w' },
		{ "help",	no_argument,		NULL, 'h' },
		{ NULL,		0,			NULL, 0 }
	};

	optreset = 1;
	optind = 1;
	while ((ch = getopt_long(argc, argv, "l:b:t:a:w:h", longopts,
	    NULL)) != -1) {
		switch (ch) {
		case 'l':
			listen_spec = optarg;
			break;
		case 'a':
			algo = proxy_algo_parse(optarg);
			break;
		case 'w':
			workers = atoi(optarg);
			break;
		case 'b': {
			char *colon = strrchr(optarg, ':');

			if (colon == NULL) {
				fprintf(stderr, "error: --backend needs "
				    "host:port\n");
				return (1);
			}
			if (nbackends < 64) {
				*colon = '\0';
				backends[nbackends].host = strdup(optarg);
				backends[nbackends].port = strdup(colon + 1);
				nbackends++;
			}
			break;
		}
		case 't':
			timeout_ms = atoi(optarg);
			break;
		case 'h':
			proxy_usage();
			return (0);
		default:
			proxy_usage();
			return (1);
		}
	}
	if (listen_spec == NULL || nbackends == 0) {
		fprintf(stderr, "error: proxy requires --listen and at least "
		    "one --backend\n");
		return (1);
	}

	/* Split listen spec into [addr:]port. */
	{
		const char *colon = strrchr(listen_spec, ':');

		if (colon != NULL) {
			size_t hl = (size_t)(colon - listen_spec);

			if (hl >= sizeof(lhost))
				hl = sizeof(lhost) - 1;
			memcpy(lhost, listen_spec, hl);
			lhost[hl] = '\0';
			snprintf(lport, sizeof(lport), "%s", colon + 1);
		} else {
			lhost[0] = '\0';
			snprintf(lport, sizeof(lport), "%s", listen_spec);
		}
	}

	/* Bind + listen. */
	{
		struct addrinfo hints, *res;
		int one = 1;

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_flags = AI_PASSIVE;
		if (getaddrinfo(lhost[0] ? lhost : NULL, lport, &hints,
		    &res) != 0) {
			fprintf(stderr, "error: cannot resolve listen %s\n",
			    listen_spec);
			return (1);
		}
		lfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
		if (lfd < 0) {
			perror("socket");
			freeaddrinfo(res);
			return (1);
		}
		(void)setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one,
		    sizeof(one));
		if (bind(lfd, res->ai_addr, res->ai_addrlen) != 0) {
			perror("bind");
			freeaddrinfo(res);
			return (1);
		}
		freeaddrinfo(res);
		if (listen(lfd, 1024) != 0) {
			perror("listen");
			return (1);
		}
	}

	/*
	 * least-conn needs live connection counts shared across the
	 * fork-per-connection children; back them with anonymous shared memory
	 * so a child can decrement its backend on exit.
	 */
	if (algo == ALGO_LEASTCONN) {
		conns = mmap(NULL, sizeof(int) * (size_t)nbackends,
		    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
		if (conns == MAP_FAILED) {
			conns = NULL;
			algo = ALGO_ROUNDROBIN;
		}
	}

	/*
	 * Circuit-breaker health is shared by every worker and child regardless
	 * of algorithm, so a backend tripped by one connection is skipped by
	 * all the others. A failed mapping just disables the breaker (health
	 * stays NULL and every backend is always eligible).
	 */
	health = mmap(NULL, sizeof(struct proxy_health) * (size_t)nbackends,
	    PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANON, -1, 0);
	if (health == MAP_FAILED)
		health = NULL;

	if (workers <= 0) {
		long ncpu = sysconf(_SC_NPROCESSORS_ONLN);

		workers = (ncpu > 0) ? (int)ncpu : 4;
	}

	{
		const char *an = algo == ALGO_RANDOM ? "random" :
		    algo == ALGO_SOURCEHASH ? "source-hash (sticky)" :
		    algo == ALGO_LEASTCONN ? "least-conn" : "round-robin";

		printf("ocifbsd proxy: listening on %s -> %d backend(s), "
		    "algo=%s, %d accept workers, failover + circuit-breaker "
		    "(%d fails/%ds cooldown)%s\n",
		    listen_spec, nbackends, an, workers,
		    PROXY_CB_THRESHOLD, PROXY_CB_COOLDOWN,
		    health != NULL ? "" : " [breaker disabled]");
		fflush(stdout);
	}

	/*
	 * Pre-fork the accept workers. They share `lfd`; the kernel balances
	 * incoming connections across them so accept scales across cores
	 * instead of bottlenecking on one loop. The parent just supervises
	 * and replaces any worker that dies.
	 */
	signal(SIGPIPE, SIG_IGN);
	for (int w = 0; w < workers; w++) {
		if (fork() == 0) {
			proxy_accept_loop(lfd, backends, nbackends, algo, conns,
			    health, timeout_ms);
			_exit(0);
		}
	}
	for (;;) {
		int status;
		pid_t dead = wait(&status);

		if (dead < 0) {
			if (errno == EINTR)
				continue;
			break;		/* no children left */
		}
		/* A worker died; respawn one to keep the pool full. */
		if (fork() == 0) {
			proxy_accept_loop(lfd, backends, nbackends, algo, conns,
			    health, timeout_ms);
			_exit(0);
		}
	}
	close(lfd);
	return (0);
}
