/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Offline cluster PKI: a self-signed internal certificate authority that
 * issues per-node mTLS identities for the ocifbsd control channel. Uses the
 * FreeBSD base OpenSSL 3 library only -- no ACME, no network. See
 * cluster_pki.h for the on-disk layout.
 */

#include <sys/stat.h>

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/bn.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "cluster_pki.h"

#define PKI_CA_DAYS	3650		/* 10 years for the CA */
#define PKI_NODE_DAYS	825		/* ~27 months for node certs */

static EVP_PKEY *
pki_gen_ec_key(void)
{
	return (EVP_EC_gen("prime256v1"));
}

static int
pki_set_random_serial(X509 *cert)
{
	unsigned char bytes[16];
	BIGNUM *bn;
	int ok = -1;

	if (RAND_bytes(bytes, sizeof(bytes)) != 1)
		return (-1);
	bytes[0] &= 0x7f;			/* keep it positive */
	bn = BN_bin2bn(bytes, sizeof(bytes), NULL);
	if (bn == NULL)
		return (-1);
	if (BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(cert)) != NULL)
		ok = 0;
	BN_free(bn);
	return (ok);
}

static int
pki_add_ext(X509 *cert, X509 *issuer, int nid, const char *value)
{
	X509V3_CTX ctx;
	X509_EXTENSION *ext;

	X509V3_set_ctx_nodb(&ctx);
	X509V3_set_ctx(&ctx, issuer, cert, NULL, NULL, 0);
	ext = X509V3_EXT_conf_nid(NULL, &ctx, nid, value);
	if (ext == NULL)
		return (-1);
	X509_add_ext(cert, ext, -1);
	X509_EXTENSION_free(ext);
	return (0);
}

static int
pki_write_key(const char *path, EVP_PKEY *pkey)
{
	FILE *f = fopen(path, "w");

	if (f == NULL)
		return (-1);
	if (PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL) != 1) {
		fclose(f);
		return (-1);
	}
	fclose(f);
	return (chmod(path, 0600));
}

static int
pki_write_cert(const char *path, X509 *cert)
{
	FILE *f = fopen(path, "w");

	if (f == NULL)
		return (-1);
	if (PEM_write_X509(f, cert) != 1) {
		fclose(f);
		return (-1);
	}
	fclose(f);
	return (0);
}

static X509 *
pki_read_cert(const char *path)
{
	FILE *f = fopen(path, "r");
	X509 *cert;

	if (f == NULL)
		return (NULL);
	cert = PEM_read_X509(f, NULL, NULL, NULL);
	fclose(f);
	return (cert);
}

static EVP_PKEY *
pki_read_key(const char *path)
{
	FILE *f = fopen(path, "r");
	EVP_PKEY *pkey;

	if (f == NULL)
		return (NULL);
	pkey = PEM_read_PrivateKey(f, NULL, NULL, NULL);
	fclose(f);
	return (pkey);
}

int
cluster_pki_init_ca(const char *dir, const char *cluster_name)
{
	char keyp[PATH_MAX], crtp[PATH_MAX];
	EVP_PKEY *pkey = NULL;
	X509 *cert = NULL;
	X509_NAME *nm;
	int rc = -1;

	if (dir == NULL || cluster_name == NULL)
		return (-1);

	snprintf(keyp, sizeof(keyp), "%s/ca.key", dir);
	snprintf(crtp, sizeof(crtp), "%s/ca.crt", dir);

	/* Idempotent: keep an existing CA. */
	if (access(keyp, R_OK) == 0 && access(crtp, R_OK) == 0)
		return (0);

	pkey = pki_gen_ec_key();
	if (pkey == NULL)
		return (-1);
	cert = X509_new();
	if (cert == NULL)
		goto out;

	X509_set_version(cert, 2);
	if (pki_set_random_serial(cert) != 0)
		goto out;
	X509_gmtime_adj(X509_get_notBefore(cert), 0);
	X509_gmtime_adj(X509_get_notAfter(cert), (long)PKI_CA_DAYS * 86400L);
	X509_set_pubkey(cert, pkey);

	nm = X509_get_subject_name(cert);
	X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
	    (const unsigned char *)cluster_name, -1, -1, 0);
	X509_set_issuer_name(cert, nm);		/* self-signed */

	if (pki_add_ext(cert, cert, NID_basic_constraints, "critical,CA:TRUE") != 0)
		goto out;
	if (pki_add_ext(cert, cert, NID_key_usage,
	    "critical,keyCertSign,cRLSign,digitalSignature") != 0)
		goto out;

	if (X509_sign(cert, pkey, EVP_sha256()) == 0)
		goto out;

	if (pki_write_key(keyp, pkey) != 0)
		goto out;
	if (pki_write_cert(crtp, cert) != 0)
		goto out;
	rc = 0;
out:
	X509_free(cert);
	EVP_PKEY_free(pkey);
	return (rc);
}

int
cluster_pki_issue_node(const char *dir, const char *node_name)
{
	char cakeyp[PATH_MAX], cacrtp[PATH_MAX];
	char keyp[PATH_MAX], crtp[PATH_MAX], san[320];
	EVP_PKEY *cakey = NULL, *nkey = NULL;
	X509 *cacrt = NULL, *cert = NULL;
	X509_NAME *nm;
	int rc = -1;

	if (dir == NULL || node_name == NULL)
		return (-1);

	snprintf(cakeyp, sizeof(cakeyp), "%s/ca.key", dir);
	snprintf(cacrtp, sizeof(cacrtp), "%s/ca.crt", dir);
	snprintf(keyp, sizeof(keyp), "%s/%s.key", dir, node_name);
	snprintf(crtp, sizeof(crtp), "%s/%s.crt", dir, node_name);

	cakey = pki_read_key(cakeyp);
	cacrt = pki_read_cert(cacrtp);
	if (cakey == NULL || cacrt == NULL)
		goto out;

	nkey = pki_gen_ec_key();
	if (nkey == NULL)
		goto out;
	cert = X509_new();
	if (cert == NULL)
		goto out;

	X509_set_version(cert, 2);
	if (pki_set_random_serial(cert) != 0)
		goto out;
	X509_gmtime_adj(X509_get_notBefore(cert), 0);
	X509_gmtime_adj(X509_get_notAfter(cert), (long)PKI_NODE_DAYS * 86400L);
	X509_set_pubkey(cert, nkey);

	nm = X509_get_subject_name(cert);
	X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC,
	    (const unsigned char *)node_name, -1, -1, 0);
	/* Issuer is the CA's subject. */
	X509_set_issuer_name(cert, X509_get_subject_name(cacrt));

	if (pki_add_ext(cert, cacrt, NID_basic_constraints, "critical,CA:FALSE") != 0)
		goto out;
	if (pki_add_ext(cert, cacrt, NID_key_usage,
	    "critical,digitalSignature,keyEncipherment") != 0)
		goto out;
	/* mTLS: usable as both client and server. */
	if (pki_add_ext(cert, cacrt, NID_ext_key_usage,
	    "serverAuth,clientAuth") != 0)
		goto out;
	snprintf(san, sizeof(san), "DNS:%s", node_name);
	if (pki_add_ext(cert, cacrt, NID_subject_alt_name, san) != 0)
		goto out;

	if (X509_sign(cert, cakey, EVP_sha256()) == 0)
		goto out;

	if (pki_write_key(keyp, nkey) != 0)
		goto out;
	if (pki_write_cert(crtp, cert) != 0)
		goto out;
	rc = 0;
out:
	X509_free(cert);
	X509_free(cacrt);
	EVP_PKEY_free(nkey);
	EVP_PKEY_free(cakey);
	return (rc);
}

int
cluster_pki_verify_node(const char *dir, const char *cert_path)
{
	char cacrtp[PATH_MAX];
	X509 *cacrt = NULL, *cert = NULL;
	X509_STORE *store = NULL;
	X509_STORE_CTX *ctx = NULL;
	int rc = -1;

	if (dir == NULL || cert_path == NULL)
		return (-1);

	snprintf(cacrtp, sizeof(cacrtp), "%s/ca.crt", dir);
	cacrt = pki_read_cert(cacrtp);
	cert = pki_read_cert(cert_path);
	if (cacrt == NULL || cert == NULL)
		goto out;

	store = X509_STORE_new();
	if (store == NULL || X509_STORE_add_cert(store, cacrt) != 1)
		goto out;
	ctx = X509_STORE_CTX_new();
	if (ctx == NULL || X509_STORE_CTX_init(ctx, store, cert, NULL) != 1)
		goto out;

	rc = (X509_verify_cert(ctx) == 1) ? 0 : -1;
out:
	if (ctx != NULL)
		X509_STORE_CTX_free(ctx);
	if (store != NULL)
		X509_STORE_free(store);
	X509_free(cert);
	X509_free(cacrt);
	return (rc);
}
