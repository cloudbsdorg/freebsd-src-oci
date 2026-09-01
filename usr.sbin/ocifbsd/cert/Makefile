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

# json-c is a port (not in FreeBSD base). Port path + LDADD (not LIBADD,
# which is validated against src.libnames.mk and rejects port libs).
# Install on VM with:  sudo pkg install json-c
CFLAGS+=	-I/usr/local/include/json-c
LDFLAGS+=	-L/usr/local/lib
LDADD+=		-ljson-c

CFLAGS+=	-I${SRCDIR}/include

WARNS?=	3

.include <bsd.prog.mk>
