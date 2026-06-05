# $FreeBSD$
#
# Makefile for ocifbsd orchestration module
#

SRCS=	pod.c stack.c scheduler.c health.c rolling_update.c orch_cli.c orch_init.c

PROG=	ocifbsd_orch
MAN=

# json-c is a port (not in FreeBSD base). The orchestration
# subdir uses json_object_* in pod.c and stack.c. <sys/json.h>
# was removed from FreeBSD 16 base, so we use the port path.
# Install on VM with:
#   sudo pkg install json-c
CFLAGS+=	-I/usr/local/include
LDFLAGS+=	-L/usr/local/lib
LDADD+=		-ljson-c

WARNS?=	3

.include <bsd.prog.mk>
