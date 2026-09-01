# Makefile for PAM authentication module
# Phase 13: PAM/System Credentials

# pam_auth.c is a PAM module (shared library), not a program — it
# has no main(). The proper form is LIB= (bare name) + bsd.lib.mk;
# SHLIB_NAME controls the actual .so filename installed to /usr/lib.
SRCS=	pam_auth.c
INC=	pam_auth.h

# SRCDIR is the ocifbsd source root (parent of this subdir).
SRCDIR?=	${.CURDIR:H}

LIB=	pam_ocifbsd
SHLIB_NAME=	pam_ocifbsd.so.1
MAN=	pam_ocifbsd.8

# pam, util, crypto, z are in FreeBSD base; md is libmd for SHA256/HMAC/base64
LIBADD=	 pam util crypto z md

# json-c is a port (not in FreeBSD base). Port path + LDADD.
# Install on VM with:  sudo pkg install json-c
LDFLAGS+=	-L/usr/local/lib

CFLAGS+=	-I${SRCDIR}/include
CFLAGS+=	-I${SRCDIR}/security-daemon

NO_WERROR=

WARNS?=	3

# json-c is vendored in-tree (contrib/json-c); no port needed.
JSONC_DIR=	${SRCDIR}/contrib/json-c
.include "${SRCDIR}/jsonc.inc.mk"

.include <bsd.lib.mk>
