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
 * Authentication and authorization header
 */

#ifndef _OCIFBSD_AUTH_H
#define _OCIFBSD_AUTH_H

#include <sys/tree.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* RBAC roles */
#define ROLE_CLUSTER_ADMIN  "cluster-admin"
#define ROLE_ADMIN         "admin"
#define ROLE_EDITOR        "editor"
#define ROLE_VIEWER        "viewer"
#define ROLE_OPERATOR      "operator"

/* Permission types */
#define PERM_READ   0x01
#define PERM_WRITE  0x02
#define PERM_DELETE 0x04
#define PERM_ADMIN  0x08

/* Resource types */
#define RES_CLUSTER    "cluster"
#define RES_NODE       "node"
#define RES_POD        "pod"
#define RES_SERVICE    "service"
#define RES_STACK      "stack"
#define RES_VOLUME     "volume"
#define RES_SECRET     "secret"
#define RES_NAMESPACE  "namespace"
#define RES_NETWORK    "network"
#define RES_IMAGE      "image"

/* Role binding */
struct role_binding {
	char name[256];
	char namespace[256];
	char role[64];           /* role name */
	char **subjects;          /* users/groups */
	int n_subjects;
	time_t created;
	time_t updated;
	RB_ENTRY(role_binding) entry;
};

RB_HEAD(role_binding_tree, role_binding);
RB_PROTOTYPE(role_binding_tree, role_binding, entry, rb_compare);

/* User identity */
struct user_identity {
	char username[256];
	char **groups;
	int n_groups;
	char **roles;
	int n_roles;
	time_t last_login;
	time_t password_expires;
	bool enabled;
	/*
	 * crypt(3)-format password hash. "*" (or empty) means no password login
	 * is possible (the account authenticates by token/cert only) — never a
	 * match. Set by auth_user_create; checked by auth_authenticate.
	 */
	char password_hash[128];
};

/* Token/session */
struct auth_token {
	char token_id[128];       /* token hash */
	char username[256];
	time_t created;
	time_t expires;
	time_t last_used;
	char remote_addr[64];
	char user_agent[256];
	int permissions;          /* bitmap of PERM_* */
	RB_ENTRY(auth_token) entry;
};

RB_HEAD(token_tree, auth_token);
RB_PROTOTYPE(token_tree, auth_token, entry, token_compare);

/* Secret */
struct secret {
	char name[256];
	char namespace[256];
	char type[64];            /* opaque, registry, tls */
	char *data;               /* encrypted secret data */
	size_t data_len;
	time_t created;
	time_t updated;
	int version;
	RB_ENTRY(secret) entry;
};

RB_HEAD(secret_tree, secret);
RB_PROTOTYPE(secret_tree, secret, entry, secret_compare);

/* Audit log entry */
struct audit_entry {
	uint64_t id;
	time_t timestamp;
	char user[256];
	char action[128];
	char resource[256];
	char resource_name[256];
	char result[32];          /* success, failure, denied */
	char remote_addr[64];
	char details[1024];
	RB_ENTRY(audit_entry) entry;
};

RB_HEAD(audit_tree, audit_entry);
RB_PROTOTYPE(audit_tree, audit_entry, entry, audit_compare);

/* TLS certificate */
struct tls_cert {
	char name[256];
	char type[32];            /* ca, node, api, service */
	char cert_file[PATH_MAX];
	char key_file[PATH_MAX];
	char *cert_pem;
	char *key_pem;
	time_t not_before;
	time_t not_after;
	char issuer[256];
	char subject[256];
	char **sans;              /* subject alternative names */
	int n_sans;
	int version;
};

/* Authentication functions */
int auth_init(void);
int auth_shutdown(void);

/* User management */
int auth_user_create(const char *username, const char *password);
int auth_user_delete(const char *username);
int auth_user_modify(const char *username, const char *password, bool enabled);
struct user_identity *auth_user_get(const char *username);
struct user_identity **auth_users_list(int *count);
int auth_user_set_groups(const char *username, char **groups, int n_groups);

/* Authentication */
int auth_authenticate(const char *username, const char *password);
int auth_authenticate_pam(const char *username, const char *password);
int auth_token_create(const char *username, char *token_out, size_t token_len);
int auth_token_validate(const char *token, struct user_identity *user);
int auth_token_revoke(const char *token);
int auth_token_revoke_user(const char *username);
struct auth_token **auth_tokens_list(const char *username, int *count);

/* Authorization / RBAC */
int auth_role_create(const char *role, int permissions);
int auth_role_delete(const char *role);
int auth_role_grant(const char *username, const char *role, const char *namespace);
int auth_role_revoke(const char *username, const char *role, const char *namespace);
int auth_check_permission(const char *username, const char *resource, int permission);
int auth_binding_create(struct role_binding *binding);
int auth_binding_delete(const char *name, const char *namespace);
struct role_binding **auth_bindings_list(const char *namespace, int *count);

/* Secrets */
int secret_create(const char *name, const char *namespace, const char *type, const void *data, size_t len);
int secret_delete(const char *name, const char *namespace);
int secret_update(const char *name, const char *namespace, const void *data, size_t len);
void *secret_get(const char *name, const char *namespace, size_t *len);
struct secret **secrets_list(const char *namespace, int *count);
int secret_encrypt(const void *data, size_t len, void **out, size_t *out_len);
int secret_decrypt(void *data, size_t len, void **out, size_t *out_len);

/* TLS certificates */
int cert_generate_ca(const char *name);
int cert_generate_node(const char *name, const char *node_id);
int cert_generate_api(const char *name, const char **sans, int n_sans);
int cert_sign_csr(const char *csr, const char *ca_name, char **cert_out);
int cert_load(const char *name, struct tls_cert *cert);
int cert_save(const char *name, struct tls_cert *cert);
int cert_rotate(const char *name);
int cert_check_expiry(const char *name, int warning_days, int critical_days);

/* Audit logging */
int audit_log(const char *user, const char *action, const char *resource,
	const char *resource_name, const char *result, const char *details);
struct audit_entry **audit_query(time_t start, time_t end, const char *user,
	const char *action, const char *resource, int *count);
int audit_export_json(FILE *fp, time_t start, time_t end);
int audit_export_syslog(const char *syslog_facility);

/* Session management */
int session_create(const char *username, char *session_id, size_t session_len);
int session_destroy(const char *session_id);
int session_destroy_user(const char *username);
bool session_valid(const char *session_id);
int session_set_data(const char *session_id, const char *key, const void *value, size_t len);
int session_get_data(const char *session_id, const char *key, void *value, size_t *len);

#endif /* _OCIFBSD_AUTH_H */
