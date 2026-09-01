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
 * FreeBSD OCI Runtime - Bridge networking implementation
 */

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

#include <net/if.h>
#include <netinet/in.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "network.h"

/* run_cmd lives in network.c (static there); forward-declare it.
 * net_capture_argv is declared in network.h. */
int run_cmd(int argc, ...);

/*
 * Bridge networking implementation
 *
 * This module handles bridge-based container networking:
 * - Create/manages bridge interfaces
 * - Connect containers via epairs
 * - VLAN and STP configuration
 */

/*
 * Bridge-specific configuration
 */
struct bridge_config {
	char		*name;
	bool		stp_enabled;
	uint16_t	stp_priority;
	uint8_t	*stp_ports;	/* STP port priorities */
	int		nports;
	bool		vlan_filtering;
};

/*
 * Create a bridge with custom configuration
 */
static int
bridge_create_advanced(const char *name, struct bridge_config *config)
{
	int ret;

	/* Create the bridge */
	ret = run_cmd(4, "ifconfig", "bridge", "create", (char *)name);
	if (ret != 0)
		return (-1);

	/* Enable STP if requested */
	if (config && config->stp_enabled) {
		char stp_name[64];
		snprintf(stp_name, sizeof(stp_name), "bridge%s", name);
		run_cmd(5, "ifconfig", stp_name, "stp", (char *)name, "on");
	}

	/* Bring up the bridge */
	ret = run_cmd(3, "ifconfig", (char *)name, "up");

	return (ret);
}

/*
 * Configure VLAN filtering on a bridge
 */
static int
bridge_set_vlan_filtering(const char *bridge, bool enable)
{

	if (enable) {
		run_cmd(3, "sysctl", "net.link.bridge.pfil_onlyip=0");
	}

	return (0);
}

/*
 * Add a tagged VLAN interface to a bridge
 */
static int
bridge_add_vlan(const char *bridge, const char *parent, int vlan_id)
{
	char vlan_if[64];

	snprintf(vlan_if, sizeof(vlan_if), "%s.%d", parent, vlan_id);

	/* Create VLAN interface */
	if (run_cmd(5, "ifconfig", vlan_if, "vlan", vlan_id, (char *)parent) != 0)
		return (-1);

	/* Add to bridge */
	if (run_cmd(5, "ifconfig", (char *)bridge, "addm", vlan_if) != 0)
		return (-1);

	return (0);
}

/*
 * Get bridge forwarding database (FDB) entries
 */
static int
bridge_get_fdb(const char *bridge, char ***entries, int *nentries)
{
	char **list = NULL;
	int count = 0;

	*entries = NULL;
	*nentries = 0;

	/*
	 * Get FDB entries via `ifconfig <bridge>` captured without a shell
	 * (bridge name is untrusted). Filter lines in C instead of `| grep`.
	 */
	char *out = NULL;
	char *argv[] = { "ifconfig", (char *)bridge, NULL };
	if (net_capture_argv(&out, argv) != 0 || out == NULL) {
		free(out);
		return (-1);
	}

	char *save = NULL;
	char *line = strtok_r(out, "\n", &save);
	while (line != NULL) {
		if (strstr(line, "00:00:00:00:00:00") == NULL) {
			char **grown = realloc(list,
			    (count + 1) * sizeof(char *));
			if (grown != NULL) {
				list = grown;
				list[count++] = strdup(line);
			}
		}
		line = strtok_r(NULL, "\n", &save);
	}
	free(out);

	*entries = list;
	*nentries = count;

	return (0);
}

/*
 * Add static FDB entry
 */
static int
bridge_add_static_fdb(const char *bridge, const char *mac, const char *iface)
{
	return run_cmd(5, "ifconfig", (char *)bridge, "addf", (char *)mac,
	    (char *)iface);
}

/*
 * Flush FDB entries
 */
static int
bridge_flush_fdb(const char *bridge, bool static_only)
{
	if (static_only)
		return run_cmd(4, "ifconfig", (char *)bridge, "flushtab");
	return run_cmd(4, "ifconfig", (char *)bridge, "flush");
}

/*
 * Get bridge port statistics
 */
static int
bridge_get_port_stats(const char *bridge, const char *port,
    uint64_t *rx_packets, uint64_t *tx_packets,
    uint64_t *rx_bytes, uint64_t *tx_bytes)
{
	char *out = NULL;

	*rx_packets = *tx_packets = *rx_bytes = *tx_bytes = 0;

	/* Get interface statistics without a shell (port name untrusted). */
	char *argv[] = { "netstat", "-I", (char *)port, "-b", "-w", "1",
	    "-h", "2", NULL };
	if (net_capture_argv(&out, argv) != 0 || out == NULL) {
		free(out);
		return (-1);
	}

	/* Parse output - first line is header, second is data. */
	char *save = NULL;
	char *line = strtok_r(out, "\n", &save);
	if (line != NULL)
		line = strtok_r(NULL, "\n", &save);	/* second: data */
	if (line != NULL) {
		sscanf(line, "%*s %llu %llu %llu %llu",
		    (unsigned long long *)rx_packets,
		    (unsigned long long *)tx_packets,
		    (unsigned long long *)rx_bytes,
		    (unsigned long long *)tx_bytes);
	}

	free(out);

	return (0);
}

/*
 * Set bridge priority (for spanning tree)
 */
static int
bridge_set_priority(const char *bridge, uint16_t priority)
{
	char prio[16];
	snprintf(prio, sizeof(prio), "%u", priority);
	return run_cmd(4, "ifconfig", (char *)bridge, "maxage", prio);
}

/*
 * Set bridge forward delay
 */
static int
bridge_set_forward_delay(const char *bridge, uint16_t delay)
{
	char dly[16];
	snprintf(dly, sizeof(dly), "%u", delay);
	return run_cmd(4, "ifconfig", (char *)bridge, "fwddelay", dly);
}

/*
 * Set bridge hello time
 */
static int
bridge_set_hello_time(const char *bridge, uint16_t hello)
{
	char h[16];
	snprintf(h, sizeof(h), "%u", hello);
	return run_cmd(4, "ifconfig", (char *)bridge, "hellotime", h);
}
