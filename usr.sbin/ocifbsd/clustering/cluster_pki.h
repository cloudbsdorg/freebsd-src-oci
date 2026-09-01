/*-
 * Copyright (c) 2026 REVYTECH, Inc.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * This software was developed by REVYTECH, Inc.
 *
 * Cluster PKI: a self-signed internal certificate authority for the ocifbsd
 * control plane, used to mutually authenticate nodes over the mTLS control
 * channel. It is entirely self-contained and offline -- no ACME, no internet,
 * no external CA -- so a cluster can be brought up on an air-gapped network.
 *
 * All material lives under one directory:
 *   <dir>/ca.key   the CA private key (mode 0600)
 *   <dir>/ca.crt   the self-signed CA certificate
 *   <dir>/<node>.key, <dir>/<node>.crt   a node's mTLS identity, signed by
 *                                        the CA, valid for both client and
 *                                        server authentication.
 */

#ifndef OCIFBSD_CLUSTER_PKI_H
#define OCIFBSD_CLUSTER_PKI_H

/*
 * Create the cluster CA under dir if it does not already exist (idempotent).
 * cluster_name becomes the CA subject CN. Returns 0 on success (including when
 * the CA already exists), -1 on error.
 */
int	cluster_pki_init_ca(const char *dir, const char *cluster_name);

/*
 * Issue an mTLS identity for node_name under dir, signed by the cluster CA.
 * The certificate carries subjectAltName DNS:<node_name> and extended key
 * usage serverAuth + clientAuth. Overwrites any existing identity for the
 * node. Returns 0 on success, -1 on error (including a missing CA).
 */
int	cluster_pki_issue_node(const char *dir, const char *node_name);

/*
 * Verify that the certificate at cert_path chains to the cluster CA under dir.
 * Returns 0 if the certificate is valid and issued by this CA, -1 otherwise.
 */
int	cluster_pki_verify_node(const char *dir, const char *cert_path);

#endif /* OCIFBSD_CLUSTER_PKI_H */
