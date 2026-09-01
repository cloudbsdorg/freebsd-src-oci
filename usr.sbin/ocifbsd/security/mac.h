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
 * FreeBSD OCI Runtime - MAC (Mandatory Access Control)
 */

#ifndef _OCIFBSD_MAC_H
#define _OCIFBSD_MAC_H

#include <sys/types.h>
#include <stdbool.h>

/*
 * MAC framework types supported on FreeBSD
 */
typedef enum {
	MAC_TYPE_BIBA = 0,	/* Biba integrity */
	MAC_TYPE_MLS,		/* Multi-Level Security */
	MAC_TYPE_LOMAC,		/* Low-watermark MAC */
	MAC_TYPE_MACLB,		/* MAC + MAC see Biba/Low-watermark */
	MAC_TYPE_PARTITION,	/* Partition visibility */
	MAC_TYPE_NONE		/* No MAC enforcement */
} mac_type_t;

/*
 * MAC label structure
 */
struct mac_label {
	mac_type_t	type;
	char		*label;		/* full label string */
	char		*hex_label;	/* hex-encoded label */

	/* Biba components */
	char		*biba_effective;
	char		*biba_range_low;
	char		*biba_range_high;

	/* MLS components */
	char		*mls_level;
	char		*mls_range_low;
	char		*mls_range_high;
};

/*
 * OCI Linux security context mapping
 */
struct security_context {
	char	*seclabel;		/* SELinux label */
	char	*apparmor_profile;	/* AppArmor profile */
	char	*seccomp_profile;	/* seccomp profile */

	/* FreeBSD MAC */
	struct mac_label *mac_label;
};

/*
 * OCI Linux syscall whitelist/blacklist
 */
struct seccomp_profile {
	bool		default_action_errno;	/* default action: return errno */
	int		default_errno;		/* errno to return */

	char		**syscall_whitelist;	/* allowed syscalls */
	int		nwhitelist;

	char		**syscall_blacklist;	/* denied syscalls */
	int		nblacklist;

	/* Architectures */
	char		**arch_whitelist;
	int		narch;
};

/*
 * MAC operations
 */
int	 mac_init(void);
int	 mac_check_available(void);
int	 mac_set_label(const char *jail_name, struct mac_label *label);
int	 mac_get_label(const char *jail_name, struct mac_label **label);
int	 mac_remove_label(const char *jail_name);
int	 mac_list_policies(char ***policies, int *npolicies);
int	 mac_load_policy(const char *policy_name);
int	 mac_unload_policy(const char *policy_name);

/*
 * MAC label operations
 */
struct mac_label *mac_label_alloc(void);
void	 mac_label_free(struct mac_label *label);
int	 mac_label_parse(const char *label_str, struct mac_label **label);
int	 mac_label_to_string(struct mac_label *label, char **str);
int	 mac_label_from_biba(int low, int high, struct mac_label **label);
int	 mac_label_from_mls(const char *low, const char *high,
	     struct mac_label **label);
bool	 mac_label_compare(struct mac_label *a, struct mac_label *b);

/*
 * Seccomp operations
 */
int	 seccomp_load_profile(const char *profile_path);
int	 seccomp_create_jail_filter(const char *jail_name,
	     struct seccomp_profile *profile);
int	 seccomp_remove_filter(const char *jail_name);
int	 seccomp_get_syscall_list(char ***syscalls, int *nsyscalls);

/*
 * Parse OCI security context
 */
int	 mac_parse_oci_security(struct security_context **ctx,
	     const char *oci_json);
int	 mac_free_security(struct security_context *ctx);

/*
 * Security policy presets
 */
struct security_policy {
	const char	*name;
	const char	*description;
	mac_type_t	mac_type;
	bool		seccomp_enabled;
	const char	*default_seccomp_action;
};

extern struct security_policy mac_policies[];
int mac_get_policies(struct security_policy **policies, int *npolicies);

#endif /* _OCIFBSD_MAC_H */
