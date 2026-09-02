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
 * FreeBSD OCI Runtime - VNET (Virtual Network Stack) implementation
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <net/if.h>
#include <netinet/in.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int mkdirp(const char *path, mode_t mode);

#include "network.h"

/* run_cmd lives in network.c (static there); forward-declare it.
 * net_capture_argv is declared in network.h. */
int run_cmd(int argc, ...);

/*
 * VNET (Virtual Network Stack) Implementation
 *
 * FreeBSD's VNET feature allows jails to have their own network stack,
 * similar to Linux network namespaces. This module provides:
 * - VNET-enabled jail creation
 * - Interface management within VNET
 * - Routing table management
 */

/*
 * Check if VNET is available and enabled
 */
static bool
vnet_is_available(void)
{
	size_t len;
	int vnet_enabled;

	len = sizeof(vnet_enabled);
	if (sysctlbyname("security.jail.vnet_enabled", &vnet_enabled, &len, NULL, 0) != 0)
		return (false);

	return (vnet_enabled != 0);
}

/*
 * Get VNET status for a jail
 */
static int
vnet_get_jail_status(const char *jail_name, bool *has_vnet, char ***interfaces, int *ninterfaces)
{
	char **ifaces = NULL;
	int count = 0;

	*has_vnet = false;
	*interfaces = NULL;
	*ninterfaces = 0;

	/*
	 * Check if the jail has vnet. Capture `jls -v -j <name>` without a
	 * shell (jail_name is untrusted) and match "vnet" in C rather than
	 * piping through grep.
	 */
	char *out = NULL;
	char *jls_argv[] = { "jls", "-v", "-j", (char *)jail_name, NULL };
	if (net_capture_argv(&out, jls_argv) == 0 && out != NULL) {
		if (strstr(out, "vnet") != NULL)
			*has_vnet = true;
	}
	free(out);
	out = NULL;

	/* Get interfaces via jexec <name> ifconfig -l (no shell). */
	char *ifc_argv[] = { "jexec", (char *)jail_name, "ifconfig", "-l",
	    NULL };
	if (net_capture_argv(&out, ifc_argv) == 0 && out != NULL) {
		char *save, *token;
		token = strtok_r(out, " \t\n", &save);
		while (token) {
			char **grown = realloc(ifaces,
			    (count + 1) * sizeof(char *));
			if (grown == NULL)
				break;
			ifaces = grown;
			ifaces[count++] = strdup(token);
			token = strtok_r(NULL, " \t\n", &save);
		}
	}
	free(out);

	*interfaces = ifaces;
	*ninterfaces = count;

	return (0);
}

/*
 * Configure routing within a VNET jail
 */
static int
vnet_add_route(const char *jail_name, const char *network, const char *gateway)
{
	return run_cmd(7, "jexec", (char *)jail_name, "route", "add", "-net",
	    (char *)network, (char *)gateway);
}

static int
vnet_delete_route(const char *jail_name, const char *network)
{
	return run_cmd(5, "jexec", (char *)jail_name, "route", "delete", "-net",
	    (char *)network);
}

/*
 * Configure firewall within VNET
 */
static int
vnet_configure_pf(const char *jail_name, const char *rules)
{
	char pf_rules_file[PATH_MAX];
	FILE *f;

	snprintf(pf_rules_file, sizeof(pf_rules_file),
	    "/var/run/ocifbsd/jails/%s/etc/pf.conf", jail_name);

	/* Ensure directory exists */
	char dir[PATH_MAX];
	snprintf(dir, sizeof(dir), "/var/run/ocifbsd/jails/%s/etc", jail_name);
	mkdirp(dir, 0755);

	f = fopen(pf_rules_file, "w");
	if (f == NULL)
		return (-1);

	fprintf(f, "%s", rules);
	fclose(f);

	/* Load rules in jail */
	return run_cmd(4, "jexec", (char *)jail_name, "pfctl", "-f",
	    pf_rules_file);
}

/*
 * Set up NAT for VNET jail
 */
static int
vnet_setup_nat(const char *jail_name, const char *external_iface, const char *internal_subnet)
{
	char rules[1024];

	snprintf(rules, sizeof(rules),
	    "nat on %s from %s to any -> (%s)\n"
	    "block in on %s from %s to any\n",
	    external_iface, internal_subnet, external_iface,
	    external_iface, internal_subnet);

	return (vnet_configure_pf(jail_name, rules));
}

/*
 * Get VNET interface statistics
 */
static int
vnet_get_interface_stats(const char *jail_name, const char *interface,
	uint64_t *rx_bytes, uint64_t *tx_bytes, uint64_t *rx_packets, uint64_t *tx_packets)
{
	char buf[256];
	char *out = NULL;

	*rx_bytes = *tx_bytes = *rx_packets = *tx_packets = 0;

	/* Get interface statistics via jexec (no shell; names untrusted). */
	char *ns_argv[] = { "jexec", (char *)jail_name, "netstat", "-I",
	    (char *)interface, "-i", NULL };
	if (net_capture_argv(&out, ns_argv) != 0 || out == NULL) {
		free(out);
		return (-1);
	}

	/* Read the third line (after two header lines). */
	char *save = NULL;
	char *line = strtok_r(out, "\n", &save);
	if (line != NULL)
		line = strtok_r(NULL, "\n", &save);	/* second */
	if (line != NULL)
		line = strtok_r(NULL, "\n", &save);	/* third: data */
	if (line != NULL) {
		strlcpy(buf, line, sizeof(buf));
		/* Parse: Name Mtu Network Address Ibytes Ipkts Obytes Opkts.
		 * Width-limit the %s conversions to avoid overrunning the
		 * fixed field buffers. */
		char name[64], mtu[16], network[64], addr[64];
		sscanf(buf, "%63s %15s %63s %63s %llu %llu %llu %llu",
		    name, mtu, network, addr,
		    (unsigned long long *)rx_bytes,
		    (unsigned long long *)rx_packets,
		    (unsigned long long *)tx_bytes,
		    (unsigned long long *)tx_packets);
	}

	free(out);
	return (0);
}

/*
 * Clone interface into VNET
 */
static int
vnet_clone_interface(const char *jail_name, const char *template_if, const char *new_name)
{
	char cmd[512];

	/* Clone the interface on host */
	snprintf(cmd, sizeof(cmd), "clone %s", new_name);
	if (run_cmd(4, "ifconfig", (char *)template_if, "clone", new_name) != 0)
		return (-1);

	/* Move to jail's VNET */
	return run_cmd(6, "jexec", (char *)jail_name, "ifconfig", new_name,
	    "vnet", (char *)jail_name);
}
