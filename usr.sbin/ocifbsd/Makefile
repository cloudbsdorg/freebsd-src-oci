# $Id$
# $FreeBSD$
# SPDX-License-Identifier: BSD-2-Clause

PROG=	ocifbsd
VERSION=	0.1.0
MAN=	ocifbsd.8

SRCS=	\
	ocifbsd.c \
	src/container.c \
	src/oci2jail.c \
	src/state.c \
	src/hooks.c \
	src/utils.c

# Module subdirectories
SUBDIR+=	api
SUBDIR+=	cert
SUBDIR+=	clustering
SUBDIR+=	convert
SUBDIR+=	export
SUBDIR+=	gc
SUBDIR+=	image
SUBDIR+=	logd
SUBDIR+=	metrics
SUBDIR+=	namespace
SUBDIR+=	network
SUBDIR+=	orchestration
SUBDIR+=	pam
SUBDIR+=	security
SUBDIR+=	security-daemon
SUBDIR+=	tpm

CFLAGS+=	-I${.CURDIR}/include
CFLAGS+=	-I${.CURDIR}
CFLAGS+=	-Wall -Wextra
CFLAGS+=	-D_XOPEN_SOURCE=700

LIBADD=	\
	jail \
	util \
	zfs \
	m \
	pthread \
	crypto \
	json-c

# Enable test suite
HAS_TESTS=
SUBDIR.${MK_TESTS}+=	tests

# === macOS host convenience targets ===
# These targets help developers on macOS hosts build ocifbsd without
# manually setting up the cross-build environment. See
# ./darwin-bootstrap.sh for the underlying logic.
#
# Usage from repo root:
#   bmake -C usr.sbin/ocifbsd darwin-bootstrap   # install toolchain
#   bmake -C usr.sbin/ocifbsd darwin-build       # bootstrap + build
#   bmake -C usr.sbin/ocifbsd darwin-test        # bootstrap + build + VM test
#
# All targets accept these env vars (with defaults):
#   TARGET=amd64, TARGET_ARCH=amd64
#   VM_HOST=freebsd-oci  (for darwin-test)
#   OCIFBSD_ENV_FILE=/tmp/ocifbsd-cross-build-env

darwin-bootstrap:
	@./darwin-bootstrap.sh --install --yes

darwin-build: darwin-bootstrap
	@. $${OCIFBSD_ENV_FILE:-/tmp/ocifbsd-cross-build-env} && \
	echo "==> Cross-build userland libraries (this may take 5-15 min)..." && \
	python3 $${TOOLS_DIR:-../../tools}/build/make.py \
		TARGET=$${TARGET:-amd64} TARGET_ARCH=$${TARGET_ARCH:-amd64} \
		-j$$(sysctl -n hw.ncpu) libraries && \
	echo "==> Build ocifbsd..." && \
	env TARGET=$${TARGET:-amd64} TARGET_ARCH=$${TARGET_ARCH:-amd64} \
		bmake -C $${OCIFBSD_DIR:-.} && \
	echo "==> Build ocifbsd tests..." && \
	env TARGET=$${TARGET:-amd64} TARGET_ARCH=$${TARGET_ARCH:-amd64} \
		bmake -C $${TESTS_DIR:-../../tests/usr.sbin/ocifbsd} && \
	echo "==> Done. Binaries:" && \
	ls -lh $${OCIFBSD_DIR:-.}/ocifbsd 2>/dev/null || true && \
	ls -lh $${TESTS_DIR:-../../tests/usr.sbin/ocifbsd}/ocifbsd_test 2>/dev/null || true

darwin-test: darwin-build
	@. $${OCIFBSD_ENV_FILE:-/tmp/ocifbsd-cross-build-env} && \
	VM=$${VM_HOST:-freebsd-oci} && \
	echo "==> Deploying to VM $$VM..." && \
	scp ocifbsd $$VM:/root/ && \
	scp ../../tests/usr.sbin/ocifbsd/ocifbsd_test $$VM:/root/ && \
	ssh $$VM "chmod +x /root/ocifbsd /root/ocifbsd_test" && \
	echo "==> Running tests on VM $$VM..." && \
	ssh $$VM "/root/ocifbsd --version" && \
	ssh $$VM "cd /root && kyua test -k tests/usr.sbin/ocifbsd/Kyuafile" && \
	echo "==> Restore VM snapshot..." && \
	../../.omo/drafts/vm-smoke-test.sh ssh-only || \
	echo "Skipping snapshot restore (no VM helper available)"

.include <src.opts.mk>
.include <bsd.prog.mk>
