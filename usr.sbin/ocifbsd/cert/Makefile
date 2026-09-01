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

# json-c and libcurl are ports (not in FreeBSD base). Port path + LDADD (not
# LIBADD, which is validated against src.libnames.mk and rejects port libs).
# libcurl is the ACME (acme.c) HTTP transport.
# Install on VM with:  sudo pkg install json-c curl
CFLAGS+=	-I/usr/local/include/json-c -I/usr/local/include
LDFLAGS+=	-L/usr/local/lib
LDADD+=		-ljson-c -lcurl

CFLAGS+=	-I${SRCDIR}/include

WARNS?=	3

.include <bsd.prog.mk>
