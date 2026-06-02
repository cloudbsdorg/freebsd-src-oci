# $FreeBSD$
# Makefile for ocifbsd security daemon module

.include <src.opts.mk>

SRCS=	auth.c
MKLLM=	no
MKSTATIC=	no
LIBS+=	-lpam

WARNS?=	2

.include <bsd.prog.mk>
