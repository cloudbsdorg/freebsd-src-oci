# $FreeBSD$
# Makefile for ocifbsd clustering (gossip + SWIM + Raft) — builds the
# ocifbsd-cluster node daemon.

# NOTE: no <src.opts.mk> here. This module references no MK_* knobs, and
# including it would break builds on a minimal FreeBSD without /usr/src (the
# self-contained/cleanroom release target). The installed bsd.prog.mk / bsd.lib.mk
# supply the defaults we need.

PROG=	ocifbsd-cluster
SRCS=	cluster.c cluster_main.c
MAN=	ocifbsd-cluster.8

# libmd provides SHA256 used by the gossip layer; pthread for the worker
# threads. Both are in FreeBSD base.
LIBADD=	md pthread

MKLLM=	no
MKSTATIC=	no

WARNS?=	2

.include <bsd.prog.mk>
