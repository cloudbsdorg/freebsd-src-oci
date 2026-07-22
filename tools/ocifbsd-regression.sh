#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# ocifbsd lab regression driver for FreeBSD.
# Produces agent-readable artifacts under artifacts/regression/<gitsha>/.
#
# Usage (on FreeBSD lab host, from anywhere):
#   sh tools/ocifbsd-regression.sh
#   REPO=$HOME/git/freebsd-src-oci sh tools/ocifbsd-regression.sh
#
# Optional env:
#   WITH_CCACHE_BUILD=yes (default yes if ccache present)
#   MAKEOBJDIRPREFIX, CCACHE_DIR, CCACHE_BASEDIR, NCPU
#

set -eu

REPO=${REPO:-}
if [ -z "${REPO}" ]; then
	# Resolve repo root from this script: tools/ -> parent
	SCRIPT=$(readlink -f "$0" 2>/dev/null || realpath "$0" 2>/dev/null || echo "$0")
	REPO=$(CDPATH= cd -- "$(dirname "$SCRIPT")/.." && pwd)
fi

cd "${REPO}"
if [ ! -f usr.sbin/ocifbsd/Makefile ]; then
	echo "error: not an ocifbsd tree at ${REPO}" >&2
	exit 1
fi

SHA=$(git rev-parse --short HEAD 2>/dev/null || echo nogit)
HOST=$(hostname)
STAMP=$(date -u +%Y-%m-%dT%H:%M:%SZ)
OUT="${REPO}/artifacts/regression/${SHA}"
NCPU=${NCPU:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
OBJ=${MAKEOBJDIRPREFIX:-${HOME}/obj/freebsd-src-oci}

mkdir -p "${OUT}" "${OBJ}"

# ccache if available
if [ -x /usr/local/bin/ccache ]; then
	export WITH_CCACHE_BUILD=${WITH_CCACHE_BUILD:-yes}
	export CCACHE_DIR=${CCACHE_DIR:-${HOME}/.ccache/ocifbsd}
	export CCACHE_BASEDIR=${CCACHE_BASEDIR:-${REPO}}
	mkdir -p "${CCACHE_DIR}"
	ccache -z >/dev/null 2>&1 || true
fi
export MAKEOBJDIRPREFIX="${OBJ}"

{
	echo "sha=${SHA}"
	echo "host=${HOST}"
	echo "date=${STAMP}"
	echo "ncpu=${NCPU}"
	echo "WITH_CCACHE_BUILD=${WITH_CCACHE_BUILD:-no}"
	echo "MAKEOBJDIRPREFIX=${MAKEOBJDIRPREFIX}"
	echo "==== build ocifbsd ===="
	make -C usr.sbin/ocifbsd -j"${NCPU}"
	echo "==== build tests ===="
	make -C tests/usr.sbin/ocifbsd -j"${NCPU}"
} 2>&1 | tee "${OUT}/build.log"

(
	cd tests/usr.sbin/ocifbsd
	echo "==== kyua test ===="
	kyua test -k Kyuafile 2>&1 | tee "${OUT}/kyua-test.txt"
	kyua report --verbose -r LATEST --output="${OUT}/kyua-report.txt" || true
	kyua report-junit -r LATEST --output="${OUT}/kyua-junit.xml" || true
)

# summary for agents
if [ -f "${OUT}/kyua-junit.xml" ]; then
	python3 - "${OUT}/kyua-junit.xml" "${OUT}/summary.md" <<'PY' || {
		# fallback without python
		echo "# Regression ${SHA}" > "${OUT}/summary.md"
		echo "host=${HOST}" >> "${OUT}/summary.md"
		grep -c 'testcase ' "${OUT}/kyua-junit.xml" | \
		    awk '{print "testcase_elements="$1}' >> "${OUT}/summary.md" || true
	}
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

xml_path, out_path = Path(sys.argv[1]), Path(sys.argv[2])
root = ET.parse(xml_path).getroot()
cases = list(root.iter("testcase"))
fails = []
skips = []
for c in cases:
    if c.find("failure") is not None or c.find("error") is not None:
        fails.append(c)
    if c.find("skipped") is not None:
        skips.append(c)
lines = [
    f"# Regression {xml_path.parent.name}",
    f"total={len(cases)} failed={len(fails)} skipped={len(skips)}",
]
for c in fails:
    lines.append(f"- FAIL {c.get('classname')}.{c.get('name')}")
for c in skips:
    lines.append(f"- SKIP {c.get('classname')}.{c.get('name')}")
out_path.write_text("\n".join(lines) + "\n")
print(out_path.read_text())
PY
else
	echo "# Regression ${SHA}" > "${OUT}/summary.md"
	echo "kyua-junit.xml missing" >> "${OUT}/summary.md"
fi

if [ -x /usr/local/bin/ccache ]; then
	{
		echo "==== ccache -s ===="
		ccache -s
	} >> "${OUT}/build.log" 2>&1 || true
fi

echo "Artifacts: ${OUT}"
cat "${OUT}/summary.md"
# exit non-zero if summary reports failures
if grep -q 'failed=[1-9]' "${OUT}/summary.md" 2>/dev/null; then
	exit 1
fi
exit 0
