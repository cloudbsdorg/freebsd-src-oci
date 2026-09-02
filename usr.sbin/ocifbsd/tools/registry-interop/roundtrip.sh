#!/bin/sh
#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 REVYTECH, Inc.
#
# roundtrip.sh — pull / push interoperability check for ocifbsd against a real
# Docker Registry v2, without needing internet access to Docker Hub.
#
# It drives a full loop on the ocifbsd host:
#   pull  <registry>/<repo>:<tag>       from a seeded registry
#   push  <registry>/<repo>:<tag>       back to the registry
#   wipe the local store
#   pull  <registry>/<repo>:<tag>       again (now served from the pushed blobs)
#   verify the rootfs was reconstructed
#
# The registry itself is the minimal `minireg.py` in this directory, run on any
# host the ocifbsd machine can reach (it needs python3 and, to seed content,
# skopeo). This is dev/CI interop tooling — not part of the runtime or its
# self-contained base build.
#
# Usage:
#   roundtrip.sh <registry-host:port> <repo> [tag]
# e.g.
#   roundtrip.sh 192.168.1.154:5000 library/hello-world latest
#
# The registry host must be listed as insecure (http) in registries.conf:
#   printf '%s %s - - http\n' <host:port> <host:port> \
#       >> /etc/ocifbsd/registries.conf
#
# Requires root on the ocifbsd host (image store under /var/lib/ocifbsd).

set -u
PROG=$(basename "$0")
REG=${1:-}
REPO=${2:-}
TAG=${3:-latest}
BIN=${OCIFBSD:-ocifbsd}

[ -n "${REG}" ] && [ -n "${REPO}" ] || {
	echo "usage: ${PROG} <registry-host:port> <repo> [tag]" >&2
	exit 2
}
command -v "${BIN}" >/dev/null 2>&1 || {
	echo "${PROG}: ocifbsd not found (set OCIFBSD=/path/to/ocifbsd)" >&2
	exit 1
}
[ "$(id -u)" = "0" ] || { echo "${PROG}: must run as root" >&2; exit 1; }

REF="${REG}/${REPO}:${TAG}"
STORE="/var/lib/ocifbsd/${REG}/${REPO}/${TAG}"

echo "== 1. pull ${REF} (seed) =="
"${BIN}" pull "${REF}" >/dev/null || { echo "pull failed"; exit 1; }
[ -x "${STORE}/rootfs" ] || [ -d "${STORE}/rootfs" ] || { echo "no rootfs"; exit 1; }
before=$(find "${STORE}/rootfs" -type f | wc -l | tr -d ' ')
echo "   rootfs files: ${before}"

echo "== 2. push ${REF} =="
"${BIN}" push "${REF}" >/dev/null || { echo "push failed"; exit 1; }

echo "== 3. wipe local store =="
rm -rf "/var/lib/ocifbsd/${REG}"

echo "== 4. pull ${REF} back (from pushed blobs) =="
"${BIN}" pull "${REF}" >/dev/null || { echo "pull-back failed"; exit 1; }
after=$(find "${STORE}/rootfs" -type f | wc -l | tr -d ' ')
echo "   rootfs files: ${after}"

[ "${before}" = "${after}" ] && [ "${after}" -gt 0 ] || {
	echo "FAIL: rootfs file count changed (${before} -> ${after})"; exit 1; }

echo "PASS: push/pull round-trip reconstructed ${after} rootfs file(s)"
