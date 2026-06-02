# Makefile for ocifbsd-logd - Log aggregation daemon
# Phase 14: Observability/Logging

SRCS=	logd.c ringbuf.c forward.c alerting.c
INC=	logd.h ringbuf.h

.PATH:	${SRCDIR}/logd

<include "Makefile.inc">
<include <objfmt>.mk>

PROG=	ocifbsd-logd
MAN=	ocifbsd-logd.8

LIBADD=	 m json z crypto pthread

CFLAGS+=	-I${SRCDIR}/include
CFLAGS+=	-I${SRCDIR}/metrics

WARNS?=	3

.include <bsd.prog.mk>
