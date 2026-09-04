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
PROGS=	json_escape_test ipam_race_test yaml_test health_sched_test
MAN=

SRCS.json_escape_test=	json_escape_test.c
# utils.c supplies mkdirp(), which the networking library calls. It is found
# via .PATH; bsd.dep.mk refuses SRCS given as absolute paths under SRCTOP.
.PATH:	${.CURDIR:H}/src
SRCS.ipam_race_test=	ipam_race_test.c utils.c

.PATH:	${.CURDIR:H}/convert
SRCS.yaml_test=	yaml_test.c yaml.c

# Links the built orchestration library; build the runtime first.
ORCHOBJ?=	${.OBJDIR:H}/orchestration
SRCS.health_sched_test=	health_sched_test.c utils.c
LIBADD.health_sched_test=	pthread md util
LDADD.health_sched_test=	${ORCHOBJ}/libocifbsd_orchestration.a
DPADD.health_sched_test=	${ORCHOBJ}/libocifbsd_orchestration.a

NETOBJ?=	${.OBJDIR:H}/network

CFLAGS+=	-I${.CURDIR:H}/include -I${.CURDIR:H}/network -I${.CURDIR:H}/convert \
		-I${.CURDIR:H}/orchestration -I${.CURDIR:H}/contrib
LIBADD.ipam_race_test=	md
LDADD.ipam_race_test=	${NETOBJ}/libocifbsd_network.a
DPADD.ipam_race_test=	${NETOBJ}/libocifbsd_network.a

MK_INSTALL=	no

.include <bsd.progs.mk>
