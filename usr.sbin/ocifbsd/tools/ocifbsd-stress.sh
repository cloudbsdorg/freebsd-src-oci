#!/bin/sh
#-
# SPDX-License-Identifier: BSD-2-Clause
#
# Copyright (c) 2026 REVYTECH, Inc.
#
# ocifbsd-stress.sh — concurrency / leak stress harness (.plan tasks 1.25 / 1.26).
#
# Repeatedly brings up C containers concurrently, holds them running, then
# tears them all down, for R rounds.  After every round it asserts that the
# jail count has returned to the pre-test baseline — the whole point of the
# harness is to surface resource leaks (orphaned jails, epair/bridge remnants,
# undeleted state directories) that a single-shot lifecycle test never exercises.
#
# Self-contained: each jail rootfs is a single static /rescue binary, so no
# image pull or registry is involved.  Root is required for jail(2).
#
# Output is pretty-printed JSON by default; pass --compact for single-line.
#
# Usage:
#   ocifbsd-stress.sh [-c concurrency] [-r rounds] [-b binary] [--compact] [-h]
#
#   -c, --concurrency N  containers up at once per round   (default 10)
#   -r, --rounds R       number of up/down rounds          (default 3)
#   -b, --bin PATH       path to the ocifbsd binary         (default: auto)
#       --compact        emit single-line JSON
#   -h, --help           this help
#
# Exit status: 0 if every round tore down cleanly with no leak; non-zero if a
# container failed to launch or a jail/state leak was detected.

set -u

PROG=$(basename "$0")
CONC=10
ROUNDS=3
BIN=""
PRETTY=1

usage()
{
	sed -n '9,29p' "$0" | sed 's/^# \{0,1\}//'
	exit "${1:-0}"
}

while [ $# -gt 0 ]; do
	case "$1" in
	-c|--concurrency) CONC=$2; shift 2 ;;
	-r|--rounds)      ROUNDS=$2; shift 2 ;;
	-b|--bin)         BIN=$2; shift 2 ;;
	--compact)        PRETTY=0; shift ;;
	-h|--help)        usage 0 ;;
	*) echo "${PROG}: unknown argument: $1" >&2; usage 1 ;;
	esac
done

for v in "${CONC}" "${ROUNDS}"; do
	case "${v}" in ''|*[!0-9]*)
		echo "${PROG}: concurrency and rounds must be positive integers" >&2; exit 2 ;;
	esac
done
[ "${CONC}" -ge 1 ]   || { echo "${PROG}: concurrency must be >= 1" >&2; exit 2; }
[ "${ROUNDS}" -ge 1 ] || { echo "${PROG}: rounds must be >= 1" >&2; exit 2; }

find_bin()
{
	if [ -n "${BIN}" ]; then
		[ -x "${BIN}" ] && { echo "${BIN}"; return 0; }
		echo "${PROG}: ${BIN} is not executable" >&2; return 1
	fi
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
[ -x /rescue/sleep ]  || { echo "${PROG}: /rescue/sleep required for rootfs" >&2; exit 1; }

WORK=$(mktemp -d /tmp/ocifbsd-stress.XXXXXX) || exit 1
CIDS="${WORK}/cids"
trap 'reap_all 2>/dev/null; rm -rf "${WORK}"' EXIT INT TERM

BUNDLE="${WORK}/bundle"
mkdir -p "${BUNDLE}/rootfs/bin"
cp /rescue/sleep "${BUNDLE}/rootfs/bin/sleep"
cat > "${BUNDLE}/config.json" <<'EOF'
{
  "ociVersion": "1.0.2",
  "hostname": "ocifbsd-stress",
  "process": {
    "terminal": false,
    "user": { "uid": 0, "gid": 0 },
    "args": [ "/bin/sleep", "600" ],
    "env": [ "PATH=/bin" ],
    "cwd": "/"
  },
  "root": { "path": "rootfs", "readonly": false }
}
EOF

# Count live ocifbsd jails.  ocifbsd names each jail "ocifbsd-<12-hex>" from the
# container id (the --name we pass is not the jail name), so we match that
# prefix — the same marker lifecycle_test uses.  On a dedicated test host the
# baseline is 0; if other ocifbsd jails exist they sit in both the baseline and
# the post-round count, so the leak delta stays valid regardless.
jail_count() { jls -n name 2>/dev/null | grep -c "name=ocifbsd-" || true; }

reap_all()
{
	[ -f "${CIDS}" ] || return 0
	while read -r cid; do
		[ -n "${cid}" ] || continue
		"${BIN}" kill "${cid}" >/dev/null 2>&1
		"${BIN}" delete --force "${cid}" >/dev/null 2>&1 || \
		    "${BIN}" delete "${cid}" >/dev/null 2>&1
	done < "${CIDS}"
	: > "${CIDS}"
}

BASELINE=$(jail_count)

fail=0
leak_rounds=0
peak=0
launch_fail=0

round=0
while [ "${round}" -lt "${ROUNDS}" ]; do
	round=$((round + 1))
	: > "${CIDS}"

	# --- fan out: create+start C containers concurrently ---
	c=0
	while [ "${c}" -lt "${CONC}" ]; do
		c=$((c + 1))
		(
			name="ocifbsd-stress-$$-${round}-${c}"
			cid=$("${BIN}" create --name "${name}" "${BUNDLE}" 2>/dev/null | tr -d ' \t\r\n')
			if expr "${cid}" : '[0-9a-f]\{64\}$' >/dev/null 2>&1; then
				if "${BIN}" start "${cid}" >/dev/null 2>&1; then
					echo "${cid}" >> "${CIDS}"
				else
					"${BIN}" delete --force "${cid}" >/dev/null 2>&1
					echo "LAUNCHFAIL" >> "${CIDS}.err"
				fi
			else
				echo "LAUNCHFAIL" >> "${CIDS}.err"
			fi
		) &
	done
	wait

	up=$(grep -c . "${CIDS}" 2>/dev/null || echo 0)
	errs=$(grep -c . "${CIDS}.err" 2>/dev/null || echo 0); rm -f "${CIDS}.err"
	launch_fail=$((launch_fail + errs))
	live=$(jail_count)
	[ "${live}" -gt "${peak}" ] && peak=${live}
	echo "${PROG}: round ${round}: launched ${up}/${CONC} (jails now ${live})" >&2

	# --- tear the whole set down ---
	reap_all

	# --- leak check: jail count must be back to baseline ---
	after=$(jail_count)
	if [ "${after}" -ne "${BASELINE}" ]; then
		echo "${PROG}: round ${round}: LEAK — ${after} stress jails remain (baseline ${BASELINE})" >&2
		leak_rounds=$((leak_rounds + 1))
		fail=1
	fi
	[ "${errs}" -eq 0 ] || fail=1
done

emit()
{
	local nl="\n" i1="  "
	if [ "${PRETTY}" = "0" ]; then nl=""; i1=""; fi
	printf '{'
	printf "${nl}${i1}\"harness\": \"ocifbsd-stress\","
	printf "${nl}${i1}\"concurrency\": %s," "${CONC}"
	printf "${nl}${i1}\"rounds\": %s," "${ROUNDS}"
	printf "${nl}${i1}\"baseline_jails\": %s," "${BASELINE}"
	printf "${nl}${i1}\"peak_jails\": %s," "${peak}"
	printf "${nl}${i1}\"launch_failures\": %s," "${launch_fail}"
	printf "${nl}${i1}\"leak_rounds\": %s," "${leak_rounds}"
	printf "${nl}${i1}\"clean\": %s" "$([ "${fail}" = "0" ] && echo true || echo false)"
	printf "${nl}}\n"
}

emit
exit "${fail}"
