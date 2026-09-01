/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Mutual-TLS control channel for the ocifbsd cluster.
 *
 * Both ends of a control connection authenticate each other with certificates
 * issued by the offline cluster CA (see cluster_pki.h): a client verifies the
 * server and presents its own certificate, and the server requires and
 * verifies the client certificate. A peer whose certificate does not chain to
 * the cluster CA is rejected at the handshake, so only cluster members can
 * exchange control messages.
 *
 * Built on the FreeBSD base OpenSSL 3 library; no network access is needed to
 * establish trust (the CA is local and self-signed).
 */

#ifndef OCIFBSD_CLUSTER_MTLS_H
#define OCIFBSD_CLUSTER_MTLS_H

#include <openssl/ssl.h>

/*
 * Build an SSL_CTX for the control channel using node_name's identity under
 * dir (dir/<node>.crt, dir/<node>.key) and trusting the cluster CA
 * (dir/ca.crt). is_server selects the server role (require + verify the client
 * certificate); otherwise a client context is built (verify the server and
 * present the client certificate). Returns a new SSL_CTX (caller frees with
 * SSL_CTX_free) or NULL on error.
 */
SSL_CTX	*cluster_mtls_ctx(const char *dir, const char *node_name, int is_server);

#endif /* OCIFBSD_CLUSTER_MTLS_H */
