# Makefile for Cloud Export module
# Phase 17: Cloud Export

SRCS=	export.c aws.c gcp.c azure.c network.c jail.c
INC=	export.h

.PATH:	${SRCDIR}/export

PROG=	ocifbsd-export
MAN=	ocifbsd-export.8

# m, z, crypto, pthread, util, md are in FreeBSD base (md for SHA256_*)
LIBADD=	 m z crypto pthread util md

# json-c is a port (not in FreeBSD base). Port path + LDADD.
# Install on VM with:  sudo pkg install json-c
CFLAGS+=	-I/usr/local/include/json-c
LDFLAGS+=	-L/usr/local/lib
LDADD+=		-ljson-c

CFLAGS+=	-I${SRCDIR}/include

WARNS?=	3

.include <bsd.prog.mk>
