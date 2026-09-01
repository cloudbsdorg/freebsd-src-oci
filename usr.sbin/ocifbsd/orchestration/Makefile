# $FreeBSD$
#
# Makefile for ocifbsd orchestration module
#

SRCS=	pod.c stack.c scheduler.c health.c rolling_update.c \
	orch_cli.c orch_init.c stubs.c

LIB=	ocifbsd_orchestration
SHLIB_NAME=	ocifbsd_orchestration.so.1
MAN=	ocifbsd_orchestration.3

# json-c is a port (not in FreeBSD base). The orchestration
# subdir uses json_object_* in pod.c and stack.c. <sys/json.h>
# was removed from FreeBSD 16 base, so we use the port path.
# Install on VM with:
#   sudo pkg install json-c
CFLAGS+=	-I/usr/local/include
LDFLAGS+=	-L/usr/local/lib

WARNS?=	3

# json-c is vendored in-tree (contrib/json-c); no port needed.
JSONC_DIR=	${.CURDIR:H}/contrib/json-c
.include "${.CURDIR:H}/jsonc.inc.mk"

.include <bsd.lib.mk>
