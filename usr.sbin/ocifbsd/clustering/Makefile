# $FreeBSD$
# Makefile for ocifbsd clustering (gossip + SWIM + Raft) — builds the
# ocifbsd-cluster node daemon.

.include <src.opts.mk>

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
