# Makefile for Cloud Export module
# Phase 17: Cloud Export

SRCS=	export.c aws.c gcp.c azure.c network.c jail.c
INC=	export.h

# SRCDIR is the ocifbsd source root (parent of this subdir).
SRCDIR?=	${.CURDIR:H}

.PATH:	${SRCDIR}/export

PROG=	ocifbsd-export
MAN=	ocifbsd-export.8

# m, z, crypto, pthread, util, md are in FreeBSD base (md for SHA256_*)
LIBADD=	 m z crypto pthread util md

CFLAGS+=	-I${SRCDIR}/include

WARNS?=	3

# json-c is vendored in-tree (contrib/json-c); no port needed.
JSONC_DIR=	${SRCDIR}/contrib/json-c
.include "${SRCDIR}/jsonc.inc.mk"

.include <bsd.prog.mk>
