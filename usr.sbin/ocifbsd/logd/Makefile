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

CFLAGS+=	-I${SRCDIR}/include
CFLAGS+=	-I${SRCDIR}/metrics

WARNS?=	3

# json-c and libcurl are vendored in-tree (contrib/); no ports needed.
# libcurl is the remote log-forwarding (forward.c) HTTP transport.
JSONC_DIR=	${SRCDIR}/contrib/json-c
.include "${SRCDIR}/jsonc.inc.mk"
CURL_DIR=	${SRCDIR}/contrib/curl
.include "${SRCDIR}/curl.inc.mk"

.include <bsd.prog.mk>
