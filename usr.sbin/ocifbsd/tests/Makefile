# Unit tests for shared helpers and for the networking library.
#
# Standalone (not wired into the parent SUBDIR build). Build and run:
#
#	cd tests && make
#	./json_escape_test
#	sudo ./ipam_race_test		# needs /var/run/ocifbsd/networks
#
# ipam_race_test links the already-built networking library out of the object
# tree, so build the runtime first (cd .. && make).
#
PROGS=	json_escape_test ipam_race_test
MAN=

SRCS.json_escape_test=	json_escape_test.c
# utils.c supplies mkdirp(), which the networking library calls. It is found
# via .PATH; bsd.dep.mk refuses SRCS given as absolute paths under SRCTOP.
.PATH:	${.CURDIR:H}/src
SRCS.ipam_race_test=	ipam_race_test.c utils.c

NETOBJ?=	${.OBJDIR:H}/network

CFLAGS+=	-I${.CURDIR:H}/include -I${.CURDIR:H}/network
LIBADD.ipam_race_test=	md
LDADD.ipam_race_test=	${NETOBJ}/libocifbsd_network.a
DPADD.ipam_race_test=	${NETOBJ}/libocifbsd_network.a

MK_INSTALL=	no

.include <bsd.progs.mk>
