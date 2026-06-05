# $FreeBSD$
#
# Makefile for ocifbsd networking module
#

SRCS=		network.c bridge.c vnet.c cni.c

PROG=		ocifbsd_network
MAN=

# ossp-uuid is a port (not in FreeBSD base). Provides the
# /usr/local/include/ossp/uuid.h header (we use this path in
# network.c). Install on VM with:
#   sudo pkg install ossp-uuid
CFLAGS+=	-I/usr/local/include
LDFLAGS+=	-L/usr/local/lib
LDADD+=		-lossp-uuid

WARNS?=		3

.include <bsd.prog.mk>
