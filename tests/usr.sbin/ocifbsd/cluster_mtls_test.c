/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Unit tests for the mTLS control channel (clustering/cluster_mtls.c),
 * exercising a real handshake over a socketpair with the cluster PKI.
 */

#include <atf-c.h>
#include <sys/socket.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include "clustering/cluster_pki.c"
#include "clustering/cluster_mtls.c"

struct hs {
	SSL_CTX	*ctx;
	int	 fd;
	int	 server;
	int	 ok;		/* SSL_accept/connect returned 1 */
	long	 verify;	/* SSL_get_verify_result */
};

static void *
hs_thread(void *arg)
{
	struct hs *h = arg;
	SSL *ssl = SSL_new(h->ctx);

	if (ssl == NULL)
		return (NULL);
	SSL_set_fd(ssl, h->fd);
	h->ok = ((h->server ? SSL_accept(ssl) : SSL_connect(ssl)) == 1);
	h->verify = SSL_get_verify_result(ssl);
	SSL_shutdown(ssl);
	SSL_free(ssl);
	return (NULL);
}

/* Run a handshake between a server ctx and a client ctx; fill results. */
static void
run_handshake(SSL_CTX *sctx, SSL_CTX *cctx, struct hs *s, struct hs *c)
{
	int fds[2];
	pthread_t ts, tc;

	ATF_REQUIRE_EQ(0, socketpair(AF_UNIX, SOCK_STREAM, 0, fds));
	s->ctx = sctx; s->fd = fds[0]; s->server = 1; s->ok = 0; s->verify = -1;
	c->ctx = cctx; c->fd = fds[1]; c->server = 0; c->ok = 0; c->verify = -1;
	pthread_create(&ts, NULL, hs_thread, s);
	pthread_create(&tc, NULL, hs_thread, c);
	pthread_join(ts, NULL);
	pthread_join(tc, NULL);
	close(fds[0]);
	close(fds[1]);
}

/* Two nodes with certs from the same cluster CA complete mutual TLS. */
ATF_TC_WITHOUT_HEAD(mtls_same_ca_handshake);
ATF_TC_BODY(mtls_same_ca_handshake, tc)
{
	char dir[] = "mtls.XXXXXX";
	SSL_CTX *sctx, *cctx;
	struct hs s, c;

	ATF_REQUIRE(mkdtemp(dir) != NULL);
	ATF_REQUIRE_EQ(0, cluster_pki_init_ca(dir, "cluster"));
	ATF_REQUIRE_EQ(0, cluster_pki_issue_node(dir, "server"));
	ATF_REQUIRE_EQ(0, cluster_pki_issue_node(dir, "client"));

	sctx = cluster_mtls_ctx(dir, "server", 1);
	cctx = cluster_mtls_ctx(dir, "client", 0);
	ATF_REQUIRE(sctx != NULL);
	ATF_REQUIRE(cctx != NULL);

	run_handshake(sctx, cctx, &s, &c);
	ATF_CHECK_MSG(s.ok, "server handshake failed");
	ATF_CHECK_MSG(c.ok, "client handshake failed");
	ATF_CHECK_EQ_MSG(X509_V_OK, s.verify, "server did not verify client");
	ATF_CHECK_EQ_MSG(X509_V_OK, c.verify, "client did not verify server");

	SSL_CTX_free(sctx);
	SSL_CTX_free(cctx);
}

/* A client whose cert is from a different CA is rejected by the server. */
ATF_TC_WITHOUT_HEAD(mtls_foreign_client_rejected);
ATF_TC_BODY(mtls_foreign_client_rejected, tc)
{
	char dirA[] = "mtlsA.XXXXXX";
	char dirB[] = "mtlsB.XXXXXX";
	SSL_CTX *sctx, *cctx;
	struct hs s, c;

	ATF_REQUIRE(mkdtemp(dirA) != NULL);
	ATF_REQUIRE(mkdtemp(dirB) != NULL);
	ATF_REQUIRE_EQ(0, cluster_pki_init_ca(dirA, "cluster-a"));
	ATF_REQUIRE_EQ(0, cluster_pki_issue_node(dirA, "server"));
	ATF_REQUIRE_EQ(0, cluster_pki_init_ca(dirB, "cluster-b"));
	ATF_REQUIRE_EQ(0, cluster_pki_issue_node(dirB, "intruder"));

	sctx = cluster_mtls_ctx(dirA, "server", 1);	/* trusts CA-A */
	cctx = cluster_mtls_ctx(dirB, "intruder", 0);	/* cert from CA-B */
	ATF_REQUIRE(sctx != NULL && cctx != NULL);

	run_handshake(sctx, cctx, &s, &c);
	/* The server must NOT accept a client blessed only by a foreign CA. */
	ATF_CHECK_MSG(!s.ok, "server accepted a foreign client certificate");

	SSL_CTX_free(sctx);
	SSL_CTX_free(cctx);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, mtls_same_ca_handshake);
	ATF_TP_ADD_TC(tp, mtls_foreign_client_rejected);
	return (atf_no_error());
}
