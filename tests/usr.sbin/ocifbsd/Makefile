# $FreeBSD$
# SPDX-License-Identifier: BSD-2-Clause
#
# Test suite for ocifbsd
#
# Unit (C ATF) — sources #include'd like image_ref_test (no SRCS from
# outside the test dir; FreeBSD bsd.dep.mk forbids $OBJS under $SRCTOP):
#   parser_test / ensemble_test     — convert module
#   utils_test                 — src/utils.c pure helpers
#   oci2jail_test / hooks_test — runtime/spec helpers
#   image_ref_test             — reference + paths (no network)
#   whiteout_test              — whiteout name helpers
#   image_parse_test           — manifest/config/digest/registry_init
#   unpack_layer_test          — real tar unpack + dirname safety
#
# Integration (shell ATF):
#   cli_test                   — CLI smoke + failure matrix
#   lifecycle_test             — create/start/kill/delete (root)
#
# ocifbsd_test.c.disabled is preserved for history; it referenced
# APIs that never landed.
#
# Self-contained: like the ocifbsd product build, the tests link json-c and
# libcurl from the vendored in-tree copies under usr.sbin/ocifbsd/contrib/
# (via jsonc.inc.mk / curl.inc.mk) and otherwise only against FreeBSD base
# libraries. No ports (/usr/local) are referenced, so the suite builds and
# runs on a stock base system.

TESTSDIR=	${TESTSBASE}/usr.sbin/ocifbsd

ATF_TESTS_C=	parser_test ensemble_test utils_test oci2jail_test hooks_test \
		image_ref_test whiteout_test image_parse_test unpack_layer_test \
		netcfg_test jsonfmt_test logd_forward_test registries_test \
		lb_test sched_test sched_sync_test \
		cluster_pki_test control_plane_test \
		cluster_mtls_test node_agent_test \
		controller_test pipeline_test rctl_test
ATF_TESTS_SH=	cli_test lifecycle_test

# convert/ sources compile at WARNS=3; match for #include tests
WARNS=		3

# ocifbsd source root (parent tree the tests reach into).
SRCDIR=		${.CURDIR}/../../../usr.sbin/ocifbsd

# Paths into usr.sbin/ocifbsd from tests/usr.sbin/ocifbsd
CFLAGS+=	-I${SRCDIR}
CFLAGS+=	-I${SRCDIR}/include
CFLAGS+=	-I${SRCDIR}/convert
CFLAGS+=	-I${SRCDIR}/src
CFLAGS+=	-I${SRCDIR}/image
CFLAGS+=	-I${SRCDIR}/orchestration
CFLAGS+=	-I${SRCDIR}/clustering
CFLAGS+=	-I${SRCDIR}/logd
CFLAGS+=	-I${SRCDIR}/security

# Vendored json-c and libcurl (contrib/), identical to the product build.
# These add the include paths and link the in-tree static archives, so the
# tests that #include json-c- or curl-using sources (oci2jail.c, hooks.c,
# netcfg.c, jsonfmt.c, pull.c, logd/forward.c) need no port.
JSONC_DIR=	${SRCDIR}/contrib/json-c
.include "${SRCDIR}/jsonc.inc.mk"
CURL_DIR=	${SRCDIR}/contrib/curl
.include "${SRCDIR}/curl.inc.mk"

# Per-test base-system libraries (all in FreeBSD base):
#   utils_test        — libmd for SHA256_Data
#   oci2jail_test     — libjail
#   image_parse_test  — libmd (digests) + libarchive (layer unpack)
#   unpack_layer_test — libarchive
#   logd_forward_test — pthread (in-process HTTP sink)
#   cluster_pki/mtls  — OpenSSL (base)
LIBADD.utils_test+=		md
LIBADD.oci2jail_test+=		jail
LIBADD.image_parse_test+=	md archive
LIBADD.unpack_layer_test+=	archive
LIBADD.logd_forward_test+=	pthread
LIBADD.cluster_pki_test+=	crypto ssl
LIBADD.cluster_mtls_test+=	crypto ssl pthread

.include <bsd.test.mk>
