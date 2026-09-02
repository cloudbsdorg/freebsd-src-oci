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
 * PAM authentication implementation
 * Phase 13: PAM/System Credentials
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/sysctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <grp.h>
#include <libutil.h>
#include <login_cap.h>
#include <security/pam_appl.h>
#include <security/pam_modules.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sha256.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <time.h>

#include "pam_auth.h"
#include "../include/ocifbsd.h"

/*
 * HMAC-SHA256 wrapper. FreeBSD 16's libmd does not export
 * hmac_sha256() in any public header; the standard location is
 * OpenSSL's <openssl/hmac.h>. This thin wrapper matches the
 * libmd-style call signature used throughout this file.
 */
static void
hmac_sha256(const void *key, size_t key_len, const void *data,
	size_t data_len, void *digest, size_t digest_size)
{
	unsigned int digest_len;

	(void)digest_size;
	(void)HMAC(EVP_sha256(), key, (int)key_len, data, data_len,
	    digest, &digest_len);
}

static const char base64_alphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int
base64_encode(const void *src, size_t src_len, char *dst, size_t dst_size)
{
	const unsigned char *in = src;
	size_t o = 0, i, needed;

	needed = 4 * ((src_len + 2) / 3) + 1;
	if (dst_size < needed)
		return (-1);

	for (i = 0; i + 3 <= src_len; i += 3) {
		dst[o++] = base64_alphabet[(in[i] >> 2) & 0x3f];
		dst[o++] = base64_alphabet[((in[i] << 4) | (in[i+1] >> 4)) & 0x3f];
		dst[o++] = base64_alphabet[((in[i+1] << 2) | (in[i+2] >> 6)) & 0x3f];
		dst[o++] = base64_alphabet[in[i+2] & 0x3f];
	}
	if (i < src_len) {
		dst[o++] = base64_alphabet[(in[i] >> 2) & 0x3f];
		if (i + 1 < src_len) {
			dst[o++] = base64_alphabet[((in[i] << 4) |
			    (in[i+1] >> 4)) & 0x3f];
			dst[o++] = base64_alphabet[(in[i+1] << 2) & 0x3f];
			dst[o++] = '=';
		} else {
			dst[o++] = base64_alphabet[(in[i] << 4) & 0x3f];
			dst[o++] = '=';
			dst[o++] = '=';
		}
	}
	dst[o] = '\0';
	return (0);
}

static int
base64_decode(const char *src, void *dst, size_t dst_size)
{
	unsigned char *out = dst;
	size_t o = 0;
	uint32_t buf = 0;
	int bits = 0;
	const char *p;

	for (p = src; *p != '\0' && *p != '='; p++) {
		const char *q = strchr(base64_alphabet, *p);
		if (q == NULL)
			return (-1);
		buf = (buf << 6) | (uint32_t)(q - base64_alphabet);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (o >= dst_size)
				return (-1);
			out[o++] = (unsigned char)((buf >> bits) & 0xff);
		}
	}
	return ((int)o);
}

static int mkdirp_local(const char *path, mode_t mode);
static bool user_in_group(const char *username, const char *groupname);
static struct ocifbsd_user *ocifbsd_pam_get_user_locked(const char *username);
int pam_create_user_locked(struct ocifbsd_user *user);
int pam_auth_password(const char *username, const char *password);
static int save_user_state(struct ocifbsd_user *user, const char *path);
int user_compare(struct ocifbsd_user *a, struct ocifbsd_user *b);
int group_compare(struct group_role_map *a, struct group_role_map *b);

/*
 * Recursive mkdir(2). FreeBSD 16's <libutil.h> does not export mkdirp
 * in any public header. We provide a local copy.
 */
static int
mkdirp_local(const char *path, mode_t mode)
{
	char buf[PATH_MAX];
	char *p;
	size_t len;

	if (path == NULL || *path == '\0')
		return (-1);

	len = strlcpy(buf, path, sizeof(buf));
	if (len >= sizeof(buf))
		return (-1);

	for (p = buf + 1; *p != '\0'; p++) {
		if (*p != '/')
			continue;
		*p = '\0';
		if (mkdir(buf, mode) != 0 && errno != EEXIST)
			return (-1);
		*p = '/';
	}

	if (mkdir(buf, mode) != 0 && errno != EEXIST)
		return (-1);
	return (0);
}

#define	mkdirp(path, mode)	mkdirp_local((path), (mode))

/* Global state */
static struct user_tree user_registry;
static struct group_role_map_tree group_map;
static struct role_binding *role_bindings = NULL;
static int num_role_bindings = 0;
static int initialized = 0;
static pthread_mutex_t auth_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t rate_limit_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pam_config default_config;
static pam_conv_func_t conv_func = NULL;
static FILE *audit_fp = NULL;

/* Rate limiting table */
struct rate_limit_entry {
	char            addr[64];
	int             attempts;
	time_t          first_attempt;
	time_t          last_attempt;
	SLIST_ENTRY(rate_limit_entry) next;
};
static SLIST_HEAD(, rate_limit_entry) rate_limit_list;

/* Default RBAC role permissions */
static const struct role_permissions role_perms[NUM_ROLES] = {
	{ ROLE_CLUSTER_ADMIN, PERM_READ | PERM_WRITE | PERM_DELETE | PERM_EXEC | PERM_ADMIN | PERM_AUDIT,
	  "Full cluster administration" },
	{ ROLE_ADMIN, PERM_READ | PERM_WRITE | PERM_DELETE | PERM_EXEC | PERM_ADMIN,
	  "Namespace administration" },
	{ ROLE_EDITOR, PERM_READ | PERM_WRITE | PERM_EXEC,
	  "Can create and modify resources" },
	{ ROLE_VIEWER, PERM_READ,
	  "Read-only access" },
	{ ROLE_OPERATOR, PERM_READ | PERM_EXEC,
	  "Can execute commands, read resources" },
	{ ROLE_AUDITOR, PERM_READ | PERM_AUDIT,
	  "Read access with audit log" }
};

/*
 * Initialize PAM authentication
 */
int
pam_auth_init(void)
{
	FILE *fp;
	char path[PATH_MAX];
	char buf[256];

	if (__sync_fetch_and_add(&initialized, 0))
		return (0);

	pthread_mutex_lock(&auth_lock);

	if (initialized) {
		pthread_mutex_unlock(&auth_lock);
		return (0);
	}

	/* Initialize registries */
	RB_INIT(&user_registry);
	RB_INIT(&group_map);
	SLIST_INIT(&rate_limit_list);

	/* Initialize default configuration */
	memset(&default_config, 0, sizeof(default_config));
	default_config.enable_pam = true;
	default_config.enable_group_map = true;
	default_config.enable_pubkey = true;
	default_config.max_failed_attempts = 5;
	default_config.lockout_duration = 300;  /* 5 minutes */
	default_config.token_lifetime = 86400;  /* 24 hours */
	default_config.session_timeout = 3600;   /* 1 hour */
	default_config.default_role = strdup(ROLE_VIEWER);
	default_config.pam_service = strdup("ocifbsd");

	/* Load existing users from state directory */
	snprintf(path, sizeof(path), "%s/users", OCIFBSD_STATE_DIR);
	mkdirp(path, 0755);

	/* Open audit log */
	snprintf(path, sizeof(path), "%s/audit.log", OCIFBSD_STATE_DIR);
	audit_fp = fopen(path, "a");
	if (audit_fp == NULL) {
		fprintf(stderr, "Warning: Could not open audit log: %s\n",
			strerror(errno));
	}

	/* Load system users into registry */
	setpwent();
	struct passwd *pw;
	while ((pw = getpwent()) != NULL) {
		/* Skip system users with UID < 1000 */
		if (pw->pw_uid < 1000)
			continue;

		struct ocifbsd_user *user = calloc(1, sizeof(*user));
		if (user == NULL)
			continue;

		strlcpy(user->username, pw->pw_name, sizeof(user->username));
		user->uid = pw->pw_uid;
		strlcpy(user->role, ROLE_VIEWER, sizeof(user->role)); /* Default */
		user->permissions = pam_permissions_for_role(ROLE_VIEWER);
		user->created = time(NULL);

		/* Check if user is in wheel group for admin role */
		if (user_in_group(pw->pw_name, "wheel")) {
			strlcpy(user->role, ROLE_ADMIN, sizeof(user->role));
			user->permissions = pam_permissions_for_role(ROLE_ADMIN);
		}

		RB_INSERT(user_tree, &user_registry, user);
	}
	endpwent();

	/* Load group-to-role mappings */
	snprintf(path, sizeof(path), "%s/group_role_map.conf", OCIFBSD_CONFIG_DIR);
	fp = fopen(path, "r");
	if (fp != NULL) {
		while (fgets(buf, sizeof(buf), fp) != NULL) {
			char *group, *role;
			/* Skip comments and empty lines */
			if (buf[0] == '#' || buf[0] == '\n')
				continue;

			/* Parse "group:role" */
			group = buf;
			role = strchr(buf, ':');
			if (role == NULL)
				continue;
			*role++ = '\0';
			role[strcspn(role, "\n\r")] = '\0';

			struct group_role_map *map = calloc(1, sizeof(*map));
			if (map) {
				strlcpy(map->group_name, group, sizeof(map->group_name));
				strlcpy(map->role, role, sizeof(map->role));
				RB_INSERT(group_role_map_tree, &group_map, map);
			}
		}
		fclose(fp);
	}

	__sync_fetch_and_add(&initialized, 1);
	pthread_mutex_unlock(&auth_lock);

	return (0);
}

/*
 * Shutdown PAM authentication
 */
void
pam_auth_shutdown(void)
{
	struct ocifbsd_user *user;
	struct group_role_map *map;

	if (!initialized)
		return;

	pthread_mutex_lock(&auth_lock);

	/* Free user registry */
	while ((user = RB_MIN(user_tree, &user_registry)) != NULL) {
		RB_REMOVE(user_tree, &user_registry, user);
		free(user);
	}

	/* Free group map */
	while ((map = RB_MIN(group_role_map_tree, &group_map)) != NULL) {
		RB_REMOVE(group_role_map_tree, &group_map, map);
		free(map);
	}

	/* Close audit log */
	if (audit_fp != NULL) {
		fclose(audit_fp);
		audit_fp = NULL;
	}

	initialized = 0;
	pthread_mutex_unlock(&auth_lock);
}

/*
 * Compare users by username
 */
int
user_compare(struct ocifbsd_user *a, struct ocifbsd_user *b)
{
	return (strcmp(a->username, b->username));
}
RB_GENERATE(user_tree, ocifbsd_user, entry, user_compare);

/*
 * Compare group mappings
 */
int
group_compare(struct group_role_map *a, struct group_role_map *b)
{
	return (strcmp(a->group_name, b->group_name));
}
RB_GENERATE(group_role_map_tree, group_role_map, entry, group_compare);

/*
 * Get PAM configuration
 */
struct pam_config *
pam_get_config(void)
{
	return (&default_config);
}

/*
 * Set PAM configuration
 */
int
pam_set_config(struct pam_config *config)
{
	if (config == NULL)
		return (-1);

	pthread_mutex_lock(&auth_lock);

	/* Free old allocated strings */
	free(default_config.default_role);
	free(default_config.pam_service);
	free(default_config.admin_users);
	free(default_config.ldap_uri);
	free(default_config.ldap_base_dn);
	free(default_config.krb_realm);
	free(default_config.totp_issuer);

	default_config = *config;

	/* Duplicate allocated strings */
	default_config.default_role = config->default_role ? strdup(config->default_role) : NULL;
	default_config.pam_service = config->pam_service ? strdup(config->pam_service) : NULL;
	default_config.admin_users = config->admin_users ? strdup(config->admin_users) : NULL;
	default_config.ldap_uri = config->ldap_uri ? strdup(config->ldap_uri) : NULL;
	default_config.ldap_base_dn = config->ldap_base_dn ? strdup(config->ldap_base_dn) : NULL;
	default_config.krb_realm = config->krb_realm ? strdup(config->krb_realm) : NULL;
	default_config.totp_issuer = config->totp_issuer ? strdup(config->totp_issuer) : NULL;

	pthread_mutex_unlock(&auth_lock);

	return (0);
}

/*
 * Check if user is in group
 */
static bool
user_in_group(const char *username, const char *groupname)
{
	int ngroups = 0;
	gid_t groups[NGROUPS_MAX];
	struct passwd *pw;
	struct group *gr;

	if ((pw = getpwnam(username)) == NULL)
		return (false);

	/* Get user's groups */
	if (getgrouplist(username, pw->pw_gid, groups, &ngroups) == -1)
		return (false);

	/* Check each group */
	for (int i = 0; i < ngroups; i++) {
		if ((gr = getgrgid(groups[i])) != NULL) {
			if (strcmp(gr->gr_name, groupname) == 0)
				return (true);
		}
	}

	return (false);
}

/*
 * PAM conversation that answers every password prompt with a fixed password.
 * Used by pam_auth_password() to drive the system PAM stack non-interactively.
 */
struct pam_pw_appdata {
	const char *password;
};

static int
pam_pw_conv(int nmsg, const struct pam_message **msg,
	struct pam_response **resp, void *appdata_ptr)
{
	struct pam_pw_appdata *ad = appdata_ptr;
	struct pam_response *r;
	int i;

	if (nmsg <= 0 || nmsg > PAM_MAX_NUM_MSG || msg == NULL)
		return (PAM_CONV_ERR);
	r = calloc((size_t)nmsg, sizeof(*r));
	if (r == NULL)
		return (PAM_BUF_ERR);
	for (i = 0; i < nmsg; i++) {
		if (msg[i]->msg_style == PAM_PROMPT_ECHO_OFF ||
		    msg[i]->msg_style == PAM_PROMPT_ECHO_ON)
			r[i].resp = strdup(ad->password != NULL ?
			    ad->password : "");
	}
	*resp = r;
	return (PAM_SUCCESS);
}

/*
 * Verify a username/password against the system PAM stack (service "ocifbsd",
 * falling back to PAM's "other"). Previously this was declared and called but
 * never defined, so pam_ocifbsd.so.1 carried an unresolved symbol and the whole
 * interactive-auth path failed. Returns PAM_SUCCESS on success.
 */
int
pam_auth_password(const char *username, const char *password)
{
	struct pam_pw_appdata ad;
	struct pam_conv conv;
	pam_handle_t *pamh = NULL;
	int r;

	if (username == NULL || password == NULL)
		return (PAM_AUTH_ERR);
	ad.password = password;
	conv.conv = pam_pw_conv;
	conv.appdata_ptr = &ad;

	r = pam_start("ocifbsd", username, &conv, &pamh);
	if (r != PAM_SUCCESS)
		return (r);
	r = pam_authenticate(pamh, 0);
	if (r == PAM_SUCCESS)
		r = pam_acct_mgmt(pamh, 0);
	pam_end(pamh, r);
	return (r);
}

/*
 * Authenticate user with PAM
 */
int
pam_authenticate_user(struct pam_auth_request *req,
	struct pam_auth_response *resp)
{
	struct ocifbsd_user *user = NULL;
	struct audit_entry audit;
	int result = PAM_AUTH_SUCCESS;

	memset(resp, 0, sizeof(*resp));

	/* Check rate limit */
	if (pam_rate_limit(req->client_addr ? req->client_addr : "localhost") != 0) {
		resp->result = PAM_AUTH_TOO_MANY_TRIES;
		resp->message = strdup("Too many authentication attempts. Please try again later.");
		return (-1);
	}

	/* First, check if user exists in registry */
	pthread_mutex_lock(&auth_lock);
	user = ocifbsd_pam_get_user_locked(req->username);
	pthread_mutex_unlock(&auth_lock);

	if (user == NULL) {
		/* User doesn't exist - create new user */
		pthread_mutex_lock(&auth_lock);
		user = calloc(1, sizeof(*user));
		if (user) {
			strlcpy(user->username, req->username, sizeof(user->username));
			user->uid = getpwnam(req->username) ? getpwnam(req->username)->pw_uid : 999999;
			user->created = time(NULL);
			user->permissions = pam_permissions_for_role(default_config.default_role);
			strlcpy(user->role, default_config.default_role, sizeof(user->role));
			pam_create_user_locked(user);
		}
		pthread_mutex_unlock(&auth_lock);
	}

	if (user == NULL) {
		result = PAM_AUTH_NO_USER;
		goto audit_log;
	}

	/* Check if user is locked */
	if (pam_is_user_locked(req->username)) {
		result = PAM_AUTH_LOCKED;
		resp->message = strdup("Account is locked. Please try again later.");
		goto audit_log;
	}

	/* Authenticate based on session type */
	switch (req->session_type) {
	case SESSION_INTERACTIVE:
		/* PAM password authentication */
		result = pam_auth_password(req->username, req->password);
		if (result != PAM_SUCCESS) {
			user->failed_count++;
			if (user->failed_count >= (uint32_t)default_config.max_failed_attempts) {
				pam_lock_user(req->username, time(NULL) + default_config.lockout_duration);
				result = PAM_AUTH_LOCKED;
			} else {
				result = PAM_AUTH_INVALID_PASS;
			}
			/*
			 * Do NOT reset the per-IP rate limiter on failure — that deleted
			 * the attacker's counter and made brute force from one address
			 * un-throttled. The limiter is reset only on success (below).
			 */
		} else {
			/* Success - reset failed count */
			user->failed_count = 0;
		}
		break;

	case SESSION_TOKEN:
		/* Token-based authentication */
		if (pam_verify_token(req->token, user->username, &user->permissions) != 0) {
			result = PAM_AUTH_FAILURE;
		}
		break;

	case SESSION_CERTIFICATE:
		/* Certificate-based authentication (mTLS) */
		/* Certificate is verified at TLS layer, just check if user exists */
		result = PAM_AUTH_SUCCESS;
		break;

	case SESSION_SERVICE:
		/* Service account authentication */
		if (user->is_service_account) {
			result = PAM_AUTH_SUCCESS;
		} else {
			result = PAM_AUTH_FAILURE;
		}
		break;
	}

audit_log:
	/* Log audit entry */
	memset(&audit, 0, sizeof(audit));
	audit.timestamp = time(NULL);
	strlcpy(audit.username, req->username ? req->username : "unknown",
		sizeof(audit.username));
	strlcpy(audit.action, "authenticate", sizeof(audit.action));
	snprintf(audit.resource, sizeof(audit.resource), "session/%s",
		req->service ? req->service : "unknown");
	strlcpy(audit.result, result == PAM_AUTH_SUCCESS ? "success" : "failure",
		sizeof(audit.result));
	strlcpy(audit.client_addr, req->client_addr ? req->client_addr : "local",
		sizeof(audit.client_addr));
	pam_log_audit(&audit);

	resp->result = result;

	if (result == PAM_AUTH_SUCCESS) {
		/* Generate JWT token */
		if (pam_generate_token(req->username, user->role, user->permissions,
				default_config.token_lifetime, &resp->token) == 0) {
			resp->expires = time(NULL) + default_config.token_lifetime;
			strlcpy(resp->role, user->role, sizeof(resp->role));
			resp->permissions = user->permissions;
			resp->username = strdup(user->username);

			/* Update last login */
			pthread_mutex_lock(&auth_lock);
			user->last_login = time(NULL);
			user->login_count++;
			pthread_mutex_unlock(&auth_lock);
		} else {
			resp->result = PAM_AUTH_SYSTEM_ERROR;
			resp->message = strdup("Failed to generate authentication token");
		}
	}

	/* Reset rate limit on success */
	if (result == PAM_AUTH_SUCCESS) {
		pam_reset_rate_limit(req->client_addr ? req->client_addr : "localhost");
	}

	return (result == PAM_AUTH_SUCCESS ? 0 : -1);
}

/*
 * Get user from registry (internal, must hold lock)
 */
struct ocifbsd_user *
ocifbsd_pam_get_user_locked(const char *username)
{
	struct ocifbsd_user key;

	if (username == NULL)
		return (NULL);

	strlcpy(key.username, username, sizeof(key.username));
	return (RB_FIND(user_tree, &user_registry, &key));
}

/*
 * Create user in registry (internal, must hold lock)
 */
int
pam_create_user_locked(struct ocifbsd_user *user)
{
	struct ocifbsd_user *existing;

	if (user == NULL || user->username[0] == '\0')
		return (-1);

	existing = ocifbsd_pam_get_user_locked(user->username);
	if (existing != NULL)
		return (-1);  /* User already exists */

	user->created = time(NULL);
	RB_INSERT(user_tree, &user_registry, user);
	return (0);
}

/*
 * Get user by username
 */
struct ocifbsd_user *
ocifbsd_pam_get_user(const char *username)
{
	struct ocifbsd_user *user;

	pthread_mutex_lock(&auth_lock);
	user = ocifbsd_pam_get_user_locked(username);
	pthread_mutex_unlock(&auth_lock);

	return (user);
}

/*
 * List all users
 */
struct ocifbsd_user **
pam_list_users(int *count)
{
	struct ocifbsd_user **users, *user;
	int n = 0;

	users = NULL;
	*count = 0;

	pthread_mutex_lock(&auth_lock);

	RB_FOREACH(user, user_tree, &user_registry) {
		if (ocifbsd_realloc_grow((void **)&users, (n + 1) * sizeof(*users)) != 0)
			break;
		users[n++] = user;
	}

	pthread_mutex_unlock(&auth_lock);

	*count = n;
	return (users);
}

/*
 * Create new user
 */
int
pam_create_user(struct ocifbsd_user *user)
{
	int ret;

	if (user == NULL)
		return (-1);

	pthread_mutex_lock(&auth_lock);
	ret = pam_create_user_locked(user);
	if (ret == 0) {
		/* Save to disk */
		char path[PATH_MAX];
		snprintf(path, sizeof(path), "%s/users/%s.json",
			OCIFBSD_STATE_DIR, user->username);
		save_user_state(user, path);
	}
	pthread_mutex_unlock(&auth_lock);

	return (ret);
}

/*
 * Update user
 */
int
pam_update_user(struct ocifbsd_user *user)
{
	struct ocifbsd_user *existing;

	if (user == NULL || user->username[0] == '\0')
		return (-1);

	pthread_mutex_lock(&auth_lock);

	existing = ocifbsd_pam_get_user_locked(user->username);
	if (existing == NULL) {
		pthread_mutex_unlock(&auth_lock);
		return (-1);
	}

	/* Update fields */
	existing->uid = user->uid;
	existing->permissions = user->permissions;
	existing->is_service_account = user->is_service_account;
	existing->token_expires = user->token_expires;

	if (user->role[0] != '\0')
		strlcpy(existing->role, user->role, sizeof(existing->role));

	if (user->pubkey_fingerprint[0] != '\0')
		strlcpy(existing->pubkey_fingerprint, user->pubkey_fingerprint,
			sizeof(existing->pubkey_fingerprint));

	/* Save to disk */
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/users/%s.json",
		OCIFBSD_STATE_DIR, user->username);
	save_user_state(existing, path);

	pthread_mutex_unlock(&auth_lock);

	return (0);
}

/*
 * Delete user
 */
int
pam_delete_user(const char *username)
{
	struct ocifbsd_user *user;
	char path[PATH_MAX];

	if (username == NULL)
		return (-1);

	pthread_mutex_lock(&auth_lock);

	user = ocifbsd_pam_get_user_locked(username);
	if (user == NULL) {
		pthread_mutex_unlock(&auth_lock);
		return (-1);
	}

	RB_REMOVE(user_tree, &user_registry, user);

	/* Remove state file */
	snprintf(path, sizeof(path), "%s/users/%s.json", OCIFBSD_STATE_DIR, username);
	unlink(path);

	free(user);
	pthread_mutex_unlock(&auth_lock);

	return (0);
}

/*
 * Set user role
 */
int
pam_set_user_role(const char *username, const char *role)
{
	struct ocifbsd_user *user;
	uint32_t perms;

	if (username == NULL || role == NULL)
		return (-1);

	perms = pam_permissions_for_role(role);
	if (perms == 0)
		return (-1);  /* Invalid role */

	pthread_mutex_lock(&auth_lock);

	user = ocifbsd_pam_get_user_locked(username);
	if (user == NULL) {
		pthread_mutex_unlock(&auth_lock);
		return (-1);
	}

	strlcpy(user->role, role, sizeof(user->role));
	user->permissions = perms;

	/* Save to disk */
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/users/%s.json", OCIFBSD_STATE_DIR, username);
	save_user_state(user, path);

	pthread_mutex_unlock(&auth_lock);

	return (0);
}

/*
 * Save user state to JSON file
 */
static int
save_user_state(struct ocifbsd_user *user, const char *path)
{
	FILE *fp;

	mkdirp(path, 0755);

	fp = fopen(path, "w");
	if (fp == NULL)
		return (-1);

	fprintf(fp, "{\n");
	fprintf(fp, "  \"username\": \"%s\",\n", user->username);
	fprintf(fp, "  \"uid\": %u,\n", user->uid);
	fprintf(fp, "  \"role\": \"%s\",\n", user->role);
	fprintf(fp, "  \"permissions\": %u,\n", user->permissions);
	fprintf(fp, "  \"is_service_account\": %s,\n",
		user->is_service_account ? "true" : "false");
	fprintf(fp, "  \"created\": %ld,\n", (long)user->created);
	fprintf(fp, "  \"last_login\": %ld,\n", (long)user->last_login);
	fprintf(fp, "  \"token_expires\": %ld,\n", (long)user->token_expires);
	fprintf(fp, "  \"login_count\": %u,\n", user->login_count);
	fprintf(fp, "  \"failed_count\": %u,\n", user->failed_count);
	fprintf(fp, "  \"locked_until\": %ld,\n", (long)user->locked_until);
	fprintf(fp, "  \"pubkey_fingerprint\": \"%s\"\n",
		user->pubkey_fingerprint);
	fprintf(fp, "}\n");

	fclose(fp);
	return (0);
}

/*
 * Verify token
 */
int
pam_verify_token(const char *token, const char *expected_user,
	uint32_t *permissions)
{
	char *sub = NULL;
	time_t exp;
	int rc;

	rc = pam_verify_jwt(token, &sub, permissions, &exp);
	if (rc != 0) {
		free(sub);
		return (-1);
	}

	/*
	 * Bind the token to the authenticating identity. pam_verify_jwt only
	 * proves the token is validly signed and unexpired; without this check a
	 * token issued to one user would authenticate a request that claims to be
	 * a different user (the caller supplies expected_user from its own
	 * lookup). Require the token's "sub" claim to equal that identity.
	 */
	if (expected_user == NULL || sub == NULL ||
		strcmp(sub, expected_user) != 0) {
		free(sub);
		return (-1);
	}

	free(sub);
	return (0);
}

/*
 * Refresh token
 */
int
pam_refresh_token(const char *refresh_token, char **new_token)
{
	char *username;
	uint32_t perms;
	struct ocifbsd_user *user;

	if (refresh_token == NULL || new_token == NULL)
		return (-1);

	if (pam_verify_jwt(refresh_token, &username, &perms, NULL) != 0)
		return (-1);

	pthread_mutex_lock(&auth_lock);

	user = ocifbsd_pam_get_user_locked(username);
	if (user == NULL) {
		pthread_mutex_unlock(&auth_lock);
		return (-1);
	}

	/* Generate new token with same permissions */
	int ret = pam_generate_token(username, user->role, perms,
		default_config.token_lifetime, new_token);

	pthread_mutex_unlock(&auth_lock);

	return (ret);
}

/*
 * Get JWT signing secret from one of:
 *   1. OCIFBSD_JWT_SECRET environment variable
 *   2. /etc/ocifbsd/jwt_secret file
 *   3. Randomly generated (lifetime of process)
 *
 * Returns a pointer to a static buffer. NOT thread-safe; intended
 * to be called once at startup. Length returned via secret_len_out.
 */
static const char *
get_jwt_secret(size_t *secret_len_out)
{
	static char secret_buf[128];
	static int initialized = 0;
	const char *env_secret;
	char path[PATH_MAX];
	struct stat st;
	ssize_t n;
	int fd;

	if (initialized) {
		*secret_len_out = strlen(secret_buf);
		return (secret_buf);
	}

	env_secret = getenv("OCIFBSD_JWT_SECRET");
	if (env_secret != NULL && env_secret[0] != '\0') {
		strlcpy(secret_buf, env_secret, sizeof(secret_buf));
		initialized = 1;
		*secret_len_out = strlen(secret_buf);
		return (secret_buf);
	}

	snprintf(path, sizeof(path), "%s/jwt_secret", OCIFBSD_CONFIG_DIR);
	if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
	    st.st_size > 0 && st.st_size < (off_t)sizeof(secret_buf)) {
		fd = open(path, O_RDONLY);
		if (fd >= 0) {
			n = read(fd, secret_buf, sizeof(secret_buf) - 1);
			close(fd);
			if (n > 0) {
				/* Strip trailing newline */
				if (secret_buf[n - 1] == '\n')
					secret_buf[n - 1] = '\0';
				else
					secret_buf[n] = '\0';
				initialized = 1;
				*secret_len_out = strlen(secret_buf);
				return (secret_buf);
			}
		}
	}

	/* Fallback: random per-process secret. */
	arc4random_buf(secret_buf, sizeof(secret_buf) - 1);
	secret_buf[sizeof(secret_buf) - 1] = '\0';
	fprintf(stderr, "WARNING: OCIFBSD_JWT_SECRET not set and %s/jwt_secret "
	    "not found. Using a random per-process secret. Tokens will NOT "
	    "survive a restart and cannot be verified by other processes.\n",
	    OCIFBSD_CONFIG_DIR);
	initialized = 1;
	*secret_len_out = strlen(secret_buf);
	return (secret_buf);
}

/*
 * Generate JWT token
 */
int
pam_generate_token(const char *username, const char *role,
	uint32_t permissions, int lifetime, char **token_out)
{
	char header[256], payload[512], signature[128];
	char header_b64[256], payload_b64[512];
	uint8_t hash[SHA256_DIGEST_LENGTH];
	const char *secret;
	size_t secret_len;

	if (username == NULL || token_out == NULL)
		return (-1);

	secret = get_jwt_secret(&secret_len);

	/* JWT Header */
	snprintf(header, sizeof(header), "{\"alg\":\"HS256\",\"typ\":\"JWT\"}");
	base64_encode(header, strlen(header), header_b64, sizeof(header_b64));

	/* JWT Payload */
	time_t now = time(NULL);
	time_t exp = now + lifetime;
	snprintf(payload, sizeof(payload),
		"{\"sub\":\"%s\",\"role\":\"%s\",\"perms\":%u,\"iat\":%ld,\"exp\":%ld}",
		username, role, permissions, (long)now, (long)exp);
	base64_encode(payload, strlen(payload), payload_b64, sizeof(payload_b64));

	/* HMAC SHA256 Signature */
	hmac_sha256(secret, secret_len, payload_b64, strlen(payload_b64),
		hash, SHA256_DIGEST_LENGTH);
	base64_encode((char *)hash, SHA256_DIGEST_LENGTH, signature, sizeof(signature));

	/* Combine JWT */
	if (asprintf(token_out, "%s.%s.%s", header_b64, payload_b64, signature) == -1)
		return (-1);

	return (0);
}

/*
 * Verify JWT token
 */
int
pam_verify_jwt(const char *jwt, char **username, uint32_t *perms, time_t *exp)
{
	char *token_copy, *header, *payload, *sig;
	char payload_dec[512];
	char expected_sig[128];
	uint8_t hash[SHA256_DIGEST_LENGTH];
	const char *secret;
	size_t secret_len;
	uint32_t p;

	if (jwt == NULL)
		return (-1);

	secret = get_jwt_secret(&secret_len);

	/* Parse JWT */
	token_copy = strdup(jwt);
	header = strtok(token_copy, ".");
	payload = strtok(NULL, ".");
	sig = strtok(NULL, ".");

	if (header == NULL || payload == NULL || sig == NULL) {
		free(token_copy);
		return (-1);
	}

	/* Decode payload and NUL-terminate it: base64_decode returns a byte
	 * count and does NOT terminate, so the strstr() claim scans below would
	 * run past the decoded data into uninitialized stack (OOB read / info
	 * leak). Leave room for the terminator. */
	{
		int plen = base64_decode(payload, payload_dec,
			sizeof(payload_dec) - 1);
		if (plen <= 0) {
			free(token_copy);
			return (-1);
		}
		payload_dec[plen] = '\0';
	}

	/* Parse claims */
	if (username != NULL) {
		char *sub = strstr(payload_dec, "\"sub\":\"");
		if (sub) {
			sub += 7;
			char *end = strchr(sub, '"');
			if (end) {
				*end = '\0';
				*username = strdup(sub);
			}
		}
	}

	if (perms != NULL) {
		char *perms_str = strstr(payload_dec, "\"perms\":");

		if (perms_str && sscanf(perms_str + 8, "%u", &p) == 1)
			*perms = p;
	}

	/*
	 * Always enforce the exp claim if present — expiry is a security check
	 * that must run regardless of whether the caller wants the value back.
	 * (It used to be gated on the exp OUT param being non-NULL, so callers
	 * passing NULL — e.g. the refresh-token path — skipped expiry entirely
	 * and accepted expired tokens.)
	 */
	{
		char *exp_str = strstr(payload_dec, "\"exp\":");
		long exp_val;

		if (exp_str != NULL && sscanf(exp_str + 6, "%ld", &exp_val) == 1) {
			if (exp != NULL)
				*exp = (time_t)exp_val;
			if ((time_t)exp_val < time(NULL)) {
				free(token_copy);
				return (-1);  /* Token expired */
			}
		}
	}

	/* Verify signature */
	hmac_sha256(secret, secret_len, payload, strlen(payload),
		hash, SHA256_DIGEST_LENGTH);
	base64_encode((char *)hash, SHA256_DIGEST_LENGTH, expected_sig, sizeof(expected_sig));

	/*
	 * Constant-time signature comparison — strcmp() short-circuits on the
	 * first differing byte, leaking how much of a forged signature is correct
	 * (a timing oracle). Compare in time independent of the contents.
	 *
	 * NB: sig points into token_copy, so this must run BEFORE freeing it
	 * (the old code compared sig after free(token_copy) — a use-after-free).
	 */
	{
		size_t elen = strlen(expected_sig);
		int bad = (strlen(sig) != elen ||
			timingsafe_bcmp(sig, expected_sig, elen) != 0);

		free(token_copy);
		if (bad)
			return (-1);  /* Invalid signature */
	}

	return (0);
}

/*
 * Get role permissions
 */
uint32_t
pam_permissions_for_role(const char *role)
{
	for (int i = 0; i < NUM_ROLES; i++) {
		if (strcmp(role, role_perms[i].role) == 0)
			return (role_perms[i].permissions);
	}
	return (0);
}

/*
 * Get role from system group
 */
const char *
pam_role_from_group(const char *group)
{
	struct group_role_map key, *map;

	if (group == NULL)
		return (NULL);

	strlcpy(key.group_name, group, sizeof(key.group_name));
	map = RB_FIND(group_role_map_tree, &group_map, &key);

	return (map ? map->role : NULL);
}

/*
 * Add role binding
 */
int
pam_add_role_binding(struct role_binding *binding)
{
	if (binding == NULL || binding->role[0] == '\0')
		return (-1);

	pthread_mutex_lock(&auth_lock);

	num_role_bindings++;
	if (ocifbsd_realloc_grow((void **)&role_bindings, num_role_bindings * sizeof(*binding)) != 0) {
		num_role_bindings--;
		pthread_mutex_unlock(&auth_lock);
		return (-1);
	}

	role_bindings[num_role_bindings - 1] = *binding;
	role_bindings[num_role_bindings - 1].created = time(NULL);
	role_bindings[num_role_bindings - 1].users = binding->users ? strdup(binding->users) : NULL;
	role_bindings[num_role_bindings - 1].groups = binding->groups ? strdup(binding->groups) : NULL;
	role_bindings[num_role_bindings - 1].namespaces = binding->namespaces ? strdup(binding->namespaces) : NULL;

	pthread_mutex_unlock(&auth_lock);

	return (0);
}

/*
 * Remove role binding
 */
int
pam_remove_role_binding(const char *role)
{
	int found = -1;

	if (role == NULL)
		return (-1);

	pthread_mutex_lock(&auth_lock);

	for (int i = 0; i < num_role_bindings; i++) {
		if (strcmp(role_bindings[i].role, role) == 0) {
			found = i;
			break;
		}
	}

	if (found >= 0) {
		free(role_bindings[found].users);
		free(role_bindings[found].groups);
		free(role_bindings[found].namespaces);

		/* Compact array */
		for (int i = found; i < num_role_bindings - 1; i++) {
			role_bindings[i] = role_bindings[i + 1];
		}
		num_role_bindings--;
		(void)ocifbsd_realloc_grow((void **)&role_bindings,
			(num_role_bindings ? num_role_bindings : 1) * sizeof(*role_bindings));
	}

	pthread_mutex_unlock(&auth_lock);

	return (found >= 0 ? 0 : -1);
}

/*
 * List role bindings
 */
struct role_binding **
pam_list_role_bindings(int *count)
{
	struct role_binding **bindings;

	if (count == NULL)
		return (NULL);

	pthread_mutex_lock(&auth_lock);

	bindings = malloc(num_role_bindings * sizeof(*bindings));
	if (bindings) {
		for (int i = 0; i < num_role_bindings; i++) {
			bindings[i] = &role_bindings[i];
		}
		*count = num_role_bindings;
	} else {
		*count = 0;
	}

	pthread_mutex_unlock(&auth_lock);

	return (bindings);
}

/*
 * Log audit entry
 */
int
pam_log_audit(struct audit_entry *entry)
{
	if (entry == NULL || audit_fp == NULL)
		return (-1);

	pthread_mutex_lock(&auth_lock);

	/*
	 * Escape all free-form fields so a crafted username/resource/details
	 * cannot forge audit records via an embedded quote or newline. Serialized
	 * by auth_lock, so the static scratch buffers are safe.
	 */
	{
		static char euser[sizeof(entry->username) * 6];
		static char eaction[sizeof(entry->action) * 6];
		static char eres[sizeof(entry->resource) * 6];
		static char eresult[sizeof(entry->result) * 6];
		static char eclient[sizeof(entry->client_addr) * 6];
		static char edetails[8192];

		fprintf(audit_fp,
			"{\"timestamp\":%ld,\"user\":\"%s\",\"action\":\"%s\","
			"\"resource\":\"%s\",\"result\":\"%s\",\"client\":\"%s\"",
			(long)entry->timestamp,
			ocifbsd_json_escape(entry->username, euser, sizeof(euser)),
			ocifbsd_json_escape(entry->action, eaction, sizeof(eaction)),
			ocifbsd_json_escape(entry->resource, eres, sizeof(eres)),
			ocifbsd_json_escape(entry->result, eresult, sizeof(eresult)),
			ocifbsd_json_escape(entry->client_addr, eclient, sizeof(eclient)));

		if (entry->details)
			fprintf(audit_fp, ",\"details\":\"%s\"",
				ocifbsd_json_escape(entry->details, edetails,
				sizeof(edetails)));
	}

	fprintf(audit_fp, "}\n");
	fflush(audit_fp);

	pthread_mutex_unlock(&auth_lock);

	return (0);
}

/*
 * Query audit log
 */
struct audit_entry **
pam_query_audit(const char *username, time_t start, time_t end, int *count)
{
	struct audit_entry **entries = NULL;
	char buf[1024];
	char path[PATH_MAX];
	FILE *fp;
	int n = 0;

	if (count == NULL)
		return (NULL);

	*count = 0;

	snprintf(path, sizeof(path), "%s/audit.log", OCIFBSD_STATE_DIR);
	fp = fopen(path, "r");
	if (fp == NULL)
		return (NULL);

	while (fgets(buf, sizeof(buf), fp) != NULL) {
		/* Simple JSON parsing - in production, use json_parser */
		char user[256], action[128], result[32];
		long ts;

		if (sscanf(buf, "{\"timestamp\":%ld,\"user\":\"%255[^\"]\","
				"\"action\":\"%127[^\"]\",\"result\":\"%31[^\"]",
				&ts, user, action, result) == 4) {

			if (ts >= start && ts <= end) {
				if (username == NULL || strcmp(user, username) == 0) {
					if (ocifbsd_realloc_grow((void **)&entries,
						(n + 1) * sizeof(*entries)) == 0) {
						entries[n] = calloc(1, sizeof(**entries));
						if (entries[n]) {
							entries[n]->timestamp = ts;
							strlcpy(entries[n]->username, user, sizeof(entries[n]->username));
							strlcpy(entries[n]->action, action, sizeof(entries[n]->action));
							strlcpy(entries[n]->result, result, sizeof(entries[n]->result));
							n++;
						}
					}
				}
			}
		}
	}

	fclose(fp);
	*count = n;
	return (entries);
}

/*
 * Rate limiting
 */
int
pam_rate_limit(const char *client_addr)
{
	struct rate_limit_entry *entry;
	int max_attempts = 20;  /* 20 attempts per minute */
	time_t now = time(NULL);

	pthread_mutex_lock(&rate_limit_lock);

	SLIST_FOREACH(entry, &rate_limit_list, next) {
		if (strcmp(entry->addr, client_addr) == 0) {
			/* Check if expired */
			if (now - entry->first_attempt > 60) {
				/* Reset */
				entry->attempts = 1;
				entry->first_attempt = now;
				entry->last_attempt = now;
				pthread_mutex_unlock(&rate_limit_lock);
				return (0);
			}

			if (entry->attempts >= max_attempts) {
				pthread_mutex_unlock(&rate_limit_lock);
				return (-1);  /* Rate limited */
			}

			entry->attempts++;
			entry->last_attempt = now;
			pthread_mutex_unlock(&rate_limit_lock);
			return (0);
		}
	}

	/* New entry */
	entry = calloc(1, sizeof(*entry));
	if (entry) {
		strlcpy(entry->addr, client_addr, sizeof(entry->addr));
		entry->attempts = 1;
		entry->first_attempt = now;
		entry->last_attempt = now;
		SLIST_INSERT_HEAD(&rate_limit_list, entry, next);
	}

	pthread_mutex_unlock(&rate_limit_lock);

	return (0);
}

/*
 * Reset rate limit
 */
void
pam_reset_rate_limit(const char *client_addr)
{
	struct rate_limit_entry *entry, *prev = NULL;

	pthread_mutex_lock(&rate_limit_lock);

	SLIST_FOREACH(entry, &rate_limit_list, next) {
		if (strcmp(entry->addr, client_addr) == 0) {
			if (prev)
				SLIST_REMOVE_AFTER(prev, next);
			else
				SLIST_REMOVE_HEAD(&rate_limit_list, next);
			free(entry);
			break;
		}
		prev = entry;
	}

	pthread_mutex_unlock(&rate_limit_lock);
}

/*
 * Lock user
 */
int
pam_lock_user(const char *username, time_t until)
{
	struct ocifbsd_user *user;

	if (username == NULL)
		return (-1);

	pthread_mutex_lock(&auth_lock);

	user = ocifbsd_pam_get_user_locked(username);
	if (user == NULL) {
		pthread_mutex_unlock(&auth_lock);
		return (-1);
	}

	user->locked_until = until;
	user->failed_count = 0;  /* Reset failed count on lock */

	pthread_mutex_unlock(&auth_lock);

	return (0);
}

/*
 * Unlock user
 */
int
pam_unlock_user(const char *username)
{
	return (pam_lock_user(username, 0));
}

/*
 * Check if user is locked
 */
bool
pam_is_user_locked(const char *username)
{
	struct ocifbsd_user *user;
	bool locked = false;

	if (username == NULL)
		return (false);

	pthread_mutex_lock(&auth_lock);

	user = ocifbsd_pam_get_user_locked(username);
	if (user != NULL && user->locked_until > time(NULL))
		locked = true;

	pthread_mutex_unlock(&auth_lock);

	return (locked);
}

/*
 * Set conversation function
 */
void
pam_set_conv_func(pam_conv_func_t func)
{
	conv_func = func;
}

/*
 * PAM module entry points. These are the symbols that the PAM
 * framework looks up via dlsym() when it loads pam_ocifbsd.so.1.
 */

static int
map_auth_result(int custom)
{
	switch (custom) {
	case PAM_AUTH_SUCCESS:    return (PAM_SUCCESS);
	case PAM_AUTH_NO_USER:    return (PAM_USER_UNKNOWN);
	case PAM_AUTH_INVALID_PASS:
	case PAM_AUTH_FAILURE:    return (PAM_AUTH_ERR);
	case PAM_AUTH_EXPIRED:    return (PAM_ACCT_EXPIRED);
	case PAM_AUTH_LOCKED:
	case PAM_AUTH_TOO_MANY_TRIES: return (PAM_MAXTRIES);
	case PAM_AUTH_SYSTEM_ERROR:  return (PAM_SYSTEM_ERR);
	default:                  return (PAM_AUTH_ERR);
	}
}

PAM_EXTERN int
pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc,
	const char **argv)
{
	const char *username = NULL;
	const char *password = NULL;
	struct pam_auth_request req;
	struct pam_auth_response resp;
	int pam_err;

	(void)flags;
	(void)argc;
	(void)argv;

	pam_err = pam_get_user(pamh, &username, NULL);
	if (pam_err != PAM_SUCCESS || username == NULL)
		return (PAM_USER_UNKNOWN);

	pam_err = pam_get_item(pamh, PAM_AUTHTOK,
	    (const void **)&password);
	if (pam_err != PAM_SUCCESS)
		password = NULL;

	memset(&req, 0, sizeof(req));
	req.username = username;
	req.password = password;
	req.service = "ocifbsd";
	req.session_type = SESSION_INTERACTIVE;

	memset(&resp, 0, sizeof(resp));
	(void)pam_authenticate_user(&req, &resp);

	pam_err = map_auth_result(resp.result);

	if (resp.token != NULL)
		free(resp.token);
	if (resp.message != NULL)
		free(resp.message);

	return (pam_err);
}

PAM_EXTERN int
pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	(void)pamh;
	(void)flags;
	(void)argc;
	(void)argv;
	return (PAM_SUCCESS);
}

PAM_EXTERN int
pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
	const char *username = NULL;
	struct ocifbsd_user *user;
	int pam_err;

	(void)flags;
	(void)argc;
	(void)argv;

	pam_err = pam_get_user(pamh, &username, NULL);
	if (pam_err != PAM_SUCCESS || username == NULL)
		return (PAM_USER_UNKNOWN);

	user = ocifbsd_pam_get_user(username);
	if (user == NULL)
		return (PAM_USER_UNKNOWN);
	if (pam_is_user_locked(username))
		return (PAM_MAXTRIES);
	return (PAM_SUCCESS);
}
