#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 displayd-reconcile-wave1 contributors
#
# Build the ATF C test program that exercises the per-instance console API
# introduced by T8.A and implemented by T8.B.  Until T8.B lands the test
# sources reference undeclared symbols and the build will fail; that is the
# expected TDD-red outcome (see .sisyphus/evidence/task-8-atf-compile.txt).
#

TESTSDIR=	${TESTSBASE}/sys/bhyve

ATF_TESTS_C=	atf_console

SRCS.atf_console+=	atf_console.c
SRCS.atf_console+=	${SRCTOP}/usr.sbin/bhyve/console.c
SRCS.atf_console+=	${SRCTOP}/usr.sbin/bhyve/bhyvegc.c

CFLAGS.atf_console+=	-I${SRCTOP}/usr.sbin/bhyve

# The suite is unit-level: no fork/exec, no network, no filesystem access.
TEST_METADATA+=	timeout="60"

.include <bsd.test.mk>
