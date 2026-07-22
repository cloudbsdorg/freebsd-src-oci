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

ATF_TESTS_C=	parser_test k8s_test utils_test
ATF_TESTS_SH=	cli_test

# convert/ sources compile at WARNS=3; match for #include tests
WARNS=		3

# Paths into usr.sbin/ocifbsd from tests/usr.sbin/ocifbsd
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/include
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/convert

# utils_test includes src/utils.c (needs libmd for SHA256_Data)
LDADD.utils_test+=	-lmd

.include <bsd.test.mk>
