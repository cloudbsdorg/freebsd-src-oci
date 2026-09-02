# $FreeBSD$
# Makefile for ocifbsd namespace module

# NOTE: no <src.opts.mk> here. This module references no MK_* knobs, and
# including it would break builds on a minimal FreeBSD without /usr/src (the
# self-contained/cleanroom release target). The installed bsd.lib.mk supplies
# the defaults we need.

LIB=		ocifbsd_namespace
SHLIB_NAME=	ocifbsd_namespace.so.1
SRCS=		namespace.c

WARNS?=		2

# Static archive may be linked into the PIE ocifbsd binary later; compile PIC.
CFLAGS+=	-fPIC

.include <bsd.lib.mk>
