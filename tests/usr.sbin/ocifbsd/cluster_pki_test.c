/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Unit tests for the offline cluster PKI (clustering/cluster_pki.c).
 */

#include <atf-c.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/pem.h>

#include "clustering/cluster_pki.c"

/* Load an X509 from a PEM file (test helper). */
static X509 *
load_cert(const char *path)
{
	FILE *f = fopen(path, "r");
	X509 *c;

	if (f == NULL)
		return (NULL);
	c = PEM_read_X509(f, NULL, NULL, NULL);
	fclose(f);
	return (c);
}

/* A CA plus a node identity are created and the node chains to the CA. */
ATF_TC_WITHOUT_HEAD(pki_issue_and_verify);
ATF_TC_BODY(pki_issue_and_verify, tc)
{
	char dir[] = "pki.XXXXXX";
	char capath[256], keypath[256], crtpath[256];

	ATF_REQUIRE(mkdtemp(dir) != NULL);

	ATF_REQUIRE_EQ(0, cluster_pki_init_ca(dir, "test-cluster"));
	snprintf(capath, sizeof(capath), "%s/ca.crt", dir);
	ATF_CHECK_MSG(access(capath, R_OK) == 0, "ca.crt not created");

	ATF_REQUIRE_EQ(0, cluster_pki_issue_node(dir, "node1"));
	snprintf(keypath, sizeof(keypath), "%s/node1.key", dir);
	snprintf(crtpath, sizeof(crtpath), "%s/node1.crt", dir);
	ATF_CHECK_MSG(access(keypath, R_OK) == 0, "node key not created");
	ATF_CHECK_MSG(access(crtpath, R_OK) == 0, "node cert not created");

	/* The node cert must chain to this CA. */
	ATF_CHECK_EQ_MSG(0, cluster_pki_verify_node(dir, crtpath),
	    "node cert does not verify against its own CA");
}

/* The node identity is valid for both client and server auth (mTLS). */
ATF_TC_WITHOUT_HEAD(pki_node_has_mtls_eku);
ATF_TC_BODY(pki_node_has_mtls_eku, tc)
{
	char dir[] = "pki.XXXXXX";
	char crtpath[256];
	X509 *cert;
	int server = 0, client = 0;
	EXTENDED_KEY_USAGE *eku;

	ATF_REQUIRE(mkdtemp(dir) != NULL);
	ATF_REQUIRE_EQ(0, cluster_pki_init_ca(dir, "test-cluster"));
	ATF_REQUIRE_EQ(0, cluster_pki_issue_node(dir, "node1"));

	snprintf(crtpath, sizeof(crtpath), "%s/node1.crt", dir);
	cert = load_cert(crtpath);
	ATF_REQUIRE(cert != NULL);

	eku = X509_get_ext_d2i(cert, NID_ext_key_usage, NULL, NULL);
	ATF_REQUIRE_MSG(eku != NULL, "no extended key usage on node cert");
	for (int i = 0; i < sk_ASN1_OBJECT_num(eku); i++) {
		int nid = OBJ_obj2nid(sk_ASN1_OBJECT_value(eku, i));
		if (nid == NID_server_auth)
			server = 1;
		if (nid == NID_client_auth)
			client = 1;
	}
	EXTENDED_KEY_USAGE_free(eku);
	X509_free(cert);
	ATF_CHECK_MSG(server, "node cert missing serverAuth EKU");
	ATF_CHECK_MSG(client, "node cert missing clientAuth EKU");
}

/* A certificate issued by a different CA must NOT verify (rejects impostors). */
ATF_TC_WITHOUT_HEAD(pki_rejects_foreign_cert);
ATF_TC_BODY(pki_rejects_foreign_cert, tc)
{
	char dirA[] = "pkiA.XXXXXX";
	char dirB[] = "pkiB.XXXXXX";
	char foreign[256];

	ATF_REQUIRE(mkdtemp(dirA) != NULL);
	ATF_REQUIRE(mkdtemp(dirB) != NULL);

	ATF_REQUIRE_EQ(0, cluster_pki_init_ca(dirA, "cluster-a"));
	ATF_REQUIRE_EQ(0, cluster_pki_init_ca(dirB, "cluster-b"));
	ATF_REQUIRE_EQ(0, cluster_pki_issue_node(dirB, "intruder"));

	snprintf(foreign, sizeof(foreign), "%s/intruder.crt", dirB);
	/* cluster-a must reject a cert blessed only by cluster-b. */
	ATF_CHECK_MSG(cluster_pki_verify_node(dirA, foreign) != 0,
	    "a foreign CA's certificate was accepted");
}

/* init_ca is idempotent: a second call keeps the same CA. */
ATF_TC_WITHOUT_HEAD(pki_init_idempotent);
ATF_TC_BODY(pki_init_idempotent, tc)
{
	char dir[] = "pki.XXXXXX";
	char capath[256];
	X509 *first, *second;

	ATF_REQUIRE(mkdtemp(dir) != NULL);
	ATF_REQUIRE_EQ(0, cluster_pki_init_ca(dir, "test-cluster"));
	snprintf(capath, sizeof(capath), "%s/ca.crt", dir);
	first = load_cert(capath);
	ATF_REQUIRE(first != NULL);

	ATF_REQUIRE_EQ(0, cluster_pki_init_ca(dir, "test-cluster"));
	second = load_cert(capath);
	ATF_REQUIRE(second != NULL);

	ATF_CHECK_MSG(X509_cmp(first, second) == 0,
	    "init_ca overwrote an existing CA");
	X509_free(first);
	X509_free(second);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, pki_issue_and_verify);
	ATF_TP_ADD_TC(tp, pki_node_has_mtls_eku);
	ATF_TP_ADD_TC(tp, pki_rejects_foreign_cert);
	ATF_TP_ADD_TC(tp, pki_init_idempotent);
	return (atf_no_error());
}
