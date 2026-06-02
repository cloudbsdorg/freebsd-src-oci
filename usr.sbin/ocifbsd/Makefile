# $Id$
# $FreeBSD$
# SPDX-License-Identifier: BSD-2-Clause

PROG=	ocifbsd
VERSION=	0.1.0
MAN=	ocifbsd.8

SRCS=	\
	ocifbsd.c \
	src/container.c \
	src/oci2jail.c \
	src/state.c \
	src/hooks.c \
	src/utils.c

# Module subdirectories
SUBDIR+=	api
SUBDIR+=	cert
SUBDIR+=	clustering
SUBDIR+=	convert
SUBDIR+=	export
SUBDIR+=	gc
SUBDIR+=	image
SUBDIR+=	logd
SUBDIR+=	metrics
SUBDIR+=	namespace
SUBDIR+=	network
SUBDIR+=	orchestration
SUBDIR+=	pam
SUBDIR+=	security
SUBDIR+=	security-daemon
SUBDIR+=	tpm

CFLAGS+=	-I${.CURDIR}/include
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-Wall -Wextra
CFLAGS+=	-D_XOPEN_SOURCE=700

LIBADD=	\
	jail \
	util \
	zfs \
	m \
	pthread \
	crypto \
	json-c

# Enable test suite
HAS_TESTS=
SUBDIR.${MK_TESTS}+=	tests

.include <src.opts.mk>
.include <bsd.prog.mk>
