# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# Shared wiring for the vendored libcurl static library (contrib/curl), so the
# ocifbsd tree builds without the curl port. Before including this file, set
# CURL_DIR to the absolute path of the contrib/curl directory, e.g.:
#
#     CURL_DIR=	${SRCDIR}/contrib/curl
#     .include "${SRCDIR}/curl.inc.mk"
#
# It adds the public-header include path, builds the archive on demand, links
# it, and pulls in the base libraries libcurl needs (OpenSSL, z, pthread). The
# vendored curl is configured for TLS via FreeBSD base OpenSSL and HTTP/HTTPS
# only, so no port and no third-party shared libraries are required.

_CURL_OBJDIR!=	cd ${CURL_DIR} && ${MAKE} -V .OBJDIR
CURL_LIB=	${_CURL_OBJDIR}/libocifbsd_curl.a

${CURL_LIB}: .PHONY
	@cd ${CURL_DIR} && ${MAKE} all

# Public API is included as <curl/curl.h>; prepend so the vendored headers win
# over any -I/usr/local/include a module might still carry.
CFLAGS:=	-I${CURL_DIR}/include ${CFLAGS}

# The vendored archive is linked as a path (allowed in LDADD). Its base-system
# dependencies — OpenSSL (TLS) and pthread (threaded resolver) — must go through
# LIBADD, which the FreeBSD src build validates against src.libnames.mk; raw
# -lssl/-lcrypto there is rejected. LIBADD expansion lands after LDADD on the
# link line, so the archive still precedes the libraries it needs.
DPADD+=		${CURL_LIB}
LDADD+=		${CURL_LIB}
LIBADD+=	ssl crypto pthread

beforebuild: ${CURL_LIB}
