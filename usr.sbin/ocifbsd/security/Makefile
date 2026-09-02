# $FreeBSD$
#
# Makefile for ocifbsd security module
#

SRCS=		rctl.c mac.c

LIB=		ocifbsd_security
SHLIB_NAME=	ocifbsd_security.so.1
MAN=		ocifbsd_security.3

WARNS?=		3

# Static archive is linked into the PIE ocifbsd binary; compile PIC so the
# archived objects relocate correctly in the position-independent executable.
CFLAGS+=	-fPIC

.include <bsd.lib.mk>
