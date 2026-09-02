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
 * Certificate management header
 */

#ifndef _OCIFBSD_CERT_H
#define _OCIFBSD_CERT_H

#include <sys/param.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

/* Certificate types */
#define CERT_TYPE_CA              0
#define CERT_TYPE_NODE            1
#define CERT_TYPE_API_SERVER      2
#define CERT_TYPE_SERVICE_ACCOUNT 3
#define CERT_TYPE_ETCD            4
#define CERT_TYPE_FRONT_PROXY     5
#define CERT_TYPE_KUBELET         6
#define CERT_TYPE_EXTERNAL        7

/* Certificate status */
#define CERT_STATUS_VALID         0
#define CERT_STATUS_EXPIRING      1
#define CERT_STATUS_EXPIRED       2
#define CERT_STATUS_REVOKED       3
#define CERT_STATUS_PENDING       4
#define CERT_STATUS_ROTATING      5

/* Certificate structure */
struct cert_info {
	char            name[256];             /* Certificate name */
	int             type;                  /* CERT_TYPE_* */
	char            cn[256];              /* Common Name */
	char            *sans;                /* Subject Alternative Names (JSON array) */
	char            issuer[256];          /* Issuer CN */
	time_t          created;
	time_t          expires;
	time_t          last_rotated;
	int             status;               /* CERT_STATUS_* */
	char            key_path[PATH_MAX];
	char            cert_path[PATH_MAX];
	char            *metadata;
	RB_ENTRY(cert_info) entry;
};
RB_HEAD(cert_tree, cert_info);
RB_PROTOTYPE(cert_tree, cert_info, entry, cert_info_cmp);

/* Certificate rotation config */
struct rotation_config {
	int             ca_rotation_years;     /* CA rotation in years */
	int             node_rotation_days;     /* Node cert rotation days */
	int             api_rotation_days;     /* API server rotation days */
	int             service_rotation_days; /* Service account rotation */
	int             warning_days;          /* Days before expiry to warn */
	int             critical_days;          /* Days before expiry for critical */
	bool            auto_rotate;
	bool            dual_cert_mode;         /* During rotation */
};

/* Certificate backup */
struct cert_backup {
	char            cert_name[256];
	time_t          backed_up;
	char            backup_path[PATH_MAX];
	char            checksum[128];
	bool            encrypted;
	char            *metadata;
};

/* ACME/Let's Encrypt configuration */
struct acme_config {
	bool            enabled;
	char            server_url[512];
	char            email[256];
	char            agree_tos;
	char            *private_key_path;
	char            *certificate_path;
	char            *chain_path;
	char            *challenges_dir;        /* HTTP-01 challenge dir */
	bool            dns_provider_enabled;
	char            dns_provider[128];
	char            dns_credentials_path[PATH_MAX];
};

/* External CA configuration */
struct external_ca_config {
	bool            enabled;
	int             type;                   /* ADCS/VAULT/OTHER */
	char            server_url[512];
	char            *ca_name;               /* Template name */
	char            *credentials_path;       /* Auth credentials */
	char            *intermediate_certs;     /* Trust chain */
	int             enrollment_type;         /* SCEP/EST/CMS */
};

/* Certificate monitoring */
struct cert_expiry_alert {
	char            cert_name[256];
	int             days_until_expiry;
	int             severity;               /* INFO/WARNING/CRITICAL */
	time_t          alerted_at;
	bool            acknowledged;
};

/* Certificate statistics */
struct cert_stats {
	uint32_t        total_certs;
	uint32_t        valid_certs;
	uint32_t        expiring_certs;
	uint32_t        expired_certs;
	uint32_t        revoked_certs;
	uint32_t        rotations_performed;
	uint32_t        rotations_failed;
	time_t          last_rotation;
	time_t          next_scheduled_rotation;
};

/* Function declarations */

/* Core certificate management */
int     cert_init(void);
void    cert_shutdown(void);

int     cert_create_ca(const char *name, int validity_days);
int     cert_create_node(const char *name, const char *cn, const char *sans);
int     cert_create_api(const char *name, const char *cn, const char *sans);
int     cert_create_service_account(const char *name, const char *namespace);

struct cert_info *cert_get(const char *name);
struct cert_info **cert_list(int *count);
int     cert_delete(const char *name);
int     cert_renew(const char *name);
int     cert_revoke(const char *name, const char *reason);

int     cert_verify(const char *name);
int     cert_verify_chain(const char *cert_path);
char    *cert_get_fingerprint(const char *name);

/* Certificate storage */
int     cert_save(const char *name, const char *key_pem, const char *cert_pem);
int     cert_load(const char *name, char **key_pem, char **cert_pem);
char    *cert_get_pem(const char *name, int pem_type);
int     cert_set_permissions(const char *name, mode_t mode);

/* Certificate rotation */
int     cert_rotation_init(struct rotation_config *config);
int     cert_rotation_start(const char *name);
int     cert_rotation_complete(const char *name);
int     cert_rotate_all(void);
int     cert_schedule_rotation(const char *name, time_t when);

int     cert_dual_cert_enter(const char *name);
int     cert_dual_cert_exit(const char *name);
int     cert_dual_cert_verify(const char *name);

/* CA rotation */
int     cert_ca_rotate(void);
int     cert_ca_cross_sign(void);
int     cert_ca_update_trust(const char *new_ca_path);

/* Certificate backup and history */
int     cert_backup_create(const char *name);
int     cert_backup_restore(const char *name, time_t when);
struct cert_backup **cert_backup_list(int *count);
int     cert_backup_delete(const char *name, time_t when);

int     cert_history_add(const char *name, const char *action);
struct cert_backup **cert_history_get(const char *name, int *count);

/* Certificate recovery */
int     cert_recovery_init(void);
int     cert_recovery_token_create(char **token);
int     cert_recovery_token_validate(const char *token);
int     cert_recover_from_backup(const char *name, time_t when);
int     cert_recover_from_token(const char *token);
int     cert_recover_expired(const char *name);

int     cert_self_service_recovery(const char *username, const char *token,
			const char *new_key_pem);
int     cert_admin_recovery(const char *name, const char *admin_token);

/* Certificate distribution */
int     cert_distribute_node(const char *name, const char *node);
int     cert_distribute_cluster(void);
int     cert_sync_all(void);
int     cert_check_node_certs(const char *node);

/* Certificate monitoring */
int     cert_monitor_init(void);
int     cert_check_all(void);
int     cert_check_expiry(const char *name, int warning_days, int critical_days);
struct cert_expiry_alert **cert_get_alerts(int *count);
int     cert_acknowledge_alert(const char *cert_name);
int     cert_clear_alert(const char *cert_name);

int     cert_expiry_metrics(FILE *fp);
int     cert_status_json(char **json_out);

/* ACME support */
int     acme_init(struct acme_config *config);
int     acme_account_register(const char *email);
int     acme_account_status(void);
const char *acme_account_kid(void);
int     acme_certificate_request(const char *domain, const char *challenge_dir);
int     acme_certificate_renew(const char *domain);
int     acme_certificate_revoke(const char *domain);

int     acme_challenge_http01(const char *token, const char *key_auth);
int     acme_challenge_dns01(const char *domain, const char *txt_value);
int     acme_challenge_cleanup(const char *token);

int     acme_order_create(const char **domains, int count);
int     acme_order_finalize(const char *order_url);
char    *acme_certificate_get(const char *domain);

/* External CA support */
int     extca_init(struct external_ca_config *config);
int     extca_enroll(const char *name, const char *template, char **key, char **cert);
int     extca_reenroll(const char *name, const char *template);
int     extca_revoke(const char *name, const char *reason);
int     extca_chain_install(const char *chain_pem);

int     extca_scep_enroll(const char *url, const char *ca_cert,
			const char *challenge, char **key, char **cert);
int     extca_est_enroll(const char *url, const char *profile,
			char **key, char **cert);

/* Statistics */
int     cert_stats_get(struct cert_stats *stats);
int     cert_stats_json(char **json_out);

/* CLI commands */
int     cmd_cert_list(int argc, char *argv[]);
int     cmd_cert_create(int argc, char *argv[]);
int     cmd_cert_rotate(int argc, char *argv[]);
int     cmd_cert_backup(int argc, char *argv[]);
int     cmd_cert_recover(int argc, char *argv[]);
int     cmd_cert_status(int argc, char *argv[]);
int     cmd_cert_acme(int argc, char *argv[]);
int     cmd_cert_extca(int argc, char *argv[]);

#endif /* _OCIFBSD_CERT_H */
