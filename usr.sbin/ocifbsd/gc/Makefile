# Makefile for ocifbsd-gc - Garbage collection daemon
# Phase 15: Garbage Collection

SRCS=	gc.c container.c image.c volume.c network.c cluster.c
INC=	gc.h

.PATH:	${SRCDIR}/gc

<include "Makefile.inc">
<include <objfmt>.mk>

PROG=	ocifbsd-gc
MAN=	ocifbsd-gc.8

LIBADD=	 m json z crypto pthread util

CFLAGS+=	-I${SRCDIR}/include

WARNS?=	3

.include <bsd.prog.mk>
