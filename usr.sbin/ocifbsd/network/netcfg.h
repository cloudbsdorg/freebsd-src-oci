/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * $FreeBSD$
 *
 * Container network configuration: a small, self-contained model for the
 * per-container network settings that `ocifbsd network list|set` manages and
 * persists as network.json in the image/bundle store. Kept independent of the
 * full OCI runtime spec so it can be parsed, validated, and serialized (and
 * unit-tested) on its own.
 */

#ifndef _OCIFBSD_NETWORK_NETCFG_H
#define _OCIFBSD_NETWORK_NETCFG_H

#include <stdbool.h>
#include <stddef.h>

struct netcfg {
	int	vnet;			/* -1 unset, 0 disabled, 1 enabled */
	char	**ip4;			/* IPv4 addresses in CIDR form */
	size_t	n_ip4;
	char	**ip6;			/* IPv6 addresses in CIDR form */
	size_t	n_ip6;
	char	*gateway4;		/* default IPv4 gateway, or NULL */
	char	*gateway6;		/* default IPv6 gateway, or NULL */
	char	**dns;			/* DNS nameserver addresses */
	size_t	n_dns;
	char	*bridge;		/* host bridge to attach the epair to */
};

/* Lifecycle. */
void	netcfg_init(struct netcfg *nc);
void	netcfg_free(struct netcfg *nc);

/*
 * Validation helpers. A CIDR must be <addr>/<prefix> with a numeric,
 * in-range prefix (0-32 for v4, 0-128 for v6) and an address that parses.
 * A plain address (no prefix) is accepted for gateways and DNS servers.
 */
bool	netcfg_valid_ip4_cidr(const char *s);
bool	netcfg_valid_ip6_cidr(const char *s);
bool	netcfg_valid_ip4_addr(const char *s);
bool	netcfg_valid_ip6_addr(const char *s);

/*
 * Mutators. Each returns 0 on success or -1 (with errno set) on an invalid
 * value or allocation failure; on failure the config is left unchanged.
 */
int	netcfg_set_vnet(struct netcfg *nc, bool on);
int	netcfg_add_ip4(struct netcfg *nc, const char *cidr);
int	netcfg_add_ip6(struct netcfg *nc, const char *cidr);
int	netcfg_set_gateway4(struct netcfg *nc, const char *gw);
int	netcfg_set_gateway6(struct netcfg *nc, const char *gw);
int	netcfg_set_bridge(struct netcfg *nc, const char *bridge);
int	netcfg_add_dns(struct netcfg *nc, const char *ns);
void	netcfg_clear_ip4(struct netcfg *nc);
void	netcfg_clear_ip6(struct netcfg *nc);
void	netcfg_clear_dns(struct netcfg *nc);

/*
 * Serialization. netcfg_to_json returns a malloc'd, NUL-terminated JSON
 * object (caller frees). netcfg_parse populates *out from a JSON object
 * string; unknown keys are ignored and missing keys leave defaults. Returns
 * 0 on success, -1 on malformed JSON.
 */
char	*netcfg_to_json(const struct netcfg *nc);
int	netcfg_parse(const char *json, struct netcfg *out);

#endif /* _OCIFBSD_NETWORK_NETCFG_H */
