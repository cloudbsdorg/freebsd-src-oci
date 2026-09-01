# $FreeBSD$
# Makefile for ocifbsd namespace module

# NOTE: no <src.opts.mk> here. This module references no MK_* knobs, and
# including it would break builds on a minimal FreeBSD without /usr/src (the
# self-contained/cleanroom release target). The installed bsd.prog.mk / bsd.lib.mk
# supply the defaults we need.

SRCS=	namespace.c
MKLLM=	no
MKSTATIC=	no

WARNS?=	2

.include <bsd.prog.mk>
