#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 displayd-reconcile-wave1 contributors
#
# Build the ATF C test program that exercises the per-instance console API
# introduced by T8.A and implemented by T8.B.  The test sources are
# compiled in-tree against console.c and bhyvegc.c (the lower-level pixel
# buffer primitive).
#

TESTSDIR=	${TESTSBASE}/sys/bhyve

ATF_TESTS_C=	atf_console

SRCS.atf_console=	atf_console.c
# Pull in the in-tree console + bhyvegc as additional compile units.  These
# are pure pixel-buffer / per-instance state with no VMM coupling (T1
# audit), so a direct in-process link is safe for unit tests.  We
# override bsd.dep.mk's "$OBJS inside $SRCTOP not allowed" check because
# the tests legitimately need to reach into usr.sbin/bhyve/ to compile
# against the impl under test.
_ALLOW_ABSOLUTE_OBJ_PATH=	yes
SRCS.atf_console+=	${SRCTOP}/usr.sbin/bhyve/console.c
SRCS.atf_console+=	${SRCTOP}/usr.sbin/bhyve/bhyvegc.c

CFLAGS.atf_console+=	-I${SRCTOP}/usr.sbin/bhyve
# Required for console.c's sys/queue.h STAILQ usage.
CFLAGS.atf_console+=	-I${SRCTOP}/sys

# The suite is unit-level: no fork/exec, no network, no filesystem access.
TEST_METADATA+=	timeout=60

.include <bsd.test.mk>
