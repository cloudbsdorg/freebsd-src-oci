# Makefile for PAM authentication module
# Phase 13: PAM/System Credentials

SRCS=	pam_auth.c
INC=	pam_auth.h

# pam_auth.c is a PAM module (shared library), not a program — it
# has no main(). The previous incarnation declared PROG= and used
# bsd.lib.mk + LINKS=, which is contradictory. The proper form is
# LIB= + bsd.lib.mk.

SRCS=	pam_auth.c
INC=	pam_auth.h

LIB=	pam_ocifbsd.so
SHLIB_NAME=	pam_ocifbsd.so.1
MAN=	pam_ocifbsd.8

# pam, util, crypto, z are in FreeBSD base; md is libmd for SHA256/HMAC/base64
LIBADD=	 pam util crypto z md

# json-c is a port (not in FreeBSD base). Port path + LDADD.
# Install on VM with:  sudo pkg install json-c
CFLAGS+=	-I/usr/local/include/json-c
LDFLAGS+=	-L/usr/local/lib
LDADD+=		-ljson-c

CFLAGS+=	-I${SRCDIR}/include
CFLAGS+=	-I${SRCDIR}/security-daemon

NO_WERROR=

WARNS?=	3

.include <bsd.lib.mk>
