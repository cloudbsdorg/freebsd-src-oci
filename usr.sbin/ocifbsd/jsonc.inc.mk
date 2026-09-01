# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# Shared wiring for the vendored json-c static library (contrib/json-c), so the
# ocifbsd tree builds without the json-c port. Before including this file, set
# JSONC_DIR to the absolute path of the contrib/json-c directory, e.g.:
#
#     JSONC_DIR=	${SRCDIR}/contrib/json-c     # in a subdir module
#     .include "${SRCDIR}/jsonc.inc.mk"
#
# It adds the include path, builds the archive on demand, and links it.

_JSONC_OBJDIR!=	cd ${JSONC_DIR} && ${MAKE} -V .OBJDIR
JSONC_LIB=	${_JSONC_OBJDIR}/libocifbsd_jsonc.a

${JSONC_LIB}: .PHONY
	@cd ${JSONC_DIR} && ${MAKE} all

# Support both include styles: <json.h> (via -I on the json-c dir) and
# <json-c/json.h> (via -I on its parent, contrib/). Prepend so the vendored
# copy is found before any -I/usr/local/include a module adds for libcurl.
CFLAGS:=	-I${JSONC_DIR} -I${JSONC_DIR:H} ${CFLAGS}
DPADD+=		${JSONC_LIB}
LDADD+=		${JSONC_LIB}

beforebuild: ${JSONC_LIB}
