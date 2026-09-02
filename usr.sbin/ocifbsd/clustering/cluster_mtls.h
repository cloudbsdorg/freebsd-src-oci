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
 *
 * expected_peer pins the peer's identity: when non-NULL, the peer certificate's
 * SAN dNSName (or CN) must equal expected_peer or the handshake fails. Chaining
 * to the cluster CA alone is NOT sufficient — without this, ANY node holding a
 * CA-issued certificate could impersonate ANY other node. A client dialing a
 * specific node MUST pass that node's name here. A server generally passes NULL
 * (it cannot know which member will connect) and authorizes the learned peer
 * identity, obtained via cluster_mtls_peer_name(), afterward.
 */
SSL_CTX	*cluster_mtls_ctx(const char *dir, const char *node_name,
	    const char *expected_peer, int is_server);

/*
 * After a successful handshake, copy the verified peer certificate's CN into
 * buf (NUL-terminated, at most buflen bytes). Returns 0 on success or -1 if no
 * verified peer certificate is present or the name does not fit. Use this on
 * the server to learn (and then authorize) which node connected.
 */
int	cluster_mtls_peer_name(SSL *ssl, char *buf, size_t buflen);

#endif /* OCIFBSD_CLUSTER_MTLS_H */
