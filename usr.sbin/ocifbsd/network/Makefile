# $FreeBSD$
#
# Makefile for ocifbsd networking module
#

SRCS=		network.c bridge.c vnet.c cni.c

PROG=		ocifbsd_network
MAN=

# json-c is a port (not in FreeBSD base). cni.c uses json_object_*.
# Same pattern as main Makefile: CFLAGS/LDFLAGS add the /usr/local
# paths, LDADD adds -ljson-c. Install on VM with:
#   sudo pkg install json-c
CFLAGS+=	-I/usr/local/include
LDFLAGS+=	-L/usr/local/lib
LDADD+=		-ljson-c

WARNS?=		3

.include <bsd.prog.mk>
