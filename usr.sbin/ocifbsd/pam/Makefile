# Makefile for PAM authentication module
# Phase 13: PAM/System Credentials

SRCS=	pam_auth.c
INC=	pam_auth.h

.PATH:	${SRCDIR}/pam

<include "Makefile.inc">
<include <objfmt>.mk>

LIBADD=	 pam util json crypto z

LINKS=	${BINDIR}/ocifbsd-pam
MLINKS=	pam_auth.8 ocifbsd-pam.8

CFLAGS+=	-I${SRCDIR}/include
CFLAGS+=	-I${SRCDIR}/security-daemon

NO_WERROR=

WARNS?=	3

.include <bsd.lib.mk>
