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

# === Inspection and utility targets ===
# These targets help developers inspect, audit, and maintain the OCI codebase
# without requiring a full build or VM. They run on the host (macOS or Linux)
# and operate on the source tree directly.
OCIFBSD_ROOT := ${.CURDIR}

# Show all available targets with descriptions
help:
	@echo "ocifbsd Makefile targets:"
	@echo ""
	@echo "Build (FreeBSD native):"
	@echo "  (default)         Build ocifbsd + subdirs (BSD make required)"
	@echo "  install           Install to \$${DESTDIR}/usr/sbin"
	@echo "  clean             Remove build artifacts"
	@echo ""
	@echo "macOS host (cross-build):"
	@echo "  darwin-bootstrap  Install toolchain via darwin-bootstrap.sh"
	@echo "  darwin-build      Bootstrap + cross-build userland + ocifbsd + tests"
	@echo "  darwin-test       darwin-build + deploy to VM + run kyua tests"
	@echo ""
	@echo "Inspection:"
	@echo "  help              Show this help (you are here)"
	@echo "  info              Print build environment info"
	@echo "  audit             Run AI slop audit (TODO/FIXME/stub markers)"
	@echo "  lint              Lint source for common issues"
	@echo "  smoke             Run host-side smoke test (syntax + license)"
	@echo "  docs              List all .plan/ documentation files"
	@echo "  sources           List all OCI source files with LOC"
	@echo "  size              Show size of source tree and key files"
	@echo ""
	@echo "VM management:"
	@echo "  vm-snapshot       Take a snapshot of the FreeBSD VM"
	@echo "  vm-restore        Restore VM to last snapshot"
	@echo "  vm-status         Show VM status"
	@echo ""
	@echo "Maintenance:"
	@echo "  clean-all         Remove all build artifacts including obj/"

# Print build environment info (host, target, tools)
info:
	@echo "=== ocifbsd Build Environment ==="
	@echo "Host OS:    $$(uname -s) $$(uname -r) $$(uname -m)"
	@echo "Host make:  $$(which make) ($$(make --version 2>/dev/null | head -1 || echo 'unknown'))"
	@echo "Host bmake: $$(which bmake 2>/dev/null || echo 'NOT FOUND')"
	@if [ -x "/opt/homebrew/opt/llvm/bin/clang" ]; then \
		echo "Homebrew llvm: /opt/homebrew/opt/llvm/bin/clang ($$(/opt/homebrew/opt/llvm/bin/clang --version | head -1))"; \
	elif command -v clang >/dev/null 2>&1; then \
		echo "System clang:  $$(which clang) ($$(clang --version | head -1))"; \
	else \
		echo "clang:         NOT FOUND"; \
	fi
	@if [ -x "/opt/homebrew/bin/lld" ]; then \
		echo "Homebrew lld:  /opt/homebrew/bin/lld (separate package)"; \
	elif [ -x "/opt/homebrew/opt/llvm/bin/lld" ]; then \
		echo "Homebrew lld:  /opt/homebrew/opt/llvm/bin/lld (in llvm prefix)"; \
	else \
		echo "lld:           NOT FOUND (brew install lld)"; \
	fi
	@echo "Python 3:     $$(which python3 2>/dev/null || echo 'NOT FOUND')"
	@echo ""
	@echo "=== Cross-build env (if sourced) ==="
	@echo "XCC:          $${XCC:-not set}"
	@echo "XLD:          $${XLD:-not set}"
	@echo "TARGET:       $${TARGET:-not set}"
	@echo "TARGET_ARCH:  $${TARGET_ARCH:-not set}"
	@echo "MAKEOBJDIR:   $${MAKEOBJDIRPREFIX:-not set}"
	@echo ""
	@echo "=== OCI source tree ==="
	@echo "Root:         ${OCIFBSD_ROOT}"
	@echo "Source files: $$(find ${OCIFBSD_ROOT} -name '*.c' -o -name '*.h' 2>/dev/null | wc -l | tr -d ' ')"
	@echo "Subdirs:      $$(find ${OCIFBSD_ROOT} -mindepth 1 -maxdepth 1 -type d 2>/dev/null | wc -l | tr -d ' ')"

# Run AI slop audit (TODO/FIXME/stub markers in source)
audit:
	@echo "=== AI Slop Audit ==="
	@echo ""
	@echo "--- TODO markers ---"
	@grep -rn "TODO" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null | wc -l | xargs echo "  Count:"
	@grep -rn "TODO" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null | head -10
	@echo ""
	@echo "--- FIXME markers ---"
	@grep -rn "FIXME" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null | wc -l | xargs echo "  Count:"
	@grep -rn "FIXME" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null | head -10
	@echo ""
	@echo "--- XXX markers ---"
	@grep -rn "XXX" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null | wc -l | xargs echo "  Count:"
	@echo ""
	@echo "--- HACK markers ---"
	@grep -rn "HACK" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null | wc -l | xargs echo "  Count:"
	@echo ""
	@echo "--- Hotspot files (most TODO/FIXME) ---"
	@grep -rE "TODO|FIXME" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null | cut -d: -f1 | sort | uniq -c | sort -rn | head -5
	@echo ""
	@echo "Full backlog: ../../.omo/drafts/ai-slop-backlog.md"

# Lint source for common issues
lint:
	@echo "=== Source Lint ==="
	@echo ""
	@echo "--- Files missing SPDX OR copyright header ---"
	@for f in $$(find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null); do \
		if ! head -10 "$$f" | grep -qiE "SPDX-License-Identifier|copyright"; then \
			echo "  $$f"; \
		fi; \
	done | head -10
	@echo ""
	@echo "--- Files with SPDX identifier (modern) ---"
	@SPDX_CNT=$$(find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null | xargs grep -l "SPDX-License-Identifier" 2>/dev/null | wc -l | tr -d ' '); \
	echo "  $$SPDX_CNT files"
	@echo ""
	@echo "--- Files with traditional copyright (BSD-style) ---"
	@COPY_CNT=$$(find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null | xargs grep -l "Copyright (c)" 2>/dev/null | wc -l | tr -d ' '); \
	echo "  $$COPY_CNT files"
	@echo ""
	@echo "--- Seccomp references (need capsicum translation) ---"
	@grep -rln "seccomp" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null
	@echo ""
	@echo "--- json-c references (not in FreeBSD base) ---"
	@grep -rln "json-c\|json_object\|json_tokener" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null | head -5

# Run host-side smoke test (syntax + license check, no build)
smoke:
	@echo "=== Host-Side Smoke Test ==="
	@echo ""
	@echo "--- Shell script syntax ---"
	@for f in $$(find ${OCIFBSD_ROOT} -name "*.sh" 2>/dev/null); do \
		if sh -n "$$f" 2>/dev/null; then \
			echo "  [OK] $$f"; \
		else \
			echo "  [FAIL] $$f"; \
		fi; \
	done
	@echo ""
	@echo "--- Makefile syntax ---"
	@if bmake -n -C ${OCIFBSD_ROOT} >/dev/null 2>&1; then \
		echo "  [OK] usr.sbin/ocifbsd/Makefile"; \
	else \
		echo "  [WARN] usr.sbin/ocifbsd/Makefile (src.opts.mk not found, expected on macOS host)"; \
	fi
	@echo ""
	@echo "--- License header coverage (SPDX OR traditional copyright) ---"
	@TOTAL=$$(find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null | wc -l | tr -d ' '); \
	WITH_LICENSE=$$(find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null | xargs grep -lE "SPDX-License-Identifier|Copyright \\(c\\)" 2>/dev/null | wc -l | tr -d ' '); \
	WITH_SPDX=$$(find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null | xargs grep -l "SPDX-License-Identifier" 2>/dev/null | wc -l | tr -d ' '); \
	echo "  Total:      $$TOTAL files"; \
	echo "  With SPDX:  $$WITH_SPDX files (modern)"; \
	echo "  With copy:  $$WITH_LICENSE files (SPDX OR BSD-style)"

# List all .plan/ documentation files
docs:
	@echo "=== ocifbsd Documentation (.plan/) ==="
	@for f in $$(find ../../.plan -name "*.md" 2>/dev/null | sort); do \
		LINES=$$(wc -l < "$$f" | tr -d ' '); \
		TITLE=$$(head -3 "$$f" | grep "^# " | head -1 | sed 's/^# //'); \
		printf "  %-50s %5s lines  %s\n" "$$f" "$$LINES" "$$TITLE"; \
	done
	@echo ""
	@echo "Total: $$(find ../../.plan -name "*.md" 2>/dev/null | wc -l | tr -d ' ') files, $$(find ../../.plan -name "*.md" 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}') lines"

# List all OCI source files with LOC
sources:
	@echo "=== OCI Source Files ==="
	@for f in $$(find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null | sort); do \
		LOC=$$(wc -l < "$$f" | tr -d ' '); \
		printf "  %5s  %s\n" "$$LOC" "$$f"; \
	done | sort -rn
	@echo ""
	@echo "Total: $$(find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null | wc -l | tr -d ' ') files, $$(find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}') lines"

# Show size of source tree and key files
size:
	@echo "=== Source Tree Size ==="
	@echo "Total .c/.h files: $$(find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null | wc -l | tr -d ' ')"
	@echo "Total lines:       $$(find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}')"
	@echo "Total bytes:       $$(du -sh ${OCIFBSD_ROOT} 2>/dev/null | awk '{print $$1}')"
	@echo ""
	@echo "--- Subdirectory breakdown ---"
	@for d in $$(find ${OCIFBSD_ROOT} -mindepth 1 -maxdepth 1 -type d 2>/dev/null | sort); do \
		FILES=$$(find "$$d" -name "*.c" -o -name "*.h" 2>/dev/null | wc -l | tr -d ' '); \
		LOC=$$(find "$$d" -name "*.c" -o -name "*.h" 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}'); \
		printf "  %-40s %3s files  %5s lines\n" "$$d" "$$FILES" "$$LOC"; \
	done

# === VM management targets ===
# These require a FreeBSD VM with SSH access. Set VM_HOST to override.
VM_HELPER ?= ../../.omo/drafts/vm-smoke-test.sh

vm-snapshot:
	@echo "=== Taking VM snapshot ==="
	@if [ -x "${VM_HELPER}" ]; then \
		${VM_HELPER} snapshot || echo "VM helper not configured for snapshots"; \
	else \
		echo "VM helper not found: ${VM_HELPER}"; \
		echo "See .omo/drafts/vm-provisioning.md for VM setup"; \
	fi

vm-restore:
	@echo "=== Restoring VM snapshot ==="
	@if [ -x "${VM_HELPER}" ]; then \
		${VM_HELPER} restore || echo "VM helper not configured for restore"; \
	else \
		echo "VM helper not found: ${VM_HELPER}"; \
	fi

vm-status:
	@echo "=== VM Status ==="
	@VM=$${VM_HOST:-freebsd-oci}; \
	if command -v ssh >/dev/null 2>&1; then \
		if ssh -o ConnectTimeout=3 -o BatchMode=yes $$VM "uname -a" 2>/dev/null; then \
			echo ""; \
			echo "VM $$VM is reachable"; \
		else \
			echo "VM $$VM is NOT reachable"; \
			echo "See .omo/drafts/vm-provisioning.md for setup"; \
		fi; \
	else \
		echo "ssh not available"; \
	fi

# Remove all build artifacts including obj/
clean-all:
	@echo "=== Deep clean ==="
	@rm -rf ocifbsd ocifbsd_test *.o *.a
	@find ${OCIFBSD_ROOT} -name "*.o" -delete 2>/dev/null || true
	@find ${OCIFBSD_ROOT} -name "*.a" -delete 2>/dev/null || true
	@find ${OCIFBSD_ROOT} -name "obj" -type d -exec rm -rf {} + 2>/dev/null || true
	@rm -rf /tmp/obj-oci /tmp/obj-oci-test /tmp/obj-oci-test2 2>/dev/null || true
	@echo "Cleaned: *.o, *.a, ocifbsd, obj/, /tmp/obj-oci*"

.if exists(src.opts.mk)
.include <src.opts.mk>
.endif

.if exists(bsd.prog.mk)
.include <bsd.prog.mk>
.endif
