/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * mTLS control channel: build OpenSSL contexts that mutually authenticate
 * cluster nodes against the offline cluster CA. See cluster_mtls.h.
 */

#include <limits.h>
#include <stdio.h>

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "cluster_mtls.h"

SSL_CTX *
cluster_mtls_ctx(const char *dir, const char *node_name, int is_server)
{
	char crtp[PATH_MAX], keyp[PATH_MAX], cap[PATH_MAX];
	SSL_CTX *ctx;
	int mode;

	if (dir == NULL || node_name == NULL)
		return (NULL);

	ctx = SSL_CTX_new(TLS_method());
	if (ctx == NULL)
		return (NULL);

	/* Modern TLS only. */
	if (SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION) != 1)
		goto err;

	snprintf(crtp, sizeof(crtp), "%s/%s.crt", dir, node_name);
	snprintf(keyp, sizeof(keyp), "%s/%s.key", dir, node_name);
	snprintf(cap, sizeof(cap), "%s/ca.crt", dir);

	/* This node's own identity. */
	if (SSL_CTX_use_certificate_chain_file(ctx, crtp) != 1)
		goto err;
	if (SSL_CTX_use_PrivateKey_file(ctx, keyp, SSL_FILETYPE_PEM) != 1)
		goto err;
	if (SSL_CTX_check_private_key(ctx) != 1)
		goto err;

	/* Trust only certificates issued by the cluster CA. */
	if (SSL_CTX_load_verify_locations(ctx, cap, NULL) != 1)
		goto err;

	/*
	 * Both ends verify the peer against the CA. The server additionally
	 * requires the client to present a certificate (mutual TLS); a client
	 * that offers none, or one from a foreign CA, fails the handshake.
	 */
	mode = SSL_VERIFY_PEER;
	if (is_server)
		mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
	SSL_CTX_set_verify(ctx, mode, NULL);
	SSL_CTX_set_verify_depth(ctx, 4);

	return (ctx);
err:
	SSL_CTX_free(ctx);
	return (NULL);
}
