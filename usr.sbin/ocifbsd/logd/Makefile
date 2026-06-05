# Makefile for ocifbsd-logd - Log aggregation daemon
# Phase 14: Observability/Logging

SRCS=	logd.c ringbuf.c forward.c alerting.c
INC=	logd.h ringbuf.h

.PATH:	${SRCDIR}/logd

PROG=	ocifbsd-logd
MAN=	ocifbsd-logd.8

# m, z, crypto, pthread are in FreeBSD base
LIBADD=	 m z crypto pthread

# json-c is a port (not in FreeBSD base). Port path + LDADD.
# Install on VM with:  sudo pkg install json-c
CFLAGS+=	-I/usr/local/include/json-c
LDFLAGS+=	-L/usr/local/lib
LDADD+=		-ljson-c

CFLAGS+=	-I${SRCDIR}/include
CFLAGS+=	-I${SRCDIR}/metrics
CFLAGS+=	-I/usr/local/include
LDADD+=		-lcurl

WARNS?=	3

.include <bsd.prog.mk>
