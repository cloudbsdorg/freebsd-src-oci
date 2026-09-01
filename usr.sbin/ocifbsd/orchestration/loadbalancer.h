/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Service load balancer for ocifbsd.
 *
 * A service with N replicas is fronted by a virtual IP (VIP). The load
 * balancer distributes connections to the VIP across the replicas' endpoints
 * using pf(4) redirection pools -- native to FreeBSD, no external daemon. The
 * pool works no matter which node each replica runs on, as long as the replica
 * IPs are routable from the load-balancer host.
 *
 * Algorithm mapping (service_spec.network_config.lb_algorithm):
 *   roundrobin (default) -> pf "round-robin"
 *   iphash               -> pf "source-hash"  (also used for clientip affinity)
 *   leastconn            -> pf "round-robin"  (pf has no least-conn; documented)
 */

#ifndef OCIFBSD_ORCH_LOADBALANCER_H
#define OCIFBSD_ORCH_LOADBALANCER_H

#include "orchestration.h"

/*
 * Build the pf(4) ruleset that load-balances a service's VIP across its
 * ready replica endpoints. On success, *out is set to a malloc'd, NUL-
 * terminated ruleset the caller must free, and 0 is returned. Returns -1 on
 * bad arguments or allocation failure. If the service has no VIP, no ports, or
 * no eligible replicas, *out is set to an empty (comment-only) ruleset and 0
 * is returned.
 */
int	service_lb_build_rules(const struct service *svc, char **out);

/*
 * Apply / remove the load-balancer ruleset for a service by loading it into a
 * per-service pf anchor ("ocifbsd/lb/<service>"). Require pf to be enabled and
 * the caller to be root. Return 0 on success, -1 on failure.
 */
int	service_lb_apply(const struct service *svc);
int	service_lb_remove(const struct service *svc);

#endif /* OCIFBSD_ORCH_LOADBALANCER_H */
