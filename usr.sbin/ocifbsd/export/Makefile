# Makefile for Cloud Export module
# Phase 17: Cloud Export

SRCS=	export.c aws.c gcp.c azure.c network.c jail.c
INC=	export.h

.PATH:	${SRCDIR}/export

<include "Makefile.inc">
<include <objfmt>.mk>

PROG=	ocifbsd-export
MAN=	ocifbsd-export.8

LIBADD=	 m json z crypto pthread util

CFLAGS+=	-I${SRCDIR}/include

WARNS?=	3

.include <bsd.prog.mk>
