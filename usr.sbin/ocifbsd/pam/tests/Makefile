# Regression tests for the ocifbsd PAM module.
#
# Standalone (not wired into the parent SUBDIR build, so a minimal release
# without /usr/src still builds the module). Build and run from this dir:
#
#	cd ../ && make            # build pam_auth.o
#	cd tests && make && ./token_bind_test
#
PROG=	token_bind_test
SRCS=	token_bind_test.c
MAN=

# Link against the module object and its crypto/pam dependencies.
LDADD=	${.CURDIR:H}/pam_auth.o -lpam -lcrypto -lssl -lpthread
CFLAGS+=	-I${.CURDIR:H}

# No install; test binary only.
MK_INSTALL=	no

.include <bsd.prog.mk>
