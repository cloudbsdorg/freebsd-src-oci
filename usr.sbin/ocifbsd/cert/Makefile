# Makefile for Certificate Management
# Phase 16: Certificate Management

SRCS=	cert.c rotation.c backup.c recovery.c acme.c external_ca.c
INC=	cert.h

.PATH:	${SRCDIR}/cert

<include "Makefile.inc">
<include <objfmt>.mk>

PROG=	ocifbsd-cert
MAN=	ocifbsd-cert.8

LIBADD=	 crypto ssl json z pthread util

CFLAGS+=	-I${SRCDIR}/include
CFLAGS+=	-I${SRCDIR}/tpm

WARNS?=	3

.include <bsd.prog.mk>
