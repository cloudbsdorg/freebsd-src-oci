# $FreeBSD$
#
# Makefile for ocifbsd image management module
#

SRCS=		zfs_store.c pull.c unpack.c push.c

PROG=		ocifbsd_image
MAN=

# curl is a port (not in FreeBSD base), so use LDADD with the port's
# library path rather than LIBADD (which is validated against
# src.libnames.mk and would fail). Install on VM with:
#   sudo pkg install curl
CFLAGS+=	-I/usr/local/include
LDFLAGS+=	-L/usr/local/lib
LDADD+=		-lcurl

WARNS?=		3

.include <bsd.prog.mk>
