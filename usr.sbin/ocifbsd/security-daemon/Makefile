# $FreeBSD$
# Makefile for ocifbsd security-daemon (auth backend)

# NOTE: no <src.opts.mk> here (self-contained/cleanroom release target).

LIB=		ocifbsd_auth
SHLIB_NAME=	ocifbsd_auth.so.1
SRCS=		auth.c

WARNS?=		2

# auth.c uses PAM, OpenSSL (AES via EVP + cert gen), libmd (SHA256).
LIBADD=		pam crypto md crypt

# Static archive may be linked into the PIE ocifbsd binary later; compile PIC.
CFLAGS+=	-fPIC

.include <bsd.lib.mk>
