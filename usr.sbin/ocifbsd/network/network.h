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
 * FreeBSD OCI Runtime - Networking
 */

#ifndef _OCIFBSD_NETWORK_H
#define _OCIFBSD_NETWORK_H

#include <sys/types.h>
#include <stdbool.h>
#include <netinet/in.h>

/*
 * Network types
 */
typedef enum {
	NETWORK_TYPE_BRIDGE = 0,
	NETWORK_TYPE_ROUTED,
	NETWORK_TYPE_OVERLAY,
	NETWORK_TYPE_HOST,
	NETWORK_TYPE_NONE
} network_type_t;

/*
 * IP address allocation
 */
struct ipam_range {
	struct in_addr		start;		/* start IP */
	struct in_addr		end;		/* end IP */
	struct in_addr		gateway;	/* gateway */
	uint8_t		prefix_len;	/* CIDR prefix length */
};

struct ipam_range6 {
	struct in6_addr	start;
	struct in6_addr	end;
	struct in6_addr	gateway;
	uint8_t		prefix_len;
};

/*
 * Network configuration
 */
struct network_config {
	char		*name;		/* network name */
	char		*id;		/* network UUID */
	network_type_t	type;		/* bridge, routed, overlay, etc. */
	char		*driver;	/* driver name */
	char		*bridge;	/* bridge interface name */
	char		*mtu;		/* MTU */
	bool		ipv6_enabled;
	bool		internal;	/* internal network */
	bool		attachable;

	/* IPAM settings */
	char		*ipam_driver;	/* default, host-local, dhcp */
	struct ipam_range	ipv4_range;
	struct ipam_range6	ipv6_range;
	char		*gateway;	/* gateway IP */
	char		*subnet;	/* subnet in CIDR notation */

	/* DNS settings */
	char		**dns_servers;
	int		ndns;

	/* Options */
	char		**options;
	char		**labels;
};

/*
 * Network namespace
 */
struct network_namespace {
	char		*name;
	char		*path;		/* /var/run/netns/<name> */
};

/*
 * Endpoint (container network interface)
 */
struct endpoint {
	char		*id;
	char		*network_id;
	char		*container_id;
	char		*interface_name;
	char		*mac_address;
	char		*address;	/* IPv4 */
	char		*address_v6;	/* IPv6 */
	char		*gateway;
	char		*gateway_v6;
};

/*
 * VNET jail network setup
 */
struct vnet_config {
	bool		vnet_enabled;
	char		*epair_prefix;	/* epair naming prefix */
	bool		bridge_to;	/* bridge to parent interface */
	char		*bridge_name;	/* which bridge */
	bool		nat_enable;	/* enable NAT for this jail */
	char		*nat_interface;	/* which interface for NAT */
};

/*
 * Network initialization and cleanup
 */
int	 network_init(void);

/*
 * Run a command (NULL-terminated argv) via fork/exec, capturing stdout
 * into *output (caller frees). No shell — safe for untrusted arguments.
 */
int	 net_capture_argv(char **output, char *const argv[]);
int	 network_cleanup(void);

/*
 * Network management
 */
int	 network_create(struct network_config *config);
int	 network_delete(const char *network_id);
int	 network_connect(const char *network_id, const char *container_id,
	     struct endpoint **ep);
int	 network_disconnect(const char *network_id, const char *container_id);
struct network_config *network_get(const char *network_id);
int	 network_list(struct network_config ***networks, int *nnetworks);
int	 network_inspect(const char *network_id, char **json_output);

/*
 * Bridge management
 */
int	 bridge_create(const char *name);
int	 bridge_delete(const char *name);
int	 bridge_add_interface(const char *bridge, const char *iface);
int	 bridge_remove_interface(const char *bridge, const char *iface);
int	 bridge_set_mtu(const char *bridge, int mtu);
int	 bridge_list_interfaces(const char *bridge, char ***interfaces,
	     int *ninterfaces);
bool	 bridge_exists(const char *name);

/*
 * epair management
 */
int	 epair_create(const char *prefix, char **side_a, char **side_b);
int	 epair_peer_name(const char *aname, char *buf, size_t buflen);
int	 epair_delete(const char *epair);
int	 epair_set_mtu(const char *epair, int mtu);
bool	 epair_exists(const char *epair);

/*
 * VNET management for jails
 */
int	 vnet_create_jail(const char *jail_name, struct vnet_config *config);
int	 vnet_wire_jail(int jid, const char *ip4cidr, const char *gw4,
	     const char *bridge, char **out_side_a);
int	 vnet_delete_jail(const char *jail_name);
int	 vnet_attach_interface(const char *jail_name, const char *iface);
int	 vnet_detach_interface(const char *jail_name, const char *iface);
int	 vnet_get_interfaces(const char *jail_name, char ***interfaces,
	     int *ninterfaces);

/*
 * IP address management
 */
int	 ipam_alloc(struct ipam_range *range, struct in_addr *addr);
int	 ipam_release(struct ipam_range *range, struct in_addr *addr);
int	 ipam_alloc6(struct ipam_range6 *range, struct in6_addr *addr);
int	 ipam_release6(struct ipam_range6 *range, struct in6_addr *addr);

/*
 * NAT setup
 */
int	 nat_enable(const char *jail_name, const char *external_iface);
int	 nat_disable(const char *jail_name);
int	 nat_check(void);

/*
 * DNS configuration
 */
int	 dns_add_server(const char *network_id, const char *server);
int	 dns_remove_server(const char *network_id, const char *server);
int	 dns_set_resolver(const char *jail_name, char **servers, int nservers);

/*
 * CNI integration
 */
int	 cni_add(const char *network_name, const char *container_id,
	     const char *interface_name, char **result_json);
int	 cni_del(const char *network_name, const char *container_id,
	     const char *interface_name);
int	 cni_check(const char *network_name);

/*
 * Network monitoring
 */
int	 network_stats(const char *network_id, uint64_t *rx_bytes,
	     uint64_t *tx_bytes);
int	 endpoint_stats(const char *endpoint_id, uint64_t *rx_bytes,
	     uint64_t *tx_bytes);

#endif /* _OCIFBSD_NETWORK_H */
