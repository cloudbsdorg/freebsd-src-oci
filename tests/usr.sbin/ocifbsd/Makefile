# $FreeBSD$
# SPDX-License-Identifier: BSD-2-Clause
#
# Test suite for ocifbsd
#
# - parser_test / k8s_test: convert module pure helpers
# - utils_test: src/utils.c pure helpers (no jail)
# - cli_test: atf_check against built ocifbsd binary
#
# ocifbsd_test.c.disabled is preserved for history; it referenced
# APIs that never landed.

ATF_TESTS_C=	parser_test k8s_test utils_test oci2jail_test hooks_test \
		image_ref_test whiteout_test
ATF_TESTS_SH=	cli_test lifecycle_test

# convert/ sources compile at WARNS=3; match for #include tests
WARNS=		3

# Paths into usr.sbin/ocifbsd from tests/usr.sbin/ocifbsd
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/include
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/convert
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/src
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/image

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

.include <bsd.test.mk>
