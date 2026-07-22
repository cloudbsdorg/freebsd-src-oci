#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# ocifbsd stress / soak harness for FreeBSD lab hosts.
# Exercises pure CLI paths and optional live registry pull without
# requiring root for the non-jail portion.
#
# Usage:
#   sh tools/ocifbsd-stress.sh
#   ITER=500 LIVE_PULL=1 sh tools/ocifbsd-stress.sh
#
# Artifacts: artifacts/stress/<gitsha>/
#

set -eu

REPO=${REPO:-}
if [ -z "${REPO}" ]; then
	SCRIPT=$0
	case ${SCRIPT} in
	/*) ;;
	*) SCRIPT=$(pwd)/${SCRIPT} ;;
	esac
	REPO=$(CDPATH= cd -- "$(dirname "${SCRIPT}")/.." && pwd)
fi

cd "${REPO}"
if [ ! -f usr.sbin/ocifbsd/Makefile ]; then
	echo "error: not an ocifbsd tree at ${REPO}" >&2
	exit 1
fi

SHA=$(git rev-parse --short HEAD 2>/dev/null || echo nogit)
HOST=$(hostname)
STAMP=$(date -u +%Y%m%dT%H%M%SZ)
OUT="${REPO}/artifacts/stress/${SHA}"
ITER=${ITER:-200}
LIVE_PULL=${LIVE_PULL:-0}
OBJ=${MAKEOBJDIRPREFIX:-${HOME}/obj/freebsd-src-oci}
export MAKEOBJDIRPREFIX="${OBJ}"

mkdir -p "${OUT}"

BIN=$(make -C usr.sbin/ocifbsd -V .OBJDIR 2>/dev/null)/ocifbsd
if [ ! -x "${BIN}" ]; then
	BIN="${REPO}/usr.sbin/ocifbsd/ocifbsd"
fi
if [ ! -x "${BIN}" ]; then
	echo "error: ocifbsd binary not found; build first" >&2
	exit 1
fi

STORE="${OUT}/store"
rm -rf "${STORE}"
mkdir -p "${STORE}"
export OCIFBSD_DATA_DIR="${STORE}"

fail=0
pass=0
log() { echo "$*" | tee -a "${OUT}/stress.log"; }

ok() { pass=$((pass + 1)); log "PASS: $*"; }
bad() { fail=$((fail + 1)); log "FAIL: $*"; }

log "==== ocifbsd stress sha=${SHA} host=${HOST} iter=${ITER} ===="
log "bin=${BIN}"
log "store=${STORE}"

# --- 1. parse-only dry-run loop ---
i=0
while [ "${i}" -lt "${ITER}" ]; do
	if ! "${BIN}" pull --dry-run "hello-world:latest" >/dev/null 2>&1; then
		bad "dry-run hello-world iter=${i}"
		break
	fi
	if ! "${BIN}" pull --dry-run "ghcr.io/cloudbsd/ocifbsd:dev" >/dev/null 2>&1; then
		bad "dry-run ghcr iter=${i}"
		break
	fi
	if ! "${BIN}" pull --dry-run "localhost:5000/ns/img:v1" >/dev/null 2>&1; then
		bad "dry-run localhost iter=${i}"
		break
	fi
	i=$((i + 1))
done
if [ "${i}" -eq "${ITER}" ]; then
	ok "dry-run loop ${ITER}x3"
fi

# --- 2. invalid reference failure (must stay failing) ---
if "${BIN}" pull --dry-run "" >/dev/null 2>&1; then
	bad "empty ref should fail"
else
	ok "empty ref fails"
fi
if "${BIN}" rmi "ghcr.io/no/such:image" >/dev/null 2>&1; then
	bad "rmi missing should fail"
else
	ok "rmi missing fails"
fi
if "${BIN}" create --image "ghcr.io/no/such:image" >/dev/null 2>&1; then
	bad "create --image missing should fail"
else
	ok "create --image missing fails"
fi

# --- 3. local image layout: create store + rmi churn ---
j=0
while [ "${j}" -lt 50 ]; do
	img="${STORE}/localhost/stress-img/v${j}"
	mkdir -p "${img}/rootfs"
	printf '%s\n' '{"ociVersion":"1.0.2","process":{"args":["/bin/true"]},"root":{"path":"rootfs"}}' \
		> "${img}/config.json"
	if ! "${BIN}" rmi "localhost/stress-img:v${j}" >/dev/null 2>&1; then
		# path uses tag as last component — ref must match
		:
	fi
	j=$((j + 1))
done
# proper layout: registry/repo/tag
k=0
while [ "${k}" -lt 50 ]; do
	img="${STORE}/localhost/stress/t${k}"
	mkdir -p "${img}/rootfs"
	echo '{}' > "${img}/config.json"
	if ! "${BIN}" rmi "localhost/stress:t${k}" >/dev/null 2>&1; then
		bad "rmi localhost/stress:t${k}"
		break
	fi
	if [ -d "${img}" ]; then
		bad "rmi left directory ${img}"
		break
	fi
	k=$((k + 1))
done
if [ "${k}" -eq 50 ]; then
	ok "rmi create/delete churn 50"
fi

# --- 4. images on empty-ish store ---
if ! "${BIN}" images >/dev/null 2>&1; then
	bad "images command"
else
	ok "images command"
fi

# --- 5. optional live pull (network) ---
if [ "${LIVE_PULL}" = "1" ]; then
	log "==== LIVE_PULL hello-world ===="
	if "${BIN}" pull hello-world:latest >"${OUT}/live-pull.log" 2>&1; then
		if [ -f "${STORE}/docker.io/library/hello-world/latest/config.json" ] &&
		   [ -d "${STORE}/docker.io/library/hello-world/latest/rootfs" ]; then
			ok "live pull hello-world"
			"${BIN}" rmi hello-world:latest >>"${OUT}/live-pull.log" 2>&1 || true
		else
			bad "live pull missing store files"
		fi
	else
		bad "live pull hello-world (see live-pull.log)"
	fi
else
	log "==== skip LIVE_PULL (set LIVE_PULL=1 to enable) ===="
fi

# --- summary ---
{
	echo "sha=${SHA}"
	echo "host=${HOST}"
	echo "date=${STAMP}"
	echo "iter=${ITER}"
	echo "pass=${pass}"
	echo "fail=${fail}"
	if [ "${fail}" -eq 0 ]; then
		echo "result=PASS"
	else
		echo "result=FAIL"
	fi
} | tee "${OUT}/RESULT.txt"

log "Artifacts: ${OUT}"
if [ "${fail}" -ne 0 ]; then
	exit 1
fi
exit 0
