# Makefile for Certificate Management
# Phase 16: Certificate Management

SRCS=	cert.c rotation.c backup.c recovery.c acme.c external_ca.c
INC=	cert.h

# SRCDIR is the ocifbsd source root (parent of this subdir). Without this
# definition it expanded to the empty string, so shared include paths became
# root-relative (e.g. -I/include), silently missing the real headers.
SRCDIR?=	${.CURDIR:H}

.PATH:	${SRCDIR}/cert

PROG=	ocifbsd-cert
MAN=	ocifbsd-cert.8

# crypto, ssl, z, pthread, util are in FreeBSD base
LIBADD=	 crypto ssl z pthread util

CFLAGS+=	-I${SRCDIR}/include

WARNS?=	3

# json-c and libcurl are vendored in-tree (contrib/); no ports needed.
# libcurl is the ACME (acme.c) HTTP transport.
JSONC_DIR=	${SRCDIR}/contrib/json-c
.include "${SRCDIR}/jsonc.inc.mk"
CURL_DIR=	${SRCDIR}/contrib/curl
.include "${SRCDIR}/curl.inc.mk"

.include <bsd.prog.mk>
