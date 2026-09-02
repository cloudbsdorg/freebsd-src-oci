# Regression tests for the ocifbsd clustering mTLS control channel.
#
# Standalone (not wired into the parent SUBDIR build). Build and run:
#
#	cd ../ && make                 # build cluster_mtls.pieo, cluster_pki.pieo
#	cd tests && make && ./mtls_pin_test
#
PROG=	mtls_pin_test
SRCS=	mtls_pin_test.c
MAN=

# Link against the module objects and the base OpenSSL.
LDADD=	${.CURDIR:H}/cluster_mtls.pieo ${.CURDIR:H}/cluster_pki.pieo -lcrypto -lssl
CFLAGS+=	-I${.CURDIR:H}

MK_INSTALL=	no

.include <bsd.prog.mk>
