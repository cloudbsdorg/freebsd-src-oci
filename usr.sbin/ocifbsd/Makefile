# $Id$
# $FreeBSD$
# SPDX-License-Identifier: BSD-2-Clause

PROG=	ocifbsd
VERSION=	0.1.0
MAN=	ocifbsd.8

SRCS=	\
	ocifbsd.c \
	container.c \
	oci2jail.c \
	state.c \
	hooks.c \
	utils.c

# Orchestration module
SUBDIR+=	orchestration

# Image management module
SUBDIR+=	image

# Network module
SUBDIR+=	network

# Security module
SUBDIR+=	security

CFLAGS+=	-I${.CURDIR}/include
CFLAGS+=	-Wall -Wextra -Werror
CFLAGS+=	-D_XOPEN_SOURCE=700

LIBADD=	\
	jail \
	util \
	zfs \
	m

# Enable test suite
HAS_TESTS=
SUBDIR.${MK_TESTS}+=	tests

.include <src.opts.mk>
.include <bsd.prog.mk>
