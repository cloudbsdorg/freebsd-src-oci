# $FreeBSD$
#
# Makefile for ocifbsd image management module
#

SRCS=		zfs_store.c pull.c unpack.c push.c reference.c paths.c \
		whiteout.c load.c

LIB=		ocifbsd_image
SHLIB_NAME=	ocifbsd_image.so.1
MAN=		ocifbsd_image.3

# curl and json-c are ports (not in FreeBSD base), so use LDADD with the
# port's library path rather than LIBADD (which is validated against
# src.libnames.mk and would fail). Install on VM with:
#   sudo pkg install curl json-c
CFLAGS+=	-I/usr/local/include
LDFLAGS+=	-L/usr/local/lib
LDADD+=	-lcurl
# archive is a base-system library tracked in src.libnames.mk
LIBADD+=	archive

WARNS?=		3

# Static archive is linked into the PIE ocifbsd binary (LTO).
CFLAGS+=	-fPIC

# json-c is vendored in-tree (contrib/json-c); no port needed.
JSONC_DIR=	${.CURDIR:H}/contrib/json-c
.include "${.CURDIR:H}/jsonc.inc.mk"

.include <bsd.lib.mk>
