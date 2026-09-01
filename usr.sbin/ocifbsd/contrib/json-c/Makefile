# Vendored json-c (MIT) built as a private static library for ocifbsd, so the
# tree builds without the json-c port. Upstream 0.18; see COPYING.

LIB=	ocifbsd_jsonc
INTERNALLIB=	yes

SRCS=	arraylist.c debug.c json_c_version.c json_object.c \
	json_object_iterator.c json_patch.c json_pointer.c json_tokener.c \
	json_util.c json_visit.c linkhash.c printbuf.c random_seed.c \
	strerror_override.c

# Use our hand-written config.h / json_config.h instead of the cmake ones.
CFLAGS+=	-I${.CURDIR} -DHAVE_CONFIG_H

# ocifbsd binaries are PIE, so the archive's objects must be position
# independent to link into them.
CFLAGS+=	-fPIC

# json-c ships release builds with NDEBUG (its internal assert()s are debug
# aids, not error handling). Match that so the library behaves as the port did.
CFLAGS+=	-DNDEBUG

# Upstream code is not warning-clean at the tree's default level; keep it
# quiet so a third-party import does not gate the build.
WARNS=	0
NO_WERROR=	yes

.include <bsd.lib.mk>
