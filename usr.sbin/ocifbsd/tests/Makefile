# Unit tests for shared helpers in include/ocifbsd.h.
#
# Standalone (not wired into the parent SUBDIR build). Build and run:
#
#	cd tests && make && ./json_escape_test
#
PROG=	json_escape_test
SRCS=	json_escape_test.c
MAN=

CFLAGS+=	-I${.CURDIR:H}/include
MK_INSTALL=	no

.include <bsd.prog.mk>
