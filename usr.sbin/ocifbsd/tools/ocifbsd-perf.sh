#!/bin/sh
#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 REVYTECH, Inc.
#
# ocifbsd-perf.sh — lifecycle performance harness (.plan tasks 1.23 / 1.24).
#
# Drives the real ocifbsd binary through the OCI lifecycle
#   create -> start -> state -> kill -> delete
# over N iterations, timing each phase with the monotonic clock, and reports
# per-phase latency percentiles (p50 / p95 / max / mean) plus end-to-end
# create->delete cost.
#
# Self-contained: the jail rootfs is a single static /rescue binary, so no
# image pull, registry, or port is required.  Root is needed for jail(2).
#
# Output is pretty-printed JSON by default (a person reads it); pass --compact
# for a single-line object suitable for a pipeline or a CI artifact.
#
# Usage:
#   ocifbsd-perf.sh [-n iterations] [-b binary] [--compact] [-h]
#
#   -n, --iterations N   number of full lifecycles to time   (default 20)
#   -b, --bin PATH       path to the ocifbsd binary          (default: auto)
#       --compact        emit single-line JSON instead of pretty
#   -h, --help           this help
#
# Exit status: 0 on success, non-zero if the binary is missing, not root, or a
# lifecycle phase fails.

set -u

PROG=$(basename "$0")
ITERS=20
BIN=""
PRETTY=1

usage()
{
	sed -n '9,27p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-0}"
}

while [ $# -gt 0 ]; do
	case "$1" in
	-n|--iterations) ITERS=$2; shift 2 ;;
	-b|--bin)        BIN=$2; shift 2 ;;
	--compact)       PRETTY=0; shift ;;
	-h|--help)       usage 0 ;;
	*) echo "${PROG}: unknown argument: $1" >&2; usage 1 ;;
	esac
done

case "${ITERS}" in
''|*[!0-9]*) echo "${PROG}: iterations must be a positive integer" >&2; exit 2 ;;
esac
[ "${ITERS}" -ge 1 ] || { echo "${PROG}: iterations must be >= 1" >&2; exit 2; }

# --- locate the ocifbsd binary ------------------------------------------------
find_bin()
{
	if [ -n "${BIN}" ]; then
		[ -x "${BIN}" ] && { echo "${BIN}"; return 0; }
		echo "${PROG}: ${BIN} is not executable" >&2; return 1
	fi
	# tools/ sits under usr.sbin/ocifbsd; the built binary is one level up.
	local here cand
	here=$(cd "$(dirname "$0")" && pwd)
	for cand in "${here}/../ocifbsd" "${here}/../../ocifbsd"; do
		[ -x "${cand}" ] && { echo "$(cd "$(dirname "${cand}")" && pwd)/ocifbsd"; return 0; }
	done
	command -v ocifbsd 2>/dev/null && return 0
	echo "${PROG}: ocifbsd binary not found (use --bin)" >&2
	return 1
}

BIN=$(find_bin) || exit 1

[ "$(id -u)" = "0" ] || { echo "${PROG}: must run as root (jail(2))" >&2; exit 1; }
[ -x /rescue/sleep ] || { echo "${PROG}: /rescue/sleep required for rootfs" >&2; exit 1; }

WORK=$(mktemp -d /tmp/ocifbsd-perf.XXXXXX) || exit 1
trap 'rm -rf "${WORK}"' EXIT INT TERM

# --- build the minimal bundle once, reused for every iteration ---------------
BUNDLE="${WORK}/bundle"
mkdir -p "${BUNDLE}/rootfs/bin"
cp /rescue/sleep "${BUNDLE}/rootfs/bin/sleep"
cat > "${BUNDLE}/config.json" <<'EOF'
{
  "ociVersion": "1.0.2",
  "hostname": "ocifbsd-perf",
  "process": {
    "terminal": false,
    "user": { "uid": 0, "gid": 0 },
    "args": [ "/bin/sleep", "300" ],
    "env": [ "PATH=/bin" ],
    "cwd": "/"
  },
  "root": { "path": "rootfs", "readonly": false }
}
EOF

# --- monotonic millisecond clock ---------------------------------------------
# date +%s%N is not portable on FreeBSD; use the realtime sysctl (nanoseconds).
now_ms()
{
	# kern.pps or clock_gettime aren't shell-reachable; %N is unsupported in
	# BSD date.  Use the C-free path: read the monotonic clock via sysctl-ish
	# `date +%s` seconds plus a sub-second sample from `date +%N`-less awk.
	# awk's systime() is whole seconds; for sub-ms we shell out to a tiny
	# clock reader if present, else fall back to whole-second timing.
	if [ -n "${_MS_READER}" ]; then
		"${_MS_READER}"
	else
		echo $(( $(date +%s) * 1000 ))
	fi
}

# Prefer a high-resolution reader: FreeBSD ships `date` without %N, but
# /usr/bin/limits and friends can't help.  Use `env` trick via awk reading
# /dev/null and calling gettimeofday — not available.  So compile a one-liner
# reader with cc if we can; otherwise degrade to whole seconds (still useful
# at higher iteration counts).
_MS_READER=""
setup_clock()
{
	local src="${WORK}/clock.c" out="${WORK}/clock"
	cat > "${src}" <<'EOF'
#include <stdio.h>
#include <time.h>
int main(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);
printf("%lld\n",(long long)t.tv_sec*1000+t.tv_nsec/1000000);return 0;}
EOF
	if cc -O2 -o "${out}" "${src}" 2>/dev/null && [ -x "${out}" ]; then
		_MS_READER="${out}"
	fi
}
setup_clock

# --- percentile over a whitespace list of integers ---------------------------
# pct FILE P  -> the P-th percentile (nearest-rank) of the numbers in FILE.
pct()
{
	sort -n "$1" | awk -v p="$2" '
		{ a[NR]=$1 }
		END {
			if (NR==0) { print 0; exit }
			rank=int((p/100.0)*NR + 0.9999); if (rank<1) rank=1; if (rank>NR) rank=NR;
			print a[rank]
		}'
}
stat_mean() { awk '{s+=$1} END{ if(NR) printf "%d", s/NR; else print 0 }' "$1"; }
stat_max()  { sort -n "$1" | tail -1; }

# --- run the timed lifecycles ------------------------------------------------
CREATE_MS="${WORK}/create.ms"; : > "${CREATE_MS}"
START_MS="${WORK}/start.ms";   : > "${START_MS}"
STATE_MS="${WORK}/state.ms";   : > "${STATE_MS}"
KILL_MS="${WORK}/kill.ms";     : > "${KILL_MS}"
DELETE_MS="${WORK}/delete.ms"; : > "${DELETE_MS}"
E2E_MS="${WORK}/e2e.ms";       : > "${E2E_MS}"

fail=0
i=0
echo "${PROG}: timing ${ITERS} lifecycle(s) with ${BIN}" >&2
[ -n "${_MS_READER}" ] || echo "${PROG}: no cc; sub-second resolution unavailable, timing in whole seconds" >&2

while [ "${i}" -lt "${ITERS}" ]; do
	i=$((i + 1))
	name="perf$$_${i}"

	t0=$(now_ms)
	cid=$("${BIN}" create --name "${name}" "${BUNDLE}" 2>/dev/null | tr -d ' \t\r\n')
	t1=$(now_ms)
	if ! expr "${cid}" : '[0-9a-f]\{64\}$' >/dev/null 2>&1; then
		echo "${PROG}: iteration ${i}: create failed" >&2; fail=1; break
	fi
	echo $((t1 - t0)) >> "${CREATE_MS}"

	t0=$(now_ms); "${BIN}" start "${cid}"  >/dev/null 2>&1 || fail=1; t1=$(now_ms)
	echo $((t1 - t0)) >> "${START_MS}"

	t0=$(now_ms); "${BIN}" state "${cid}"  >/dev/null 2>&1 || fail=1; t1=$(now_ms)
	echo $((t1 - t0)) >> "${STATE_MS}"

	t0=$(now_ms); "${BIN}" kill "${cid}"   >/dev/null 2>&1 || fail=1; t1=$(now_ms)
	echo $((t1 - t0)) >> "${KILL_MS}"

	t0=$(now_ms)
	"${BIN}" delete --force "${cid}" >/dev/null 2>&1 || "${BIN}" delete "${cid}" >/dev/null 2>&1 || fail=1
	t1=$(now_ms)
	echo $((t1 - t0)) >> "${DELETE_MS}"

	# end-to-end = sum of this iteration's phases
	c=$(tail -1 "${CREATE_MS}"); s=$(tail -1 "${START_MS}"); st=$(tail -1 "${STATE_MS}")
	k=$(tail -1 "${KILL_MS}");   d=$(tail -1 "${DELETE_MS}")
	echo $((c + s + st + k + d)) >> "${E2E_MS}"

	[ "${fail}" = "0" ] || { echo "${PROG}: iteration ${i}: a phase failed" >&2; break; }
done

# --- emit the JSON report -----------------------------------------------------
phase_json()
{
	# $1 label  $2 file  $3 indent
	local ind="$3"
	printf '%s"%s": { "p50": %s, "p95": %s, "max": %s, "mean": %s }' \
	    "${ind}" "$1" \
	    "$(pct "$2" 50)" "$(pct "$2" 95)" "$(stat_max "$2")" "$(stat_mean "$2")"
}

emit()
{
	local nl="\n" i2="    " i1="  "
	if [ "${PRETTY}" = "0" ]; then nl=""; i2=""; i1=""; fi
	printf '{'
	printf "${nl}${i1}\"harness\": \"ocifbsd-perf\","
	printf "${nl}${i1}\"iterations\": %s," "${ITERS}"
	printf "${nl}${i1}\"unit\": \"ms\","
	printf "${nl}${i1}\"resolution\": \"%s\"," \
	    "$([ -n "${_MS_READER}" ] && echo millisecond || echo second)"
	printf "${nl}${i1}\"phases\": {"
	printf "${nl}"; phase_json create "${CREATE_MS}" "${i2}"; printf ','
	printf "${nl}"; phase_json start  "${START_MS}"  "${i2}"; printf ','
	printf "${nl}"; phase_json state  "${STATE_MS}"  "${i2}"; printf ','
	printf "${nl}"; phase_json kill   "${KILL_MS}"   "${i2}"; printf ','
	printf "${nl}"; phase_json delete "${DELETE_MS}" "${i2}"
	printf "${nl}${i1}},"
	printf "${nl}${i1}\"end_to_end\": { \"p50\": %s, \"p95\": %s, \"max\": %s, \"mean\": %s }" \
	    "$(pct "${E2E_MS}" 50)" "$(pct "${E2E_MS}" 95)" "$(stat_max "${E2E_MS}")" "$(stat_mean "${E2E_MS}")"
	printf "${nl}}\n"
}

emit
exit "${fail}"
