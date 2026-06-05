# Makefile for ocifbsd-gc - Garbage collection daemon
# Phase 15: Garbage Collection

SRCS=	gc.c container.c image.c volume.c network.c cluster.c
INC=	gc.h

.PATH:	${SRCDIR}/gc

PROG=	ocifbsd-gc
MAN=	ocifbsd-gc.8

# m, z, crypto, pthread, util are in FreeBSD base
LIBADD=	 m z crypto pthread util

# json-c is a port (not in FreeBSD base). Port path + LDADD.
# Install on VM with:  sudo pkg install json-c
CFLAGS+=	-I/usr/local/include/json-c
LDFLAGS+=	-L/usr/local/lib
LDADD+=		-ljson-c

CFLAGS+=	-I${SRCDIR}/include

WARNS?=	3

.include <bsd.prog.mk>
