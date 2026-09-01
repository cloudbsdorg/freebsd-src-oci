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

ATF_TESTS_C=	parser_test ensemble_test utils_test oci2jail_test hooks_test \
		image_ref_test whiteout_test image_parse_test unpack_layer_test \
		netcfg_test jsonfmt_test logd_forward_test registries_test \
		lb_test sched_test sched_sync_test \
		cluster_pki_test control_plane_test \
		cluster_mtls_test
ATF_TESTS_SH=	cli_test lifecycle_test

# convert/ sources compile at WARNS=3; match for #include tests
WARNS=		3

# Paths into usr.sbin/ocifbsd from tests/usr.sbin/ocifbsd
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/include
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/convert
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/src
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/image
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/orchestration

# utils_test includes src/utils.c (needs libmd for SHA256_Data)
LIBADD.utils_test+=	md

# oci2jail_test includes src/oci2jail.c (json-c port + libjail)
CFLAGS.oci2jail_test+=	-I/usr/local/include/json-c
LDFLAGS.oci2jail_test+=	-L/usr/local/lib
LDADD.oci2jail_test+=	-ljson-c
LIBADD.oci2jail_test+=	jail

# hooks_test includes src/hooks.c (json-c for completeness)
CFLAGS.hooks_test+=	-I/usr/local/include/json-c
LDFLAGS.hooks_test+=	-L/usr/local/lib
LDADD.hooks_test+=	-ljson-c

# image_parse_test #includes pull.c + unpack + json-c/curl/md/archive
CFLAGS.image_parse_test+=	-I/usr/local/include -I/usr/local/include/json-c
LDFLAGS.image_parse_test+=	-L/usr/local/lib
LDADD.image_parse_test+=	-lcurl -ljson-c
LIBADD.image_parse_test+=	md archive

# netcfg_test #includes network/netcfg.c (json-c for (de)serialization)
CFLAGS.netcfg_test+=	-I/usr/local/include/json-c
LDFLAGS.netcfg_test+=	-L/usr/local/lib
LDADD.netcfg_test+=	-ljson-c

# jsonfmt_test #includes src/jsonfmt.c (json-c pretty-printer)
CFLAGS.jsonfmt_test+=	-I/usr/local/include/json-c
LDFLAGS.jsonfmt_test+=	-L/usr/local/lib
LDADD.jsonfmt_test+=	-ljson-c

# unpack_layer_test #includes unpack.c + whiteout.c
CFLAGS.unpack_layer_test+=	-I/usr/local/include
LDFLAGS.unpack_layer_test+=	-L/usr/local/lib
LIBADD.unpack_layer_test+=	archive

# logd_forward_test #includes logd/forward.c (libcurl HTTP POST) and needs the
# logd/ include dir plus curl; the in-process HTTP sink uses pthreads.
CFLAGS.logd_forward_test+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/logd \
				-I/usr/local/include
LDFLAGS.logd_forward_test+=	-L/usr/local/lib
LDADD.logd_forward_test+=	-lcurl
LIBADD.logd_forward_test+=	pthread

# cluster_pki_test #includes clustering/cluster_pki.c (OpenSSL X509)
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/clustering
LIBADD.cluster_pki_test+=	crypto ssl
LIBADD.cluster_mtls_test+=	crypto ssl pthread

.include <bsd.test.mk>
