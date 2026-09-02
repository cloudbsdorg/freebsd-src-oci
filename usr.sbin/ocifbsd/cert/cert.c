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
 * Certificate management implementation
 * Phase 16: Certificate Management
 */

#include <sys/param.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/types.h>
#include <dirent.h>

#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/rand.h>
#include <openssl/bn.h>

#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <libutil.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include "cert.h"
#include "../include/ocifbsd.h"

/*
 * Recursive mkdir(2). FreeBSD 16's <libutil.h> does not export mkdirp
 * in any public header (it lives in libutil's internal mkdir.c and is
 * gated by strict feature test macros). We provide a local copy.
 * Returns 0 on success, -1 on failure (errno set by mkdir(2)).
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

/*
 * Assign a random, positive 64-bit serial number to a certificate.
 * Fixed/predictable serials (a constant, or time(NULL)) weaken defense
 * against hash-collision forgery and violate CA/Browser Forum rules.
 * Returns 0 on success, -1 on failure.
 */
static int
cert_set_random_serial(X509 *cert)
{
	unsigned char bytes[8];
	BIGNUM *bn = NULL;
	ASN1_INTEGER *serial;
	int ret = -1;

	if (RAND_bytes(bytes, sizeof(bytes)) != 1)
		return (-1);
	bytes[0] &= 0x7f;	/* keep it positive */
	bn = BN_bin2bn(bytes, sizeof(bytes), NULL);
	if (bn == NULL)
		return (-1);
	serial = X509_get_serialNumber(cert);
	if (serial != NULL && BN_to_ASN1_INTEGER(bn, serial) != NULL)
		ret = 0;
	BN_free(bn);
	return (ret);
}

/* Forward declarations for functions defined later in this file */
static struct cert_info *cert_parse_json(const char *json);
static X509 *cert_create_signed(EVP_PKEY *pkey, const char *cn,
    const char *sans, int days);
static int cert_save_registry(void);
static void cert_load_registry(void);

/* Global state */
static struct cert_tree cert_registry;
static struct rotation_config rot_config;
static struct cert_stats stats;
static pthread_mutex_t cert_lock = PTHREAD_MUTEX_INITIALIZER;
static int initialized = 0;
static char cert_dir[PATH_MAX];
static char backup_dir[PATH_MAX];

/*
 * Generate an EC P-256 key using the OpenSSL 3.0 provider API.
 * The legacy EC_KEY_* functions (EC_KEY_new_by_curve_name,
 * EC_KEY_generate_key, EVP_PKEY_assign_EC_KEY) are deprecated
 * in OpenSSL 3.0 and emit -Wdeprecated-declarations errors.
 * Returns 0 on success, -1 on failure. Caller owns *pkey.
 */
static int
cert_generate_ec_key(EVP_PKEY **pkey)
{
    EVP_PKEY_CTX *ctx;

    *pkey = NULL;
    ctx = EVP_PKEY_CTX_new_from_name(NULL, "EC", NULL);
    if (ctx == NULL)
        return (-1);
    if (EVP_PKEY_keygen_init(ctx) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return (-1);
    }
    if (EVP_PKEY_generate(ctx, pkey) <= 0) {
        EVP_PKEY_CTX_free(ctx);
        return (-1);
    }
    EVP_PKEY_CTX_free(ctx);
    return (0);
}

/*
 * Initialize certificate management
 */
int
cert_init(void)
{
    if (initialized)
        return (0);

    /* Initialize registry */
    RB_INIT(&cert_registry);

    /* Set up directories */
    snprintf(cert_dir, sizeof(cert_dir), "%s/certs", OCIFBSD_CONFIG_DIR);
    snprintf(backup_dir, sizeof(backup_dir), "%s/certs/backups", OCIFBSD_DATA_DIR);
    mkdirp(cert_dir, 0700);
    mkdirp(backup_dir, 0700);

    /* Load existing certificates */
    cert_load_registry();

    /* Initialize rotation config */
    memset(&rot_config, 0, sizeof(rot_config));
    rot_config.ca_rotation_years = 10;
    rot_config.node_rotation_days = 365;
    rot_config.api_rotation_days = 90;
    rot_config.service_rotation_days = 365;
    rot_config.warning_days = 30;
    rot_config.critical_days = 7;
    rot_config.auto_rotate = true;

    /* Initialize stats */
    memset(&stats, 0, sizeof(stats));

    /* Open syslog */
    openlog("ocifbsd-cert", LOG_PID, LOG_DAEMON);

    syslog(LOG_INFO, "Certificate management initialized");

    initialized = 1;
    return (0);
}

/*
 * Shutdown certificate management
 */
void
cert_shutdown(void)
{
    struct cert_info *cert;

    if (!initialized)
        return;

    pthread_mutex_lock(&cert_lock);

    RB_FOREACH(cert, cert_tree, &cert_registry) {
        RB_REMOVE(cert_tree, &cert_registry, cert);
        free(cert);
    }

    pthread_mutex_unlock(&cert_lock);
    closelog();

    initialized = 0;
}

/*
 * Load certificate registry from disk
 */
static void
cert_load_registry(void)
{
    char path[PATH_MAX];
    FILE *fp;
    char buf[1024];

    snprintf(path, sizeof(path), "%s/registry.json", cert_dir);
    fp = fopen(path, "r");
    if (fp == NULL)
        return;

    /* Parse JSON registry */
    while (fgets(buf, sizeof(buf), fp) != NULL) {
        struct cert_info *cert = cert_parse_json(buf);
        if (cert) {
            if (RB_INSERT(cert_tree, &cert_registry, cert) != NULL) {
                /* Duplicate name in the registry file; don't leak. */
                free(cert);
            } else {
                stats.total_certs++;
            }
        }
    }

    fclose(fp);
}

/*
 * Parse certificate info from JSON
 */
static struct cert_info *
cert_parse_json(const char *json)
{
    struct cert_info *cert;
    char name[256], cn[256], key_path[PATH_MAX], cert_path[PATH_MAX];
    int type, status;
    long created, expires, last_rotated;

    cert = calloc(1, sizeof(*cert));
    if (cert == NULL)
        return (NULL);

    if (sscanf(json,
        "{\"name\":\"%255[^\"]\",\"type\":%d,\"cn\":\"%255[^\"]\","
        "\"status\":%d,\"created\":%ld,\"expires\":%ld,\"last_rotated\":%ld,"
        "\"key_path\":\"%1023[^\"]\",\"cert_path\":\"%1023[^\"]\"}",
        name, &type, cn, &status, &created, &expires, &last_rotated,
        key_path, cert_path) == 9) {

        strlcpy(cert->name, name, sizeof(cert->name));
        strlcpy(cert->cn, cn, sizeof(cert->cn));
        strlcpy(cert->key_path, key_path, sizeof(cert->key_path));
        strlcpy(cert->cert_path, cert_path, sizeof(cert->cert_path));
        cert->type = type;
        cert->status = status;
        cert->created = created;
        cert->expires = expires;
        cert->last_rotated = last_rotated;
    } else {
        free(cert);
        cert = NULL;
    }

    return (cert);
}

/*
 * Create CA certificate
 */
int
cert_create_ca(const char *name, int validity_days)
{
    EVP_PKEY *pkey;
    X509 *cert;
    FILE *fp;
    char key_path[PATH_MAX], cert_path[PATH_MAX];
    struct cert_info *info;

    if (name == NULL || validity_days <= 0)
        return (-1);

    /* Generate EC P-256 key (OpenSSL 3.0 provider API) */
    if (cert_generate_ec_key(&pkey) != 0)
        return (-1);

    /* Create self-signed CA certificate */
    cert = X509_new();
    if (cert == NULL) {
        EVP_PKEY_free(pkey);
        return (-1);
    }

    X509_set_version(cert, 2);  /* v3 */
    if (cert_set_random_serial(cert) != 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return (-1);
    }
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), validity_days * 86400L);
    X509_set_pubkey(cert, pkey);

    X509_NAME *name_obj = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name_obj, "CN", MBSTRING_ASC,
        (unsigned char *)name, -1, -1, 0);
    X509_set_issuer_name(cert, name_obj);

    /* CA extensions */
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, cert, cert, NULL, NULL, 0);

    X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &ctx,
        NID_basic_constraints, "CA:TRUE");
    if (ext) {
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
    }

    ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_key_usage,
        "keyCertSign,cRLSign,digitalSignature");
    if (ext) {
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
    }

    /* Sign with CA key */
    if (X509_sign(cert, pkey, EVP_sha256()) == 0) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return (-1);
    }

    /* Save files */
    snprintf(key_path, sizeof(key_path), "%s/%s.key", cert_dir, name);
    snprintf(cert_path, sizeof(cert_path), "%s/%s.crt", cert_dir, name);

    /* Create the private-key file 0600 from the start. fopen("w") creates it
     * 0644 (& ~umask) and the trailing chmod leaves a TOCTOU window in which
     * the EC private key is world/group-readable. */
    {
        int kfd = open(key_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        fp = (kfd >= 0) ? fdopen(kfd, "w") : NULL;
        if (fp == NULL && kfd >= 0)
            close(kfd);
    }
    if (fp) {
        PEM_write_PrivateKey(fp, pkey, NULL, NULL, 0, NULL, NULL);
        fclose(fp);
    }

    fp = fopen(cert_path, "w");
    if (fp) {
        PEM_write_X509(fp, cert);
        fclose(fp);
    }

    /* Add to registry */
    info = calloc(1, sizeof(*info));
    if (info) {
        strlcpy(info->name, name, sizeof(info->name));
        strlcpy(info->cn, name, sizeof(info->cn));
        info->type = CERT_TYPE_CA;
        info->status = CERT_STATUS_VALID;
        info->created = time(NULL);
        info->expires = info->created + (validity_days * 86400L);
        info->last_rotated = info->created;
        strlcpy(info->key_path, key_path, sizeof(info->key_path));
        strlcpy(info->cert_path, cert_path, sizeof(info->cert_path));

        pthread_mutex_lock(&cert_lock);
        if (RB_INSERT(cert_tree, &cert_registry, info) != NULL) {
            /* Name already present; don't leak the duplicate or double-count. */
            free(info);
            info = NULL;
        } else {
            stats.total_certs++;
            stats.valid_certs++;
        }
        pthread_mutex_unlock(&cert_lock);

        cert_save_registry();
    }

    EVP_PKEY_free(pkey);
    X509_free(cert);

    syslog(LOG_INFO, "Created CA certificate: %s", name);

    return (0);
}

/*
 * Create node certificate
 */
int
cert_create_node(const char *name, const char *cn, const char *sans)
{
    /* Similar to CA but signed by CA, not self-signed */
    char *key_pem, *cert_pem;
    int ret;

    if (name == NULL || cn == NULL)
        return (-1);

    /* Generate EC P-256 key (OpenSSL 3.0 provider API) */
    EVP_PKEY *pkey;
    if (cert_generate_ec_key(&pkey) != 0)
        return (-1);

    /* Convert key to PEM */
    BIO *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL);
    char *key_buf;
    long key_len = BIO_get_mem_data(bio, &key_buf);
    key_pem = strndup(key_buf, key_len);
    BIO_free(bio);

    /* Create and sign certificate */
    X509 *cert = cert_create_signed(pkey, cn, sans, rot_config.node_rotation_days);
    if (cert == NULL) {
        EVP_PKEY_free(pkey);
        free(key_pem);
        return (-1);
    }

    bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, cert);
    char *cert_buf;
    long cert_len = BIO_get_mem_data(bio, &cert_buf);
    cert_pem = strndup(cert_buf, cert_len);
    BIO_free(bio);

    /* Save */
    ret = cert_save(name, key_pem, cert_pem);

    free(key_pem);
    free(cert_pem);
    EVP_PKEY_free(pkey);
    X509_free(cert);

    return (ret);
}

/*
 * Create signed certificate (internal helper)
 */
static X509 *
cert_create_signed(EVP_PKEY *pkey, const char *cn, const char *sans, int days)
{
    EVP_PKEY *ca_key;
    X509 *ca_cert, *cert;

    /* Load CA key and cert */
    char ca_key_path[PATH_MAX], ca_cert_path[PATH_MAX];
    snprintf(ca_key_path, sizeof(ca_key_path), "%s/ca.key", cert_dir);
    snprintf(ca_cert_path, sizeof(ca_cert_path), "%s/ca.crt", cert_dir);

    FILE *fp = fopen(ca_key_path, "r");
    if (fp == NULL)
        return (NULL);
    ca_key = PEM_read_PrivateKey(fp, NULL, NULL, NULL);
    fclose(fp);

    fp = fopen(ca_cert_path, "r");
    if (fp == NULL) {
        EVP_PKEY_free(ca_key);
        return (NULL);
    }
    ca_cert = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);

    if (ca_key == NULL || ca_cert == NULL) {
        if (ca_key) EVP_PKEY_free(ca_key);
        if (ca_cert) X509_free(ca_cert);
        return (NULL);
    }

    /* Create certificate */
    cert = X509_new();
    if (cert == NULL) {
        EVP_PKEY_free(ca_key);
        X509_free(ca_cert);
        return (NULL);
    }

    X509_set_version(cert, 2);
    if (cert_set_random_serial(cert) != 0) {
        X509_free(cert);
        EVP_PKEY_free(ca_key);
        X509_free(ca_cert);
        return (NULL);
    }
    X509_gmtime_adj(X509_get_notBefore(cert), 0);
    X509_gmtime_adj(X509_get_notAfter(cert), days * 86400L);
    X509_set_pubkey(cert, pkey);

    X509_NAME *name_obj = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name_obj, "CN", MBSTRING_ASC,
        (unsigned char *)cn, -1, -1, 0);
    X509_set_issuer_name(cert, X509_get_subject_name(ca_cert));

    /* Sign with CA */
    if (X509_sign(cert, ca_key, EVP_sha256()) == 0) {
        X509_free(cert);
        EVP_PKEY_free(ca_key);
        X509_free(ca_cert);
        return (NULL);
    }

    EVP_PKEY_free(ca_key);
    X509_free(ca_cert);

    return (cert);
}

/*
 * Create API server certificate
 */
int
cert_create_api(const char *name, const char *cn, const char *sans)
{
    return (cert_create_node(name, cn, sans));
}

/*
 * Create service account certificate
 */
int
cert_create_service_account(const char *name, const char *namespace)
{
    char cn[512];
    snprintf(cn, sizeof(cn), "system:serviceaccount:%s:%s",
        namespace ? namespace : "default", name);
    return (cert_create_node(name, cn, NULL));
}

/*
 * Get certificate info
 */
struct cert_info *
cert_get(const char *name)
{
    struct cert_info key, *cert;

    if (name == NULL)
        return (NULL);

    pthread_mutex_lock(&cert_lock);
    strlcpy(key.name, name, sizeof(key.name));
    cert = RB_FIND(cert_tree, &cert_registry, &key);
    pthread_mutex_unlock(&cert_lock);

    return (cert);
}

/*
 * List all certificates
 */
struct cert_info **
cert_list(int *count)
{
    struct cert_info **list = NULL;
    struct cert_info *cert;
    int n = 0;

    if (count == NULL)
        return (NULL);

    pthread_mutex_lock(&cert_lock);

    RB_FOREACH(cert, cert_tree, &cert_registry) {
        if (ocifbsd_realloc_grow((void **)&list, (n + 1) * sizeof(*list)) != 0)
        	break;
        list[n++] = cert;
    }

    pthread_mutex_unlock(&cert_lock);

    *count = n;
    return (list);
}

/*
 * Delete certificate
 */
int
cert_delete(const char *name)
{
    struct cert_info *cert;

    if (name == NULL)
        return (-1);

    pthread_mutex_lock(&cert_lock);

    /*
     * Look the node up directly under the lock. Calling cert_get() here
     * would try to re-acquire the non-recursive cert_lock and deadlock.
     */
    {
        struct cert_info key;

        strlcpy(key.name, name, sizeof(key.name));
        cert = RB_FIND(cert_tree, &cert_registry, &key);
    }
    if (cert == NULL) {
        pthread_mutex_unlock(&cert_lock);
        return (-1);
    }

    /* Remove files */
    unlink(cert->key_path);
    unlink(cert->cert_path);

    RB_REMOVE(cert_tree, &cert_registry, cert);
    stats.total_certs--;
    if (cert->status == CERT_STATUS_VALID)
        stats.valid_certs--;

    free(cert);

    pthread_mutex_unlock(&cert_lock);

    cert_save_registry();

    return (0);
}

/*
 * Renew certificate
 */
int
cert_renew(const char *name)
{
    struct cert_info *cert;

    cert = cert_get(name);
    if (cert == NULL)
        return (-1);

    /* Re-create the certificate */
    switch (cert->type) {
    case CERT_TYPE_NODE:
        cert_create_node(name, cert->cn, cert->sans);
        break;
    case CERT_TYPE_API_SERVER:
        cert_create_api(name, cert->cn, cert->sans);
        break;
    default:
        return (-1);
    }

    cert->last_rotated = time(NULL);
    cert_save_registry();

    return (0);
}

/*
 * Revoke certificate
 */
int
cert_revoke(const char *name, const char *reason)
{
    struct cert_info *cert;

    cert = cert_get(name);
    if (cert == NULL)
        return (-1);

    pthread_mutex_lock(&cert_lock);
    cert->status = CERT_STATUS_REVOKED;
    stats.revoked_certs++;
    pthread_mutex_unlock(&cert_lock);

    cert_save_registry();
    cert_history_add(name, "revoked");

    syslog(LOG_WARNING, "Certificate revoked: %s reason: %s", name, reason);

    return (0);
}

/*
 * Verify certificate
 */
int
cert_verify(const char *name)
{
    struct cert_info *cert;
    X509 *cert_x509;
    FILE *fp;
    int ret = -1;

    cert = cert_get(name);
    if (cert == NULL)
        return (-1);

    fp = fopen(cert->cert_path, "r");
    if (fp == NULL)
        return (-1);

    cert_x509 = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);

    if (cert_x509 == NULL)
        return (-1);

    /* Check expiry */
    if (X509_cmp_current_time(X509_get_notAfter(cert_x509)) < 0) {
        pthread_mutex_lock(&cert_lock);
        cert->status = CERT_STATUS_EXPIRED;
        stats.expired_certs++;
        pthread_mutex_unlock(&cert_lock);
        ret = 0;
    } else {
        ret = 0;
    }

    X509_free(cert_x509);
    return (ret);
}

/*
 * Get certificate fingerprint
 */
char *
cert_get_fingerprint(const char *name)
{
    struct cert_info *cert;
    X509 *cert_x509;
    FILE *fp;
    static char fingerprint[EVP_MAX_MD_SIZE * 3 + 1];
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int n;
    char *ret = NULL;

    cert = cert_get(name);
    if (cert == NULL)
        return (NULL);

    fp = fopen(cert->cert_path, "r");
    if (fp == NULL)
        return (NULL);

    cert_x509 = PEM_read_X509(fp, NULL, NULL, NULL);
    fclose(fp);

    if (cert_x509 == NULL)
        return (NULL);

    if (X509_digest(cert_x509, EVP_sha256(), md, &n)) {
        for (unsigned int i = 0; i < n; i++) {
            sprintf(fingerprint + (i * 3), "%02X:", md[i]);
        }
        fingerprint[n * 3 - 1] = '\0';
        ret = fingerprint;
    }

    X509_free(cert_x509);
    return (ret);
}

/*
 * Save certificate
 */
int
cert_save(const char *name, const char *key_pem, const char *cert_pem)
{
    char key_path[PATH_MAX], cert_path[PATH_MAX];
    FILE *fp;

    if (name == NULL || key_pem == NULL || cert_pem == NULL)
        return (-1);

    snprintf(key_path, sizeof(key_path), "%s/%s.key", cert_dir, name);
    snprintf(cert_path, sizeof(cert_path), "%s/%s.crt", cert_dir, name);

    /* Create the key file 0600 from the start (avoid the fopen-then-chmod
     * TOCTOU window that exposes the private key). */
    {
        int kfd = open(key_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        fp = (kfd >= 0) ? fdopen(kfd, "w") : NULL;
        if (fp == NULL && kfd >= 0)
            close(kfd);
    }
    if (fp == NULL)
        return (-1);
    fputs(key_pem, fp);
    fclose(fp);

    fp = fopen(cert_path, "w");
    if (fp == NULL)
        return (-1);
    fputs(cert_pem, fp);
    fclose(fp);

    /* Add to registry */
    struct cert_info *info = calloc(1, sizeof(*info));
    if (info) {
        strlcpy(info->name, name, sizeof(info->name));
        info->type = CERT_TYPE_NODE;
        info->status = CERT_STATUS_VALID;
        info->created = time(NULL);
        info->expires = info->created + (rot_config.node_rotation_days * 86400L);
        info->last_rotated = info->created;
        strlcpy(info->key_path, key_path, sizeof(info->key_path));
        strlcpy(info->cert_path, cert_path, sizeof(info->cert_path));

        pthread_mutex_lock(&cert_lock);
        struct cert_info *existing =
            RB_INSERT(cert_tree, &cert_registry, info);
        if (existing != NULL) {
            /*
             * A cert with this name already exists (e.g. on renewal).
             * RB_INSERT did not insert, so update the existing entry and
             * free the duplicate instead of leaking it and double-counting
             * the stats.
             */
            existing->type = info->type;
            existing->status = info->status;
            existing->expires = info->expires;
            existing->last_rotated = info->last_rotated;
            strlcpy(existing->key_path, info->key_path,
                sizeof(existing->key_path));
            strlcpy(existing->cert_path, info->cert_path,
                sizeof(existing->cert_path));
            free(info);
        } else {
            stats.total_certs++;
            stats.valid_certs++;
        }
        pthread_mutex_unlock(&cert_lock);

        cert_save_registry();
    }

    return (0);
}

/*
 * Load certificate
 */
int
cert_load(const char *name, char **key_pem, char **cert_pem)
{
    struct cert_info *cert;
    FILE *fp;
    long size;

    cert = cert_get(name);
    if (cert == NULL)
        return (-1);

    if (key_pem) {
        fp = fopen(cert->key_path, "r");
        if (fp == NULL)
            return (-1);
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (size < 0) {		/* unseekable / error */
            fclose(fp);
            return (-1);
        }
        *key_pem = malloc(size + 1);
        if (*key_pem) {
            size_t got = fread(*key_pem, 1, size, fp);
            (*key_pem)[got] = '\0';
        }
        fclose(fp);
    }

    if (cert_pem) {
        fp = fopen(cert->cert_path, "r");
        if (fp == NULL)
            return (-1);
        fseek(fp, 0, SEEK_END);
        size = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (size < 0) {
            fclose(fp);
            return (-1);
        }
        *cert_pem = malloc(size + 1);
        if (*cert_pem) {
            size_t got = fread(*cert_pem, 1, size, fp);
            (*cert_pem)[got] = '\0';
        }
        fclose(fp);
    }

    return (0);
}

/*
 * Save registry to disk
 */
static int
cert_save_registry(void)
{
    FILE *fp;
    struct cert_info *cert;
    char path[PATH_MAX];

    snprintf(path, sizeof(path), "%s/registry.json", cert_dir);

    fp = fopen(path, "w");
    if (fp == NULL)
        return (-1);

    RB_FOREACH(cert, cert_tree, &cert_registry) {
        /* cn comes from a CSR and the paths are filesystem paths: escape so
         * a crafted value cannot inject into the status JSON. */
        char ecn[sizeof(cert->cn) * 6];
        char ekey[sizeof(cert->key_path) * 6];
        char ecrt[sizeof(cert->cert_path) * 6];

        fprintf(fp,
            "{\"name\":\"%s\",\"type\":%d,\"cn\":\"%s\","
            "\"status\":%d,\"created\":%ld,\"expires\":%ld,\"last_rotated\":%ld,"
            "\"key_path\":\"%s\",\"cert_path\":\"%s\"}\n",
            cert->name, cert->type,
            ocifbsd_json_escape(cert->cn, ecn, sizeof(ecn)),
            cert->status, (long)cert->created, (long)cert->expires,
            (long)cert->last_rotated,
            ocifbsd_json_escape(cert->key_path, ekey, sizeof(ekey)),
            ocifbsd_json_escape(cert->cert_path, ecrt, sizeof(ecrt)));
    }

    fclose(fp);
    return (0);
}

/*
 * Copy a single file into destdir/<basename(src)>. Returns 0 on success.
 * Uses plain read/write — never a shell — so a cert name containing shell
 * metacharacters cannot cause command execution.
 */
static int
cert_copy_into_dir(const char *src, const char *destdir)
{
    char dest[PATH_MAX];
    const char *base;
    int in = -1, out = -1, ret = -1;
    char buf[8192];
    ssize_t n;

    base = strrchr(src, '/');
    base = (base != NULL) ? base + 1 : src;
    if ((size_t)snprintf(dest, sizeof(dest), "%s/%s", destdir, base) >=
        sizeof(dest))
        return (-1);

    in = open(src, O_RDONLY);
    if (in < 0)
        return (-1);
    out = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0)
        goto done;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, (size_t)n) != n)
            goto done;
    }
    if (n == 0)
        ret = 0;
done:
    if (in >= 0)
        close(in);
    if (out >= 0)
        close(out);
    return (ret);
}

/*
 * Create certificate backup
 */
int
cert_backup_create(const char *name)
{
    struct cert_info *cert;
    char backup_path[PATH_MAX];
    char manifest_path[PATH_MAX];
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    char timestamp[64];
    char key_path[PATH_MAX], cert_path[PATH_MAX];
    time_t expires;

    strftime(timestamp, sizeof(timestamp), "%Y%m%d-%H%M%S", tm);

    cert = cert_get(name);
    if (cert == NULL)
        return (-1);
    /* Snapshot the fields we need; cert_get returns after unlocking. */
    strlcpy(key_path, cert->key_path, sizeof(key_path));
    strlcpy(cert_path, cert->cert_path, sizeof(cert_path));
    expires = cert->expires;

    /* Create backup directory */
    snprintf(backup_path, sizeof(backup_path), "%s/%s-%s",
        backup_dir, name, timestamp);
    mkdirp(backup_path, 0700);

    /* Copy files in-process (no shell) */
    if (cert_copy_into_dir(key_path, backup_path) != 0 ||
        cert_copy_into_dir(cert_path, backup_path) != 0)
        return (-1);

    /* Create backup manifest as a file inside the backup directory */
    if ((size_t)snprintf(manifest_path, sizeof(manifest_path),
        "%s/manifest.txt", backup_path) < sizeof(manifest_path)) {
        FILE *fp = fopen(manifest_path, "w");
        if (fp) {
            fprintf(fp, "cert_name=%s\n", name);
            fprintf(fp, "backed_up=%ld\n", (long)now);
            fprintf(fp, "expires=%ld\n", (long)expires);
            fclose(fp);
        }
    }

    syslog(LOG_INFO, "Certificate backed up: %s to %s", name, backup_path);

    return (0);
}

/*
 * Certificate tree comparison.
 */
static int
cert_info_cmp(struct cert_info *a, struct cert_info *b)
{
    return (strcmp(a->name, b->name));
}
RB_GENERATE(cert_tree, cert_info, entry, cert_info_cmp);

/*
 * Get statistics
 */
int
cert_stats_get(struct cert_stats *out)
{
    if (out == NULL)
        return (-1);

    pthread_mutex_lock(&cert_lock);
    *out = stats;
    pthread_mutex_unlock(&cert_lock);

    return (0);
}

/*
 * Statistics as JSON
 */
int
cert_stats_json(char **json_out)
{
    struct cert_stats s;
    char *json;

    if (json_out == NULL)
        return (-1);

    cert_stats_get(&s);

    if (asprintf(&json,
        "{\"total\":%u,\"valid\":%u,\"expiring\":%u,"
        "\"expired\":%u,\"revoked\":%u,\"rotations\":%u,"
        "\"failed_rotations\":%u,\"last_rotation\":%ld}",
        s.total_certs, s.valid_certs, s.expiring_certs,
        s.expired_certs, s.revoked_certs, s.rotations_performed,
        s.rotations_failed, (long)s.last_rotation) == -1) {
        return (-1);
    }

    *json_out = json;
    return (0);
}

/*
 * Add to history
 */
int
cert_history_add(const char *name, const char *action)
{
    char path[PATH_MAX];
    FILE *fp;

    snprintf(path, sizeof(path), "%s/history.log", cert_dir);
    fp = fopen(path, "a");
    if (fp == NULL)
        return (-1);

    fprintf(fp, "%ld %s %s\n", (long)time(NULL), name, action);
    fclose(fp);

    return (0);
}

/*
 * Rotate all certificates
 */
int
cert_rotate_all(void)
{
    struct cert_info **certs;
    int count, ret = 0;

    certs = cert_list(&count);
    if (certs == NULL)
        return (-1);

    for (int i = 0; i < count; i++) {
        if (cert_renew(certs[i]->name) != 0)
            ret++;
    }

    free(certs);

    if (ret == 0)
        stats.rotations_performed++;

    return (ret);
}

/*
 * Check certificate expiry
 */
int
cert_check_expiry(const char *name, int warning_days, int critical_days)
{
    struct cert_info *cert;
    int days_until;

    cert = cert_get(name);
    if (cert == NULL)
        return (-1);

    days_until = (cert->expires - time(NULL)) / 86400;

    if (days_until <= critical_days) {
        syslog(LOG_CRIT, "Certificate CRITICAL: %s expires in %d days",
            name, days_until);
    } else if (days_until <= warning_days) {
        syslog(LOG_WARNING, "Certificate warning: %s expires in %d days",
            name, days_until);
    }

    return (0);
}

/*
 * Check all certificates
 */
int
cert_check_all(void)
{
    struct cert_info **certs;
    int count;

    certs = cert_list(&count);
    if (certs == NULL)
        return (-1);

    for (int i = 0; i < count; i++) {
        cert_verify(certs[i]->name);
        cert_check_expiry(certs[i]->name,
            rot_config.warning_days, rot_config.critical_days);
    }

    free(certs);
    return (0);
}

/*
 * Status as JSON
 */
int
cert_status_json(char **json_out)
{
    struct cert_info **certs;
    int count;
    char *buf = NULL;
    size_t bufsz = 0;
    FILE *ms;

    if (json_out == NULL)
        return (-1);

    certs = cert_list(&count);
    if (certs == NULL) {
        *json_out = strdup("[]");
        return (0);
    }

    /*
     * Build the array in one streaming pass. The previous loop asprintf'd the
     * entire accumulated JSON plus one element each iteration and freed the
     * old string — O(n^2) byte copies (the "Schlemiel the Painter" pattern).
     * open_memstream grows a single buffer, making this O(n).
     */
    ms = open_memstream(&buf, &bufsz);
    if (ms == NULL) {
        free(certs);
        return (-1);
    }
    fputc('[', ms);
    for (int i = 0; i < count; i++) {
        char ecn[sizeof(certs[i]->cn) * 6];

        fprintf(ms, "%s{\"name\":\"%s\",\"type\":%d,\"cn\":\"%s\","
            "\"status\":%d,\"expires\":%ld}",
            i > 0 ? "," : "",
            certs[i]->name, certs[i]->type,
            ocifbsd_json_escape(certs[i]->cn, ecn, sizeof(ecn)),
            certs[i]->status, (long)certs[i]->expires);
    }
    fputc(']', ms);
    fclose(ms);			/* finalizes buf/bufsz */

    free(certs);
    *json_out = buf;

    return (0);
}

/*
 * Main function
 */
int
main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <command> [args]\n", argv[0]);
        fprintf(stderr, "Commands: list, create, rotate, backup, status\n");
        return (1);
    }

    cert_init();

    if (strcmp(argv[1], "list") == 0) {
        char *json;
        cert_status_json(&json);
        printf("%s\n", json);
        free(json);
    } else if (strcmp(argv[1], "rotate") == 0) {
        cert_rotate_all();
    } else if (strcmp(argv[1], "check") == 0) {
        cert_check_all();
    } else if (strcmp(argv[1], "backup") == 0 && argc > 2) {
        cert_backup_create(argv[2]);
    }

    cert_shutdown();
    return (0);
}
