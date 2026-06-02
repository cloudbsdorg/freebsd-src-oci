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
REPO_ROOT != cd ${.CURDIR}/../.. 2>/dev/null && pwd

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

# Run all inspection targets and save report
all-checks:
	@echo "=== Running all inspection targets ==="
	@REPORT=$${REPORT_FILE:-/tmp/ocifbsd-inspection-report.txt}; \
	echo "ocifbsd Inspection Report" > $$REPORT; \
	echo "Generated: $$(date)" >> $$REPORT; \
	echo "Branch:    $$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)" >> $$REPORT; \
	echo "Commit:    $$(git rev-parse --short HEAD 2>/dev/null || echo unknown)" >> $$REPORT; \
	echo "" >> $$REPORT; \
	for t in info smoke audit lint sources size docs vm-status; do \
		echo "" >> $$REPORT; \
		echo "========================================" >> $$REPORT; \
		echo "  Target: $$t" >> $$REPORT; \
		echo "========================================" >> $$REPORT; \
		echo "" >> $$REPORT; \
		$(MAKE) -C ${OCIFBSD_ROOT} $$t >> $$REPORT 2>&1 || echo "  [WARN] $$t failed" >> $$REPORT; \
	done; \
	echo ""; \
	echo "Report saved to: $$REPORT"; \
	echo "Lines: $$(wc -l < $$REPORT | tr -d ' ')"; \
	echo ""; \
	echo "=== Quick summary ==="; \
	grep -E "Count:|Source files:|Total:|Files:" $$REPORT | head -20

# Find the HACK marker and show context
find-hack:
	@echo "=== HACK markers ==="
	@grep -rn "HACK" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null
	@echo ""
	@echo "=== With context (3 lines) ==="
	@grep -rn -B1 -A2 "HACK" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null

# Find files with TODO markers, sorted by count
find-todos:
	@echo "=== TODO markers by file ==="
	@grep -rE "TODO|FIXME|XXX" ${OCIFBSD_ROOT} --include="*.c" --include="*.h" 2>/dev/null | cut -d: -f1 | sort | uniq -c | sort -rn
	@echo ""
	@echo "=== Total count ==="
	@echo "TODO:  $$(grep -rE 'TODO' ${OCIFBSD_ROOT} --include='*.c' --include='*.h' 2>/dev/null | wc -l | tr -d ' ')"
	@echo "FIXME: $$(grep -rE 'FIXME' ${OCIFBSD_ROOT} --include='*.c' --include='*.h' 2>/dev/null | wc -l | tr -d ' ')"
	@echo "XXX:   $$(grep -rE 'XXX' ${OCIFBSD_ROOT} --include='*.c' --include='*.h' 2>/dev/null | wc -l | tr -d ' ')"

# Test cross-build and report blockers (does not attempt full build)
test-cross-build:
	@echo "=== Cross-Build Diagnostic ==="
	@echo ""
	@echo "--- Required tools ---"
	@for tool in bmake clang lld python3; do \
		if command -v $$tool >/dev/null 2>&1; then \
			echo "  [OK]   $$tool: $$(which $$tool)"; \
		else \
			echo "  [FAIL] $$tool: NOT FOUND"; \
		fi; \
	done
	@echo ""
	@echo "--- FreeBSD source tree ---"
	@SRC="$${FREEBSD_SRC:-/usr/src}"; \
	if [ -d "$$SRC" ]; then \
		echo "  [OK]   FREEBSD_SRC=$$SRC"; \
		echo "         $$($${FREEBSD_SRC}/tools/build/make.py --help 2>&1 | head -1)"; \
	else \
		echo "  [WARN] FREEBSD_SRC not set or $$SRC does not exist"; \
		echo "         Cross-build expects: \$$FREEBSD_SRC or /usr/src"; \
	fi
	@echo ""
	@echo "--- Cross-build env (if sourced) ---"
	@echo "  XCC:  $${XCC:-not set}"
	@echo "  XLD:  $${XLD:-not set}"
	@echo "  XAS:  $${XAS:-not set}"
	@if [ -n "$$XCC" ] && [ -n "$$XLD" ]; then \
		echo ""; \
		echo "  [OK] Env is set. Run: bmake -C usr.sbin/ocifbsd darwin-build"; \
	else \
		echo ""; \
		echo "  [WARN] Env not set. Run: bmake -C usr.sbin/ocifbsd darwin-bootstrap"; \
		echo "         Or:  . /tmp/ocifbsd-cross-build-env"; \
	fi
	@echo ""
	@echo "--- Known blockers ---"
	@echo "  1. macOS SDK header leak in lib/libgcc_eh (needs FreeBSD sysroot or VM)"
	@echo "  2. json-c dependency (not in FreeBSD base)"
	@echo "  3. seccomp → capsicum translation (3 files)"
	@echo ""
	@echo "See TROUBLESHOOTING.md for full details."

# Generate a release manifest (what would go in a release tarball)
release-manifest:
	@echo "=== ocifbsd Release Manifest ==="
	@echo "Version: $$(grep '^VERSION=' ${OCIFBSD_ROOT}/Makefile | head -1 | cut -d= -f2)"
	@echo ""
	@echo "--- Source files ---"
	@find ${OCIFBSD_ROOT} -name "*.c" -o -name "*.h" 2>/dev/null | sort
	@echo ""
	@echo "--- Build files ---"
	@find ${OCIFBSD_ROOT} -name "Makefile*" -o -name "*.mk" 2>/dev/null | sort
	@echo ""
	@echo "--- Documentation ---"
	@ls -la ${OCIFBSD_ROOT}/README.md 2>/dev/null
	@ls -la ${OCIFBSD_ROOT}/CHANGELOG.md 2>/dev/null || echo "  No CHANGELOG.md"
	@ls -la ${OCIFBSD_ROOT}/LICENSE 2>/dev/null || echo "  No LICENSE file (BSD-2-Clause in headers)"
	@echo ""
	@echo "--- Scripts ---"
	@find ${OCIFBSD_ROOT} -name "*.sh" 2>/dev/null
	@echo ""
	@echo "--- Total size ---"
	@echo "  Files: $$(find ${OCIFBSD_ROOT} -type f 2>/dev/null | wc -l | tr -d ' ')"
	@echo "  Bytes: $$(du -sb ${OCIFBSD_ROOT} 2>/dev/null | awk '{print $$1}')"
	@echo "  Human: $$(du -sh ${OCIFBSD_ROOT} 2>/dev/null | awk '{print $$1}')"

# Check all required dependencies for development
check-deps:
	@echo "=== Dependency Check ==="
	@MISSING=0; \
	for tool in bmake clang lld python3 git; do \
		if command -v $$tool >/dev/null 2>&1; then \
			VER=$$($$tool --version 2>&1 | head -1 | sed 's/^[^0-9]*//'); \
			printf "  [OK]   %-10s %s\n" "$$tool" "$$VER"; \
		else \
			printf "  [FAIL] %-10s NOT FOUND\n" "$$tool"; \
			MISSING=$$((MISSING + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "  For macOS:"; \
	echo "    brew install bmake llvm lld"; \
	echo ""; \
	echo "  For Linux:"; \
	echo "    sudo apt install bmake clang lld python3"; \
	echo ""; \
	if [ $$MISSING -gt 0 ]; then \
		echo "  $$MISSING tools missing. Install them and re-run."; \
		exit 1; \
	else \
		echo "  All required tools present."; \
	fi

# One-command developer setup: check deps + bootstrap + smoke test
dev: check-deps
	@echo ""
	@echo "=== Developer Setup ==="
	@if [ "$$(uname -s)" = "Darwin" ]; then \
		echo "Detected macOS host. Running darwin-bootstrap..."; \
		${MAKE} -C ${OCIFBSD_ROOT} darwin-bootstrap; \
	else \
		echo "Non-macOS host. Skipping darwin-bootstrap."; \
		echo "For FreeBSD build, run: bmake -C usr.sbin/ocifbsd"; \
	fi
	@echo ""
	@echo "=== Running smoke test ==="
	@${MAKE} -C ${OCIFBSD_ROOT} smoke
	@echo ""
	@echo "=== Developer setup complete ==="
	@echo ""
	@echo "Next steps:"
	@echo "  bmake -C usr.sbin/ocifbsd help       # see all targets"
	@echo "  bmake -C usr.sbin/ocifbsd audit      # see AI slop markers"
	@echo "  bmake -C usr.sbin/ocifbsd info       # see build env"
	@echo ""
	@echo "For FreeBSD build:"
	@echo "  bmake -C usr.sbin/ocifbsd"
	@echo ""
	@echo "For VM testing:"
	@echo "  See .omo/drafts/vm-provisioning.md"

# Verify a clean state: nothing to commit, all checks pass
verify: check-deps
	@echo ""
	@echo "=== Verification ==="
	@echo ""
	@echo "--- Git status ---"
	@git status --short
	@echo ""
	@echo "--- Branch ---"
	@git rev-parse --abbrev-ref HEAD
	@echo ""
	@echo "--- Smoke test ---"
	@${MAKE} -C ${OCIFBSD_ROOT} smoke 2>&1 | tail -10
	@echo ""
	@echo "--- Audit summary ---"
	@echo "TODO:  $$(grep -rE 'TODO' ${OCIFBSD_ROOT} --include='*.c' --include='*.h' 2>/dev/null | wc -l | tr -d ' ')"
	@echo "FIXME: $$(grep -rE 'FIXME' ${OCIFBSD_ROOT} --include='*.c' --include='*.h' 2>/dev/null | wc -l | tr -d ' ')"
	@echo "HACK:  $$(grep -rE 'HACK' ${OCIFBSD_ROOT} --include='*.c' --include='*.h' 2>/dev/null | wc -l | tr -d ' ')"

# Show diff stats against main (or specified branch)
diff-main:
	@echo "=== Diff vs main ==="
	@BRANCH=$${DIFF_BRANCH:-main}; \
	echo "Comparing feature/oci-bootstrap to $$BRANCH"; \
	echo ""; \
	echo "--- Commits ahead ---"; \
	git rev-list --count $$BRANCH..HEAD 2>/dev/null | xargs echo "  Count:"; \
	echo ""; \
	echo "--- Files changed ---"; \
	git diff --stat $$BRANCH..HEAD 2>/dev/null | tail -1; \
	echo ""; \
	echo "--- Lines added/removed ---"; \
	git diff --shortstat $$BRANCH..HEAD 2>/dev/null; \
	echo ""; \
	echo "--- New files in usr.sbin/ocifbsd ---"; \
	git diff --name-status $$BRANCH..HEAD 2>/dev/null | grep "^A.*usr.sbin/ocifbsd" | head -20; \
	echo ""; \
	echo "--- New files in .plan ---"; \
	git diff --name-status $$BRANCH..HEAD 2>/dev/null | grep "^A.*\.plan/" | head -20; \
	echo ""; \
	echo "--- New files in .omo ---"; \
	git diff --name-status $$BRANCH..HEAD 2>/dev/null | grep "^A.*\.omo/" | head -20; \
	echo ""; \
	echo "--- New files in .github ---"; \
	git diff --name-status $$BRANCH..HEAD 2>/dev/null | grep "^A.*\.github/" | head -20; \
	echo ""; \
	echo "--- Modified files ---"; \
	git diff --name-status $$BRANCH..HEAD 2>/dev/null | grep "^M" | head -20

# Show file additions/modifications against main
diff-files:
	@echo "=== Files changed vs main ==="
	@BRANCH=$${DIFF_BRANCH:-main}; \
	git diff --name-status $$BRANCH..HEAD 2>/dev/null | awk '{print $$1, $$2}' | sort | uniq -c | sort -rn

# Show commit log against main
diff-log:
	@echo "=== Commits ahead of main ==="
	@BRANCH=$${DIFF_BRANCH:-main}; \
	git log --oneline $$BRANCH..HEAD 2>/dev/null

# Run all meta targets (all-checks + diff-main + diff-files + diff-log)
all-meta: all-checks diff-main diff-files diff-log
	@echo ""
	@echo "=== All meta targets complete ==="

# Prepare for opening a PR: verify state, run all checks, show summary
prepare-pr: check-deps verify diff-main diff-files diff-log
	@echo ""
	@echo "=== PR Preparation Complete ==="
	@echo ""
	@echo "Pre-PR checklist:"
	@echo "  [ ] All inspection targets passed (see all-checks report)"
	@echo "  [ ] Diff vs main looks correct (see diff-main output)"
	@echo "  [ ] No new TODO/FIXME/HACK markers added (see audit)"
	@echo "  [ ] No new seccomp/json-c references (see lint)"
	@echo "  [ ] All files have license headers (see smoke)"
	@echo "  [ ] Branch is up to date with main: git pull --rebase origin main"
	@echo "  [ ] Commit messages are clear and descriptive"
	@echo ""
	@echo "Next steps:"
	@echo "  git push origin feature/oci-bootstrap"
	@echo "  gh pr create --base main --title '...' --body '...'"
	@echo ""
	@echo "Or use: make push-pr TITLE='...' BODY='...'"

# Push to origin and open a PR (requires gh CLI)
push-pr:
	@echo "=== Pushing to origin and opening PR ==="
	@TITLE="$${TITLE:-ocifbsd: updates}"; \
	BODY="$${BODY:-See commits for details.}"; \
	if ! command -v gh >/dev/null 2>&1; then \
		echo "gh CLI not found. Install with: brew install gh"; \
		exit 1; \
	fi; \
	echo "Title: $$TITLE"; \
	echo "Body:  $$BODY"; \
	echo ""; \
	git push -u origin feature/oci-bootstrap && \
	gh pr create --base main --title "$$TITLE" --body "$$BODY" && \
	echo "" && \
	echo "PR opened. View at: $$(gh pr view --web --json url -q .url 2>/dev/null || echo 'use: gh pr view')"

# Self-review: check for common mistakes before committing
review:
	@echo "=== Self-Review ==="
	@echo ""
	@echo "--- 1. No debug code ---"
	@if grep -rn "printf.*DEBUG\|fprintf.*stderr.*DEBUG" ${OCIFBSD_ROOT} --include="*.c" 2>/dev/null; then \
		echo "  [WARN] Debug printfs found"; \
	else \
		echo "  [OK] No debug printfs"; \
	fi
	@echo ""
	@echo "--- 2. No commented-out code ---"
	@COMMENTED=$$(grep -rE "^\s*//" ${OCIFBSD_ROOT} --include="*.c" 2>/dev/null | wc -l | tr -d ' '); \
	echo "  $$COMMENTED single-line comments (review for commented-out code)"
	@echo ""
	@echo "--- 3. No hardcoded paths ---"
	@if grep -rn "/usr/local\|/home/" ${OCIFBSD_ROOT} --include="*.c" 2>/dev/null; then \
		echo "  [WARN] Hardcoded paths found"; \
	else \
		echo "  [OK] No hardcoded /usr/local or /home/ paths"; \
	fi
	@echo ""
	@echo "--- 4. No magic numbers (basic check) ---"
	@echo "  (manual review recommended)"
	@echo ""
	@echo "--- 5. Function length ---"
	@echo "  Longest functions (top 5):"
	@for f in $$(find ${OCIFBSD_ROOT} -name "*.c" 2>/dev/null); do \
		awk '/^[a-zA-Z_].*\(.*\).*\{/ {start=NR; name=$$0} /^\}/ {if (start) print NR-start+1, name; start=0}' "$$f"; \
	done | sort -rn | head -5
	@echo ""
	@echo "--- 6. Consistent error handling ---"
	@echo "  Functions returning -1 on error (top 5 files):"
	@grep -rc 'return -1' ${OCIFBSD_ROOT} --include="*.c" 2>/dev/null | grep -v ':0$$' | sort -t: -k2 -rn | head -5
	@echo ""
	@echo "--- 7. Memory safety ---"
	@echo "  malloc/free pairs:"
	@MALLOC=`grep -rE 'malloc[(]' ${OCIFBSD_ROOT} --include="*.c" 2>/dev/null | wc -l | tr -d ' '`; \
	FREE=`grep -rE 'free[(]' ${OCIFBSD_ROOT} --include="*.c" 2>/dev/null | wc -l | tr -d ' '`; \
	echo "  malloc: $$MALLOC, free: $$FREE"
	@echo ""
	@echo "--- 8. Dead code patterns (AI slop) ---"
	@DEAD=$$(grep -rE 'strlcpy\(\(char\[\]\)\{0\}' ${OCIFBSD_ROOT} --include="*.c" 2>/dev/null | wc -l | tr -d ' '); \
	echo "  Compound-literal HACKs: $$DEAD (should be 0)"
	@echo ""
	@echo "Self-review complete. Run 'bmake audit' for AI slop markers."

# Print version information
version:
	@echo "=== ocifbsd Version Info ==="
	@echo "Source version: $$(grep '^VERSION=' ${OCIFBSD_ROOT}/Makefile | head -1 | cut -d= -f2 | tr -d ' \t')"
	@echo "Git branch:     $$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')"
	@echo "Git commit:     $$(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')"
	@echo "Git describe:   $$(git describe --always --dirty 2>/dev/null || echo 'unknown')"
	@echo "Build date:     $$(date +%Y-%m-%d)"
	@echo "Source files:   $$(find ${OCIFBSD_ROOT} -name '*.c' -o -name '*.h' 2>/dev/null | wc -l | tr -d ' ')"
	@echo "Source lines:   $$(find ${OCIFBSD_ROOT} -name '*.c' -o -name '*.h' 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}')"

# Print one-line summary of codebase state
summary:
	@TODO=$$(grep -rE 'TODO' ${OCIFBSD_ROOT} --include='*.c' --include='*.h' 2>/dev/null | wc -l | tr -d ' '); \
	FIXME=$$(grep -rE 'FIXME' ${OCIFBSD_ROOT} --include='*.c' --include='*.h' 2>/dev/null | wc -l | tr -d ' '); \
	HACK=$$(grep -rE 'HACK' ${OCIFBSD_ROOT} --include='*.c' --include='*.h' 2>/dev/null | wc -l | tr -d ' '); \
	FILES=$$(find ${OCIFBSD_ROOT} -name '*.c' -o -name '*.h' 2>/dev/null | wc -l | tr -d ' '); \
	LINES=$$(find ${OCIFBSD_ROOT} -name '*.c' -o -name '*.h' 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}'); \
	echo "ocifbsd $$(grep '^VERSION=' ${OCIFBSD_ROOT}/Makefile | head -1 | cut -d= -f2 | tr -d ' \t') | $$(git rev-parse --abbrev-ref HEAD 2>/dev/null)@$$(git rev-parse --short HEAD 2>/dev/null) | $$FILES files, $$LINES lines | TODO=$$TODO FIXME=$$FIXME HACK=$$HACK"

# Check that all documentation files are valid markdown
doc-check:
	@echo "=== Documentation Check ==="
	@echo ""
	@echo "Repo root: ${REPO_ROOT}"; \
	echo ""; \
	echo "--- .omo/drafts/*.md ---"; \
	for f in ${REPO_ROOT}/.omo/drafts/*.md; do \
		if [ -f "$$f" ]; then \
			LINES=$$(wc -l < "$$f" | tr -d ' '); \
			HEADERS=$$(grep -c '^#' "$$f" 2>/dev/null); \
			CODEBLOCKS=$$(grep -c '^```' "$$f" 2>/dev/null); \
			REL=$$(echo "$$f" | sed "s|${REPO_ROOT}/||"); \
			printf "  %-50s %5s lines  %3s headers  %3s code blocks\n" "$$REL" "$$LINES" "$$HEADERS" $$((CODEBLOCKS / 2)); \
		fi; \
	done; \
	echo ""; \
	echo "--- .plan/*.md ---"; \
	for f in ${REPO_ROOT}/.plan/*.md; do \
		if [ -f "$$f" ]; then \
			LINES=$$(wc -l < "$$f" | tr -d ' '); \
			HEADERS=$$(grep -c '^#' "$$f" 2>/dev/null); \
			REL=$$(echo "$$f" | sed "s|${REPO_ROOT}/||"); \
			printf "  %-50s %5s lines  %3s headers\n" "$$REL" "$$LINES" "$$HEADERS"; \
		fi; \
	done; \
	echo ""; \
	echo "--- Cross-references between docs ---"; \
	for doc in TROUBLESHOOTING CONTRIBUTING FAQ SECURITY MIGRATION CHANGELOG; do \
		REF_FILE=${REPO_ROOT}/.omo/drafts/$$doc.md; \
		if [ -f "$$REF_FILE" ]; then \
			REFS=$$(grep -oE '`?\.omo/drafts/[a-zA-Z-]+\.md`?' "$$REF_FILE" 2>/dev/null | sort -u | wc -l | tr -d ' '); \
			printf "  %-50s %s cross-references\n" ".omo/drafts/$$doc.md" "$$REFS"; \
		fi; \
	done

# Create a release archive (tarball)
archive:
	@echo "=== Creating release archive ==="
	@VER=`grep '^VERSION=' ${OCIFBSD_ROOT}/Makefile | head -1 | cut -d= -f2 | tr -d ' \t'`; \
	DATE=`date +%Y%m%d`; \
	ARCHIVE="ocifbsd-$$VER-$$DATE.tar.gz"; \
	SRC_DIR=`dirname ${OCIFBSD_ROOT}`; \
	SRC_BASE=`basename ${OCIFBSD_ROOT}`; \
	echo "Archive: $$ARCHIVE"; \
	echo "Source:  $$SRC_DIR/$$SRC_BASE"; \
	tar czf /tmp/$$ARCHIVE -C "$$SRC_DIR" "$$SRC_BASE" 2>/dev/null && { \
		echo "Created: /tmp/$$ARCHIVE"; \
		ls -lh /tmp/$$ARCHIVE; \
		echo ""; \
		echo "Contents (top 20):"; \
		tar tzf /tmp/$$ARCHIVE | head -20; \
		echo ""; \
		echo "Total files: `tar tzf /tmp/$$ARCHIVE | wc -l | tr -d ' '`"; \
	} || { echo "tar failed"; exit 1; }

# Show a banner with project info
banner:
	@echo ""
	@echo "  ┌─────────────────────────────────────────────────────────────┐"
	@echo "  │                                                             │"
	@echo "  │   ocifbsd - FreeBSD Native OCI Runtime                      │"
	@echo "  │   Version $$(printf '%-5s' $$(grep '^VERSION=' ${OCIFBSD_ROOT}/Makefile | head -1 | cut -d= -f2 | tr -d ' \t'))                                            │"
	@echo "  │   Branch $$(printf '%-30s' $$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown))                  │"
	@echo "  │   Commit $$(printf '%-30s' $$(git rev-parse --short HEAD 2>/dev/null || echo unknown))                  │"
	@echo "  │                                                             │"
	@echo "  │   $$(printf '%-59s' "$$(find ${OCIFBSD_ROOT} -name '*.c' -o -name '*.h' 2>/dev/null | wc -l | tr -d ' ') .c/.h files") │"
	@echo "  │   $$(printf '%-59s' "$$(find ${OCIFBSD_ROOT} -name '*.c' -o -name '*.h' 2>/dev/null | xargs wc -l 2>/dev/null | tail -1 | awk '{print $$1}') lines") │"
	@echo "  │                                                             │"
	@echo "  │   Quick start:                                              │"
	@echo "  │     bmake -C usr.sbin/ocifbsd help                          │"
	@echo "  │     bmake -C usr.sbin/ocifbsd dev                           │"
	@echo "  │     bmake -C usr.sbin/ocifbsd all-meta                      │"
	@echo "  │                                                             │"
	@echo "  └─────────────────────────────────────────────────────────────┘"
	@echo ""

.if exists(src.opts.mk)
.include <src.opts.mk>
.endif

.if exists(bsd.prog.mk)
.include <bsd.prog.mk>
.endif
