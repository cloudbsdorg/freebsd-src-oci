# Makefile for ocifbsd-logd - Log aggregation daemon
# Phase 14: Observability/Logging

SRCS=	logd.c ringbuf.c forward.c alerting.c
INC=	logd.h ringbuf.h

# SRCDIR is the ocifbsd source root (parent of this subdir).
SRCDIR?=	${.CURDIR:H}

.PATH:	${SRCDIR}/logd

PROG=	ocifbsd-logd
MAN=	ocifbsd-logd.8

# m, z, crypto, pthread are in FreeBSD base
LIBADD=	 m z crypto pthread

# json-c is a port (not in FreeBSD base). Port path + LDADD.
# Install on VM with:  sudo pkg install json-c
LDFLAGS+=	-L/usr/local/lib

CFLAGS+=	-I${SRCDIR}/include
CFLAGS+=	-I${SRCDIR}/metrics
CFLAGS+=	-I/usr/local/include
LDADD+=		-lcurl

WARNS?=	3

# json-c is vendored in-tree (contrib/json-c); no port needed.
JSONC_DIR=	${SRCDIR}/contrib/json-c
.include "${SRCDIR}/jsonc.inc.mk"

.include <bsd.prog.mk>
