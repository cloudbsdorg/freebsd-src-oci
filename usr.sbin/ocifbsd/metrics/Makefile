# $FreeBSD$
# Makefile for ocifbsd metrics module
#
# NOTE: metrics.c does NOT currently compile — it references struct members
# that do not exist (node_metrics.hostname, metric.timestamp), missing
# includes (<dirent.h>), undeclared identifiers (OCIFBSD_VAR_DIR,
# metrics_collector_thread) and a kernel-only header (<sys/sockopt.h>). It is
# never built (no PROG/LIB target here) and no binary references it, so the
# module is unbuilt scaffold pending a substantial reconciliation of the code
# with the struct definitions in metrics.h. See .plan/005.0-Risks-TODO.md.
# Do NOT add a LIB=/PROG= target until those errors are fixed, or the parent
# SUBDIR build will fail.

# NOTE: no <src.opts.mk> here. This module references no MK_* knobs, and
# including it would break builds on a minimal FreeBSD without /usr/src (the
# self-contained/cleanroom release target).

SRCS=	metrics.c
MKLLM=	no
MKSTATIC=	no

WARNS?=	2

.include <bsd.prog.mk>
