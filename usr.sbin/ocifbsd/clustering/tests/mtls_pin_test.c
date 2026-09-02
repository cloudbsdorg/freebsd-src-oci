/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Regression test: cluster mTLS must pin the peer's identity, not merely
 * verify that the peer certificate chains to the cluster CA. A client that
 * dials "node-a" must reject a (CA-valid) certificate presented for a
 * different node name.
 *
 * The test stands up a real TLS handshake between two SSL objects connected by
 * a socketpair: a server presenting node-a's certificate, and a client that
 * pins a configurable expected-peer name. Pinning the correct name must
 * succeed; pinning a different name must fail the handshake.
 */

#include <sys/socket.h>
#include <sys/wait.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "../cluster_pki.h"
#include "../cluster_mtls.h"

static int failures;

static void
check(const char *name, int cond)
{
	printf("%s: %s\n", cond ? "PASS" : "FAIL", name);
	if (!cond)
		failures++;
}

/*
 * Drive a full handshake between a server (node-a) and a client pinning
 * expected_peer, over an AF_UNIX socketpair, each side in its own process.
 * Returns 0 if BOTH sides completed the handshake, non-zero otherwise.
 */
static int
handshake(const char *dir, const char *expected_peer)
{
	int sv[2];
	pid_t pid;
	int status;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return (-1);

	pid = fork();
	if (pid < 0)
		return (-1);

	if (pid == 0) {
		/* Child: server presenting node-a. */
		SSL_CTX *ctx = cluster_mtls_ctx(dir, "node-a", NULL, 1);
		SSL *ssl;
		int ok = 0;

		close(sv[0]);
		if (ctx != NULL) {
			ssl = SSL_new(ctx);
			SSL_set_fd(ssl, sv[1]);
			ok = (SSL_accept(ssl) == 1);
			SSL_free(ssl);
			SSL_CTX_free(ctx);
		}
		close(sv[1]);
		_exit(ok ? 0 : 1);
	}

	/* Parent: client pinning expected_peer, presenting node-b. */
	SSL_CTX *ctx = cluster_mtls_ctx(dir, "node-b", expected_peer, 0);
	SSL *ssl;
	int client_ok = 0;

	close(sv[1]);
	if (ctx != NULL) {
		ssl = SSL_new(ctx);
		SSL_set_fd(ssl, sv[0]);
		client_ok = (SSL_connect(ssl) == 1);
		SSL_free(ssl);
		SSL_CTX_free(ctx);
	}
	close(sv[0]);

	waitpid(pid, &status, 0);
	/* Success only if the client handshake completed. */
	return (client_ok ? 0 : -1);
}

int
main(void)
{
	char dir[] = "/tmp/ocifbsd-mtls-test.XXXXXX";

	SSL_library_init();

	if (mkdtemp(dir) == NULL) {
		perror("mkdtemp");
		return (2);
	}
	if (cluster_pki_init_ca(dir, "test-cluster") != 0 ||
	    cluster_pki_issue_node(dir, "node-a") != 0 ||
	    cluster_pki_issue_node(dir, "node-b") != 0) {
		printf("FAIL: could not set up test PKI\n");
		return (2);
	}

	/* Correct pin: client dials node-a, server IS node-a -> succeeds. */
	check("handshake succeeds when pinned peer matches (node-a)",
	    handshake(dir, "node-a") == 0);

	/* Wrong pin: client dials node-b, server presents node-a -> fails. */
	check("handshake REJECTED when pinned peer differs (node-b)",
	    handshake(dir, "node-b") != 0);

	printf("%s (%d failure%s)\n", failures ? "TESTS FAILED" : "ALL PASSED",
	    failures, failures == 1 ? "" : "s");
	return (failures ? 1 : 0);
}
