/*-
 * Copyright (c) 2024 The FreeBSD Foundation
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by Klara, Inc. under sponsorship
 * from the FreeBSD Foundation.
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
 * FreeBSD OCI Runtime - MAC (Mandatory Access Control)
 */

#include <sys/param.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "mac.h"

/*
 * Security policy presets
 */
struct security_policy mac_policies[] = {
	{
		.name = "default",
		.description = "Basic container isolation with no MAC enforcement",
		.mac_type = MAC_TYPE_NONE,
		.seccomp_enabled = false,
		.default_seccomp_action = "allow"
	},
	{
		.name = "biba-low",
		.description = "Biba integrity model, low watermark (read-only for most data)",
		.mac_type = MAC_TYPE_BIBA,
		.seccomp_enabled = false,
		.default_seccomp_action = "allow"
	},
	{
		.name = "biba-equal",
		.description = "Biba integrity model, equal (no write up or down)",
		.mac_type = MAC_TYPE_BIBA,
		.seccomp_enabled = false,
		.default_seccomp_action = "allow"
	},
	{
		.name = "mls-low",
		.description = "MLS, low security level",
		.mac_type = MAC_TYPE_MLS,
		.seccomp_enabled = false,
		.default_seccomp_action = "allow"
	},
	{
		.name = "partition",
		.description = "Partition visibility only (no MAC enforcement)",
		.mac_type = MAC_TYPE_PARTITION,
		.seccomp_enabled = false,
		.default_seccomp_action = "allow"
	}
};

/*
 * Check if MAC framework is available
 */
int
mac_check_available(void)
{
	int mac_available;

	size_t len = sizeof(mac_available);
	if (sysctlbyname("security.mac.enabled", &mac_available, &len, NULL, 0) != 0) {
		return (0);  /* MAC not available */
	}

	return (mac_available);
}

/*
 * Initialize MAC subsystem
 */
int
mac_init(void)
{
	if (!mac_check_available()) {
		fprintf(stderr, "warning: MAC framework not available\n");
		return (-1);
	}

	return (0);
}

/*
 * List available MAC policies
 */
int
mac_list_policies(char ***policies, int *npolicies)
{
	char **list = NULL;
	int count = 0;
	char buf[256];
	FILE *fp;

	/* Get list of loaded MAC policies */
	fp = popen("sysctl security.mac", "r");
	if (fp == NULL)
		return (-1);

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		if (strstr(buf, "policy") && strstr(buf, "loaded")) {
			/* Extract policy name */
			char *p = strchr(buf, '=');
			if (p) {
				char *end;
				char **new_list;
				p++;
				end = p + strlen(p) - 1;
				while (end > p && (*end == '\n' || *end == ' '))
					*end-- = '\0';

				new_list = realloc(list, (count + 1) * sizeof(char *));
				if (new_list == NULL) continue;
				list = new_list;
				list[count++] = strdup(p);
			}
		}
	}

	pclose(fp);

	*policies = list;
	*npolicies = count;

	return (0);
}

/*
 * Load a MAC policy
 */
int
mac_load_policy(const char *policy_name)
{
	char cmd[256];
	int ret;

	snprintf(cmd, sizeof(cmd), "kldload mac_%s", policy_name);
	ret = system(cmd);

	/* Also try loading via module */
	if (ret != 0) {
		snprintf(cmd, sizeof(cmd), "kldload %s", policy_name);
		ret = system(cmd);
	}

	return (ret);
}

/*
 * Unload a MAC policy
 */
int
mac_unload_policy(const char *policy_name)
{
	char cmd[256];

	snprintf(cmd, sizeof(cmd), "kldunload mac_%s", policy_name);
	return (system(cmd));
}

/*
 * Set MAC label on a jail
 */
int
mac_set_label(const char *jail_name, struct mac_label *label)
{
	char cmd[512];
	int ret;

	if (label == NULL || label->label == NULL) {
		errno = EINVAL;
		return (-1);
	}

	snprintf(cmd, sizeof(cmd), "jail -j %s label=%s", jail_name, label->label);
	ret = system(cmd);

	return (ret);
}

/*
 * Get MAC label for a jail
 */
int
mac_get_label(const char *jail_name, struct mac_label **label)
{
	char cmd[256];
	struct mac_label *l;

	*label = NULL;

	snprintf(cmd, sizeof(cmd), "jail -j %s -v | grep label", jail_name);

	FILE *fp = popen(cmd, "r");
	if (fp == NULL)
		return (-1);

	char buf[256];
	if (fgets(buf, sizeof(buf), fp) == NULL) {
		pclose(fp);
		return (-1);
	}
	pclose(fp);

	/* Parse label from output */
	char *p = strchr(buf, '=');
	if (p == NULL)
		return (-1);
	p++;

	char *end = p + strlen(p) - 1;
	while (end > p && (*end == '\n' || *end == ' '))
		*end-- = '\0';

	l = mac_label_alloc();
	if (l == NULL)
		return (-1);

	mac_label_parse(p, &l);
	*label = l;

	return (0);
}

/*
 * Remove MAC label from a jail
 */
int
mac_remove_label(const char *jail_name)
{
	/* MAC labels are removed by setting an empty label */
	char cmd[256];

	snprintf(cmd, sizeof(cmd), "jail -j %s label=\"\"", jail_name);
	return (system(cmd));
}

/*
 * Allocate MAC label structure
 */
struct mac_label *
mac_label_alloc(void)
{
	struct mac_label *label;

	label = calloc(1, sizeof(*label));
	if (label == NULL)
		return (NULL);

	label->type = MAC_TYPE_NONE;

	return (label);
}

/*
 * Free MAC label structure
 */
void
mac_label_free(struct mac_label *label)
{
	if (label == NULL)
		return;

	free(label->label);
	free(label->hex_label);
	free(label->biba_effective);
	free(label->biba_range_low);
	free(label->biba_range_high);
	free(label->mls_level);
	free(label->mls_range_low);
	free(label->mls_range_high);
	free(label);
}

/*
 * Parse MAC label string
 */
int
mac_label_parse(const char *label_str, struct mac_label **label)
{
	struct mac_label *l;
	char *copy, *p, *save;

	if (label_str == NULL || label_str[0] == '\0') {
		*label = NULL;
		return (0);
	}

	l = mac_label_alloc();
	if (l == NULL)
		return (-1);

	l->label = strdup(label_str);

	/* Parse Biba labels: biba/effective=<low>:<high> */
	if (strncmp(label_str, "biba/", 5) == 0) {
		l->type = MAC_TYPE_BIBA;
		copy = strdup(label_str + 5);

		p = strtok_r(copy, ",", &save);
		while (p != NULL) {
			if (strncmp(p, "effective=", 10) == 0) {
				l->biba_effective = strdup(p + 10);
			} else if (strncmp(p, "range=", 6) == 0) {
				char *range = p + 6;
				char *colon = strchr(range, ':');
				if (colon) {
					l->biba_range_low = strndup(range, colon - range);
					l->biba_range_high = strdup(colon + 1);
				}
			}
			p = strtok_r(NULL, ",", &save);
		}
		free(copy);
	}
	/* Parse MLS labels: mls/level=<low>:<high> */
	else if (strncmp(label_str, "mls/", 4) == 0) {
		l->type = MAC_TYPE_MLS;
		copy = strdup(label_str + 4);

		p = strtok_r(copy, ",", &save);
		while (p != NULL) {
			if (strncmp(p, "level=", 6) == 0) {
				l->mls_level = strdup(p + 6);
			} else if (strncmp(p, "range=", 6) == 0) {
				char *range = p + 6;
				char *colon = strchr(range, ':');
				if (colon) {
					l->mls_range_low = strndup(range, colon - range);
					l->mls_range_high = strdup(colon + 1);
				}
			}
			p = strtok_r(NULL, ",", &save);
		}
		free(copy);
	}

	*label = l;
	return (0);
}

/*
 * Convert MAC label to string
 */
int
mac_label_to_string(struct mac_label *label, char **str)
{
	char buf[256];

	if (label == NULL) {
		*str = strdup("");
		return (0);
	}

	switch (label->type) {
	case MAC_TYPE_BIBA:
		snprintf(buf, sizeof(buf), "biba/effective=%s:%s",
		    label->biba_range_low ? label->biba_range_low : "0",
		    label->biba_range_high ? label->biba_range_high : "0");
		break;
	case MAC_TYPE_MLS:
		snprintf(buf, sizeof(buf), "mls/level=%s",
		    label->mls_level ? label->mls_level : "low");
		break;
	default:
		snprintf(buf, sizeof(buf), "none");
		break;
	}

	*str = strdup(buf);
	return (0);
}

/*
 * Create Biba label
 */
int
mac_label_from_biba(int low, int high, struct mac_label **label)
{
	struct mac_label *l;

	l = mac_label_alloc();
	if (l == NULL)
		return (-1);

	l->type = MAC_TYPE_BIBA;

	asprintf(&l->biba_effective, "%d:%d", low, high);
	asprintf(&l->biba_range_low, "%d", low);
	asprintf(&l->biba_range_high, "%d", high);
	asprintf(&l->label, "biba/effective=%d:%d", low, high);

	*label = l;
	return (0);
}

/*
 * Create MLS label
 */
int
mac_label_from_mls(const char *low, const char *high, struct mac_label **label)
{
	struct mac_label *l;

	l = mac_label_alloc();
	if (l == NULL)
		return (-1);

	l->type = MAC_TYPE_MLS;
	l->mls_level = strdup(low);
	l->mls_range_low = strdup(low);
	l->mls_range_high = strdup(high ? high : low);

	asprintf(&l->label, "mls/level=%s:%s", low, high ? high : low);

	*label = l;
	return (0);
}

/*
 * Compare two MAC labels
 */
bool
mac_label_compare(struct mac_label *a, struct mac_label *b)
{
	if (a == NULL && b == NULL)
		return (true);
	if (a == NULL || b == NULL)
		return (false);
	if (a->type != b->type)
		return (false);

	switch (a->type) {
	case MAC_TYPE_BIBA:
		if (a->biba_effective == NULL || b->biba_effective == NULL)
			return (false);
		return (strcmp(a->biba_effective, b->biba_effective) == 0);
	case MAC_TYPE_MLS:
		if (a->mls_level == NULL || b->mls_level == NULL)
			return (false);
		return (strcmp(a->mls_level, b->mls_level) == 0);
	default:
		return (true);
	}
}

/*
 * Seccomp operations (placeholder - requires capsicum translation)
 *
 * seccomp(2) is Linux-only. FreeBSD uses capsicum(4) for capability-
 * based sandboxing. The functions below are stubs that document the
 * API surface; the real implementation should translate seccomp
 * profiles to capsicum capabilities.
 *
 * See MIGRATION.md for the full translation plan. Until the
 * translation is implemented, containers have NO syscall filtering
 * (a misbehaving container can use any syscall).
 *
 * API surface preserved so callers compile and link; behavior is
 * "do nothing, return success" until the capsicum implementation
 * replaces these stubs.
 */
int
seccomp_load_profile(const char *profile_path)
{
	/* TODO(seccomp→capsicum): translate profile to capabilities */
	(void)profile_path;
	fprintf(stderr, "warning: seccomp not yet implemented (see MIGRATION.md)\n");
	return (0);
}

int
seccomp_create_jail_filter(const char *jail_name, struct seccomp_profile *profile)
{
	/* TODO(seccomp→capsicum): cap_enter() + cap_rights_init() per rule */
	(void)jail_name;
	(void)profile;
	return (0);
}

int
seccomp_remove_filter(const char *jail_name)
{
	/* TODO(seccomp→capsicum): capsicum has no per-filter removal */
	(void)jail_name;
	return (0);
}

int
seccomp_get_syscall_list(char ***syscalls, int *nsyscalls)
{
	/* TODO(seccomp→capsicum): no syscall list; use capsicum capabilities */
	*syscalls = NULL;
	*nsyscalls = 0;
	return (0);
}

/*
 * Parse OCI security context
 */
int
mac_parse_oci_security(struct security_context **ctx, const char *oci_json)
{
	struct security_context *c;

	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return (-1);

	/* TODO(oci-security): parse oci_json for seccomp profile path,
	 * capabilities, noNewPrivileges, etc. and populate ctx.
	 * Once seccomp→capsicum is implemented, this will also create
	 * the capsicum capability set. */

	*ctx = c;
	return (0);
}

int
mac_free_security(struct security_context *ctx)
{
	if (ctx == NULL)
		return (0);

	free(ctx->seclabel);
	free(ctx->apparmor_profile);
	free(ctx->seccomp_profile);
	mac_label_free(ctx->mac_label);
	free(ctx);

	return (0);
}

/*
 * Get predefined security policies
 */
int
mac_get_policies(struct security_policy **policies, int *npolicies)
{
	*policies = mac_policies;
	*npolicies = sizeof(mac_policies) / sizeof(mac_policies[0]);

	return (0);
}
