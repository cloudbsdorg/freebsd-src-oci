# $FreeBSD$
#
# Makefile for ocifbsd networking module
#

SRCS=		network.c bridge.c vnet.c cni.c netcfg.c

LIB=		ocifbsd_network
SHLIB_NAME=	ocifbsd_network.so.1
MAN=		ocifbsd_network.3

# json-c is a port (not in FreeBSD base). cni.c uses json_object_*.
# Same pattern as main Makefile: CFLAGS/LDFLAGS add the /usr/local
# paths, LDADD adds . Install on VM with:
#   sudo pkg install json-c
CFLAGS+=	-I/usr/local/include
LDFLAGS+=	-L/usr/local/lib

WARNS?=		3

# json-c is vendored in-tree (contrib/json-c); no port needed.
JSONC_DIR=	${.CURDIR:H}/contrib/json-c
.include "${.CURDIR:H}/jsonc.inc.mk"

.include <bsd.lib.mk>
