/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Unit tests for the service load balancer (orchestration/loadbalancer.c).
 */

#include <atf-c.h>
#include <stdlib.h>
#include <string.h>

#include "orchestration/loadbalancer.c"

/* Build a 3-replica service with a VIP and one 80->8080 port mapping. */
static void
make_service(struct service *svc, struct service_spec *spec,
    struct service_status *status, struct service_replica *reps,
    struct port_mapping *pm, const char *algo)
{
	memset(svc, 0, sizeof(*svc));
	memset(spec, 0, sizeof(*spec));
	memset(status, 0, sizeof(*status));
	memset(reps, 0, sizeof(*reps) * 3);
	memset(pm, 0, sizeof(*pm));

	pm->host_port = 80;
	pm->container_port = 8080;
	strlcpy(pm->protocol, "tcp", sizeof(pm->protocol));

	spec->ports = pm;
	spec->nports = 1;
	spec->replicas = 3;
	strlcpy(spec->network_config.lb_algorithm, algo,
	    sizeof(spec->network_config.lb_algorithm));

	strlcpy(status->load_balancer_ip, "203.0.113.100",
	    sizeof(status->load_balancer_ip));

	for (int i = 0; i < 3; i++) {
		reps[i].state = REPLICA_STATE_RUNNING;
		snprintf(reps[i].pod_ip, sizeof(reps[i].pod_ip),
		    "203.0.113.%d", 11 + i);
		snprintf(reps[i].node, sizeof(reps[i].node), "worker%d", i + 1);
	}

	svc->spec = spec;
	svc->status = status;
	svc->replicas = reps;
	svc->nreplicas = 3;
}

ATF_TC_WITHOUT_HEAD(lb_roundrobin_pool);
ATF_TC_BODY(lb_roundrobin_pool, tc)
{
	struct service svc;
	struct service_spec spec;
	struct service_status status;
	struct service_replica reps[3];
	struct port_mapping pm;
	char *rules = NULL;

	make_service(&svc, &spec, &status, reps, &pm, "roundrobin");
	ATF_REQUIRE_EQ(0, service_lb_build_rules(&svc, &rules));
	ATF_REQUIRE(rules != NULL);

	/* A redirect rule for the VIP:80 with a round-robin pool of replicas. */
	ATF_CHECK_MSG(strstr(rules, "rdr") != NULL, "no rdr rule generated");
	ATF_CHECK_MSG(strstr(rules, "203.0.113.100") != NULL, "VIP missing");
	ATF_CHECK_MSG(strstr(rules, "round-robin") != NULL, "not round-robin");
	ATF_CHECK_MSG(strstr(rules, "203.0.113.11") != NULL, "replica 1 missing");
	ATF_CHECK_MSG(strstr(rules, "203.0.113.12") != NULL, "replica 2 missing");
	ATF_CHECK_MSG(strstr(rules, "203.0.113.13") != NULL, "replica 3 missing");
	ATF_CHECK_MSG(strstr(rules, "port 80") != NULL, "frontend port missing");
	ATF_CHECK_MSG(strstr(rules, "8080") != NULL, "backend port missing");
	free(rules);
}

/* iphash / clientip affinity selects a source-hash pool. */
ATF_TC_WITHOUT_HEAD(lb_iphash_source_hash);
ATF_TC_BODY(lb_iphash_source_hash, tc)
{
	struct service svc;
	struct service_spec spec;
	struct service_status status;
	struct service_replica reps[3];
	struct port_mapping pm;
	char *rules = NULL;

	make_service(&svc, &spec, &status, reps, &pm, "iphash");
	ATF_REQUIRE_EQ(0, service_lb_build_rules(&svc, &rules));
	ATF_REQUIRE(rules != NULL);
	ATF_CHECK_MSG(strstr(rules, "source-hash") != NULL,
	    "iphash should map to pf source-hash");
	ATF_CHECK_MSG(strstr(rules, "round-robin") == NULL,
	    "iphash should not be round-robin");
	free(rules);
}

/* Only RUNNING replicas become backends; failed/terminating are excluded. */
ATF_TC_WITHOUT_HEAD(lb_excludes_unhealthy);
ATF_TC_BODY(lb_excludes_unhealthy, tc)
{
	struct service svc;
	struct service_spec spec;
	struct service_status status;
	struct service_replica reps[3];
	struct port_mapping pm;
	char *rules = NULL;

	make_service(&svc, &spec, &status, reps, &pm, "roundrobin");
	reps[1].state = REPLICA_STATE_FAILED;	/* 203.0.113.12 down */
	ATF_REQUIRE_EQ(0, service_lb_build_rules(&svc, &rules));
	ATF_REQUIRE(rules != NULL);
	ATF_CHECK_MSG(strstr(rules, "203.0.113.11") != NULL, "healthy replica missing");
	ATF_CHECK_MSG(strstr(rules, "203.0.113.13") != NULL, "healthy replica missing");
	ATF_CHECK_MSG(strstr(rules, "203.0.113.12") == NULL,
	    "failed replica must not be a backend");
	free(rules);
}

/* No VIP or no replicas yields an empty (non-crashing) ruleset. */
ATF_TC_WITHOUT_HEAD(lb_empty_when_no_backends);
ATF_TC_BODY(lb_empty_when_no_backends, tc)
{
	struct service svc;
	struct service_spec spec;
	struct service_status status;
	struct service_replica reps[3];
	struct port_mapping pm;
	char *rules = NULL;

	make_service(&svc, &spec, &status, reps, &pm, "roundrobin");
	svc.nreplicas = 0;
	ATF_REQUIRE_EQ(0, service_lb_build_rules(&svc, &rules));
	ATF_REQUIRE(rules != NULL);
	ATF_CHECK_MSG(strstr(rules, "rdr") == NULL,
	    "no rdr rule expected without backends");
	free(rules);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, lb_roundrobin_pool);
	ATF_TP_ADD_TC(tp, lb_iphash_source_hash);
	ATF_TP_ADD_TC(tp, lb_excludes_unhealthy);
	ATF_TP_ADD_TC(tp, lb_empty_when_no_backends);
	return (atf_no_error());
}
