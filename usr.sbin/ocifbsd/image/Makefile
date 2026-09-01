# $FreeBSD$
#
# Makefile for ocifbsd image management module
#

SRCS=		zfs_store.c pull.c unpack.c push.c reference.c paths.c \
		whiteout.c load.c

LIB=		ocifbsd_image
SHLIB_NAME=	ocifbsd_image.so.1
MAN=		ocifbsd_image.3

# archive is a base-system library tracked in src.libnames.mk
LIBADD+=	archive

WARNS?=		3

# Static archive is linked into the PIE ocifbsd binary (LTO).
CFLAGS+=	-fPIC

# json-c and libcurl are vendored in-tree (contrib/); no ports needed.
# libcurl is the registry pull/push (pull.c, push.c) HTTP transport.
JSONC_DIR=	${.CURDIR:H}/contrib/json-c
.include "${.CURDIR:H}/jsonc.inc.mk"
CURL_DIR=	${.CURDIR:H}/contrib/curl
.include "${.CURDIR:H}/curl.inc.mk"

.include <bsd.lib.mk>
