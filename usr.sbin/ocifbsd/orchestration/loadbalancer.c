/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Service load balancer: generate and apply pf(4) redirection pools that
 * spread connections to a service VIP across its ready replica endpoints.
 * See loadbalancer.h for the algorithm mapping and anchor layout.
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "loadbalancer.h"

/* Map the configured algorithm (and session affinity) to a pf pool type. */
static const char *
lb_pool_type(const struct service *svc)
{
	const char *algo = svc->spec->network_config.lb_algorithm;
	const char *affinity = svc->spec->network_config.session_affinity;

	if (affinity != NULL && strcasecmp(affinity, "clientip") == 0)
		return ("source-hash");
	if (algo[0] != '\0' && strcasecmp(algo, "iphash") == 0)
		return ("source-hash");
	/* roundrobin (default) and leastconn (pf has no least-conn) */
	return ("round-robin");
}

/* Is this replica an eligible backend right now? */
static int
lb_replica_ready(const struct service_replica *r)
{
	return (r->state == REPLICA_STATE_RUNNING && r->pod_ip[0] != '\0');
}

int
service_lb_build_rules(const struct service *svc, char **out)
{
	FILE *f;
	char *buf = NULL;
	size_t len = 0;
	int nbackends = 0;
	const char *pool;

	if (svc == NULL || svc->spec == NULL || svc->status == NULL ||
	    out == NULL)
		return (-1);

	f = open_memstream(&buf, &len);
	if (f == NULL)
		return (-1);

	pool = lb_pool_type(svc);

	fprintf(f, "# ocifbsd load balancer for service %s\n", svc->name);
	fprintf(f, "# VIP %s  algorithm %s\n",
	    svc->status->load_balancer_ip[0] ? svc->status->load_balancer_ip :
	    "(unset)",
	    svc->spec->network_config.lb_algorithm[0] ?
	    svc->spec->network_config.lb_algorithm : "roundrobin");

	for (int i = 0; i < svc->nreplicas; i++)
		if (lb_replica_ready(&svc->replicas[i]))
			nbackends++;

	if (svc->status->load_balancer_ip[0] == '\0' || nbackends == 0 ||
	    svc->spec->nports == 0) {
		fprintf(f, "# no active backends; no redirect installed\n");
		if (fclose(f) != 0) {
			free(buf);
			return (-1);
		}
		*out = buf;
		return (0);
	}

	/* One redirect rule per published port, each with the backend pool. */
	for (int p = 0; p < svc->spec->nports; p++) {
		const struct port_mapping *pm = &svc->spec->ports[p];
		const char *proto = pm->protocol[0] ? pm->protocol : "tcp";

		fprintf(f, "rdr pass proto %s from any to %s port %u -> {",
		    proto, svc->status->load_balancer_ip, pm->host_port);
		for (int i = 0; i < svc->nreplicas; i++) {
			if (!lb_replica_ready(&svc->replicas[i]))
				continue;
			fprintf(f, " %s", svc->replicas[i].pod_ip);
		}
		fprintf(f, " }");
		if (pm->container_port != 0)
			fprintf(f, " port %u", pm->container_port);
		/* pf requires a pool type only for more than one address, but
		 * emitting it for one is harmless and keeps intent explicit. */
		fprintf(f, " %s\n", pool);
	}

	if (fclose(f) != 0) {
		free(buf);
		return (-1);
	}
	*out = buf;
	return (0);
}

/* Sanitize a service name into a pf anchor path component. */
static void
lb_anchor(const struct service *svc, char *anchor, size_t anchorlen)
{
	size_t j = 0;

	for (size_t i = 0; svc->name[i] != '\0' && j + 1 < anchorlen; i++) {
		char c = svc->name[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.')
			anchor[j++] = c;
		else
			anchor[j++] = '_';
	}
	anchor[j] = '\0';
}

/* Run /sbin/pfctl with the given argv (NULL-terminated). Returns 0 on exit 0. */
static int
lb_pfctl(char *const argv[])
{
	pid_t pid;
	int status;

	pid = fork();
	if (pid < 0)
		return (-1);
	if (pid == 0) {
		execv("/sbin/pfctl", argv);
		_exit(127);
	}
	if (waitpid(pid, &status, 0) < 0)
		return (-1);
	return ((WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1);
}

int
service_lb_apply(const struct service *svc)
{
	char *rules = NULL;
	char anchor[320];
	char tmpl[] = "/tmp/ocifbsd-lb.XXXXXX";
	char full[384];
	int fd, rc;
	size_t n;

	if (svc == NULL)
		return (-1);
	if (service_lb_build_rules(svc, &rules) != 0)
		return (-1);

	fd = mkstemp(tmpl);
	if (fd < 0) {
		free(rules);
		return (-1);
	}
	n = strlen(rules);
	if (write(fd, rules, n) != (ssize_t)n) {
		close(fd);
		unlink(tmpl);
		free(rules);
		return (-1);
	}
	close(fd);
	free(rules);

	lb_anchor(svc, anchor, sizeof(anchor));
	snprintf(full, sizeof(full), "ocifbsd/lb/%s", anchor);
	{
		char *argv[] = { (char *)"pfctl", (char *)"-a", full,
		    (char *)"-f", tmpl, NULL };
		rc = lb_pfctl(argv);
	}
	unlink(tmpl);
	return (rc);
}

int
service_lb_remove(const struct service *svc)
{
	char anchor[320];
	char full[384];

	if (svc == NULL)
		return (-1);
	lb_anchor(svc, anchor, sizeof(anchor));
	snprintf(full, sizeof(full), "ocifbsd/lb/%s", anchor);
	{
		char *argv[] = { (char *)"pfctl", (char *)"-a", full,
		    (char *)"-F", (char *)"all", NULL };
		return (lb_pfctl(argv));
	}
}
