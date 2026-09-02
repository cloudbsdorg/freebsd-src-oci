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
 * PAM authentication header
 */

#ifndef _OCIFBSD_PAM_AUTH_H
#define _OCIFBSD_PAM_AUTH_H

#include <sys/param.h>
#include <sys/tree.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* PAM authentication result codes */
#define PAM_AUTH_SUCCESS          0
#define PAM_AUTH_FAILURE          1
#define PAM_AUTH_NO_USER          2
#define PAM_AUTH_INVALID_PASS     3
#define PAM_AUTH_EXPIRED          4
#define PAM_AUTH_LOCKED           5
#define PAM_AUTH_TOO_MANY_TRIES   6
#define PAM_AUTH_SYSTEM_ERROR     7

/* RBAC roles */
#define ROLE_CLUSTER_ADMIN    "cluster-admin"
#define ROLE_ADMIN            "admin"
#define ROLE_EDITOR           "editor"
#define ROLE_VIEWER           "viewer"
#define ROLE_OPERATOR         "operator"
#define ROLE_AUDITOR          "auditor"

#define NUM_ROLES 6

/* RBAC permissions */
#define PERM_READ         0x01
#define PERM_WRITE        0x02
#define PERM_DELETE       0x04
#define PERM_EXEC         0x08
#define PERM_ADMIN        0x10
#define PERM_AUDIT        0x20

/* Session types */
#define SESSION_INTERACTIVE   0
#define SESSION_TOKEN         1
#define SESSION_CERTIFICATE   2
#define SESSION_SERVICE       3

/* User structure */
struct ocifbsd_user {
    char            username[256];          /* username */
    uid_t           uid;                    /* system UID */
    char            role[64];               /* RBAC role */
    uint32_t       permissions;            /* PERM_* flags */
    bool            is_service_account;     /* service account flag */
    time_t          created;                /* creation time */
    time_t          last_login;             /* last successful login */
    time_t          token_expires;          /* token expiration */
    uint32_t       login_count;            /* total login count */
    uint32_t       failed_count;           /* consecutive failed logins */
    time_t          locked_until;           /* lock expiration (0 = not locked) */
    char            pubkey_fingerprint[128];/* SSH public key */
    char            *groups;                /* comma-separated groups */
    char            *annotations;           /* user annotations */
    RB_ENTRY(ocifbsd_user) entry;
};

/* RBAC role bindings */
struct role_binding {
    char            role[64];               /* role name */
    char            *users;                  /* comma-separated users */
    char            *groups;                 /* comma-separated groups */
    char            *namespaces;             /* comma-separated namespaces (empty = all) */
    time_t          created;
    RB_ENTRY(role_binding) entry;
};

/* RBAC permission mapping */
struct role_permissions {
    const char      *role;
    uint32_t       permissions;
    const char     *description;
};

/* PAM auth request */
struct pam_auth_request {
    const char      *username;
    const char      *password;
    const char      *service;
    int             session_type;
    const char      *pubkey;
    const char      *token;
    const char      *certificate;
    char            *client_addr;
    bool            interactive;
};

/* PAM auth response */
struct pam_auth_response {
    int             result;                  /* PAM_AUTH_* result code */
    char            *token;                  /* JWT token on success */
    time_t          expires;
    char            role[64];
    uint32_t       permissions;
    char            *message;
    char            *username;
};

/* User registry */
RB_HEAD(user_tree, ocifbsd_user);
RB_PROTOTYPE(user_tree, ocifbsd_user, entry, user_compare);

/* Group to role mapping entry */
struct group_role_map {
    char            group_name[64];
    char            role[64];
    RB_ENTRY(group_role_map) entry;
};
RB_HEAD(group_role_map_tree, group_role_map);
RB_PROTOTYPE(group_role_map_tree, group_role_map, entry, group_compare);

/* PAM module configuration */
struct pam_config {
    bool            enable_pam;              /* enable PAM authentication */
    bool            enable_group_map;         /* enable system group to role mapping */
    bool            enable_pubkey;           /* enable SSH public key auth */
    bool            enable_totp;              /* enable TOTP MFA */
    int             max_failed_attempts;     /* max failed attempts before lock */
    int             lockout_duration;        /* lockout duration in seconds */
    int             token_lifetime;           /* token lifetime in seconds */
    int             session_timeout;         /* session timeout in seconds */
    char            *default_role;           /* default role for new users */
    char            *admin_users;            /* comma-separated admin users */
    char            *pam_service;            /* PAM service name */
    char            *ldap_uri;               /* LDAP server URI */
    char            *ldap_base_dn;            /* LDAP base DN */
    char            *krb_realm;              /* Kerberos realm */
    char            *totp_issuer;            /* TOTP issuer name */
};

/* Audit log entry */
struct audit_entry {
    time_t          timestamp;
    char            username[256];
    char            action[128];
    char            resource[256];
    char            result[32];
    char            client_addr[64];
    char            *details;
};

/* PAM authentication functions */
int     pam_auth_init(void);
void    pam_auth_shutdown(void);
struct pam_config *pam_get_config(void);
int     pam_set_config(struct pam_config *config);

int     pam_authenticate_user(struct pam_auth_request *req,
            struct pam_auth_response *resp);
int     pam_verify_token(const char *token, const char *expected_user,
            uint32_t *permissions);
int     pam_refresh_token(const char *refresh_token, char **new_token);

struct ocifbsd_user *ocifbsd_pam_get_user(const char *username);
struct ocifbsd_user **pam_list_users(int *count);
int     pam_create_user(struct ocifbsd_user *user);
int     pam_update_user(struct ocifbsd_user *user);
int     pam_delete_user(const char *username);
int     pam_set_user_role(const char *username, const char *role);

int     pam_add_role_binding(struct role_binding *binding);
int     pam_remove_role_binding(const char *role);
struct role_binding **pam_list_role_bindings(int *count);

const char *pam_role_from_group(const char *group);
uint32_t   pam_permissions_for_role(const char *role);

int     pam_log_audit(struct audit_entry *entry);
struct audit_entry **pam_query_audit(const char *username,
            time_t start, time_t end, int *count);

int     pam_rate_limit(const char *client_addr);
void    pam_reset_rate_limit(const char *client_addr);

/* PAM conversation function for interactive auth */
typedef int (*pam_conv_func_t)(int msg_style, const char *msg, char **resp);
void    pam_set_conv_func(pam_conv_func_t func);

/* Token generation */
int     pam_generate_token(const char *username, const char *role,
            uint32_t permissions, int lifetime, char **token_out);
int     pam_verify_jwt(const char *jwt, char **username, uint32_t *perms,
            time_t *exp);

/* User locking */
int     pam_lock_user(const char *username, time_t until);
int     pam_unlock_user(const char *username);
bool    pam_is_user_locked(const char *username);

#endif /* _OCIFBSD_PAM_AUTH_H */
