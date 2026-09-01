# Makefile for ocifbsd-gc - Garbage collection daemon
# Phase 15: Garbage Collection

SRCS=	gc.c container.c image.c volume.c network.c cluster.c
INC=	gc.h

# SRCDIR is the ocifbsd source root (parent of this subdir).
SRCDIR?=	${.CURDIR:H}

.PATH:	${SRCDIR}/gc

PROG=	ocifbsd-gc
MAN=	ocifbsd-gc.8

# m, z, crypto, pthread, util are in FreeBSD base
LIBADD=	 m z crypto pthread util

CFLAGS+=	-I${SRCDIR}/include

WARNS?=	3

# json-c is vendored in-tree (contrib/json-c); no port needed.
JSONC_DIR=	${SRCDIR}/contrib/json-c
.include "${SRCDIR}/jsonc.inc.mk"

.include <bsd.prog.mk>
