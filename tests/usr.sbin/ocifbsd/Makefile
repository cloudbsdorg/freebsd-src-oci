# $Id$
# $FreeBSD$
# SPDX-License-Identifier: BSD-2-Clause

# Test suite for ocifbsd
#
# Tests live next to the source-under-test as a sibling
# *.c file (parser_test.c, k8s_test.c). The original
# `ocifbsd_test.c` is preserved as `.disabled` — it
# referenced functions (ocifbsd_generate_cid,
# ocifbsd_validate_name, ocifbsd_oci_to_jail_params,
# ocifbsd_log_init, ocifbsd_run_hooks, ...) that
# never landed in the source. It can be revived
# once those functions are implemented.

ATF_TESTS_C=	parser_test k8s_test
# SRCS.${test} is auto-derived as ${test}.c by atf.test.mk.

# The source-under-test (parser.c, k8s.c) has pre-existing warnings
# (-Wcast-qual, -Wunused-parameter on the native_format_* helpers)
# that fail under the default test WARNS=6 + -Werror. The convert/
# Makefile compiles these files at WARNS=3. Match that for the
# tests so we can #include the source directly (needed to test
# static helpers like k8s_detect_kind and yaml_get_field).
WARNS=		3

# Tests #include the source-under-test from convert/.
# The test dir is tests/usr.sbin/ocifbsd/ (3 levels deep from repo root)
# so we need 3 `..` to escape to the repo root, then descend into
# usr.sbin/ocifbsd/.
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd
CFLAGS+=	-I${.CURDIR}/../../../usr.sbin/ocifbsd/convert

.include <bsd.test.mk>
