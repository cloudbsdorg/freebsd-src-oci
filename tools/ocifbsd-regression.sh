#!/bin/sh
#
# SPDX-License-Identifier: BSD-2-Clause
#
# ocifbsd lab regression driver for FreeBSD.
# Produces agent-readable artifacts under artifacts/regression/<gitsha>/.
#
# Usage (on FreeBSD lab host):
#   sh tools/ocifbsd-regression.sh
#   REPO=$HOME/git/freebsd-src-oci sh tools/ocifbsd-regression.sh
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
STAMP=$(date -u +%Y-%m-%dT%H:%M:%SZ)
OUT="${REPO}/artifacts/regression/${SHA}"
NCPU=${NCPU:-$(sysctl -n hw.ncpu 2>/dev/null || echo 4)}
OBJ=${MAKEOBJDIRPREFIX:-${HOME}/obj/freebsd-src-oci}

mkdir -p "${OUT}" "${OBJ}"

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
	# With MAKEOBJDIRPREFIX, Kyuafile + test binaries live in .OBJDIR
	TESTOBJ=$(make -V .OBJDIR)
	if [ -z "${TESTOBJ}" ] || [ ! -f "${TESTOBJ}/Kyuafile" ]; then
		TESTOBJ=$(pwd)
	fi
	# lifecycle_test resolves ocifbsd relative to src tree
	BINOBJ=$(make -C "${REPO}/usr.sbin/ocifbsd" -V .OBJDIR 2>/dev/null || true)
	if [ -n "${BINOBJ}" ] && [ -x "${BINOBJ}/ocifbsd" ]; then
		ln -sf "${BINOBJ}/ocifbsd" "${REPO}/usr.sbin/ocifbsd/ocifbsd"
	fi
	echo "==== kyua test (dir=${TESTOBJ}) ===="
	cd "${TESTOBJ}"
	ls -la Kyuafile utils_test cli_test parser_test k8s_test oci2jail_test 2>&1 || true
	kyua test -k Kyuafile 2>&1 | tee "${OUT}/kyua-test.txt"
	kyua report --verbose -r LATEST --output="${OUT}/kyua-report.txt" || true
	kyua report-junit -r LATEST --output="${OUT}/kyua-junit.xml" || true

	# Root lifecycle when doas is available (Phase 1 acceptance)
	if command -v doas >/dev/null 2>&1; then
		echo "==== kyua lifecycle as root (doas) ===="
		doas env PATH="${PATH}" \
		    sh -c "cd \"${TESTOBJ}\" && kyua test -k Kyuafile lifecycle_test" \
		    2>&1 | tee "${OUT}/kyua-lifecycle-root.txt" || true
	elif [ "$(id -u)" -eq 0 ]; then
		echo "==== kyua lifecycle as root ===="
		kyua test -k Kyuafile lifecycle_test \
		    2>&1 | tee "${OUT}/kyua-lifecycle-root.txt" || true
	else
		echo "==== skip root lifecycle (no doas / not root) ====" |
		    tee "${OUT}/kyua-lifecycle-root.txt"
	fi
)

# summary for agents
if [ -f "${OUT}/kyua-junit.xml" ] && command -v python3 >/dev/null 2>&1; then
	python3 -c '
import sys
from pathlib import Path
import xml.etree.ElementTree as ET
xml_path, out_path = Path(sys.argv[1]), Path(sys.argv[2])
root = ET.parse(xml_path).getroot()
cases = list(root.iter("testcase"))
fails, skips = [], []
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
    lines.append(f"- FAIL {c.get(\"classname\")}.{c.get(\"name\")}")
for c in skips:
    lines.append(f"- SKIP {c.get(\"classname\")}.{c.get(\"name\")}")
out_path.write_text("\n".join(lines) + "\n")
print(out_path.read_text())
' "${OUT}/kyua-junit.xml" "${OUT}/summary.md"
else
	{
		echo "# Regression ${SHA}"
		echo "host=${HOST}"
		if [ -f "${OUT}/kyua-test.txt" ]; then
			tail -5 "${OUT}/kyua-test.txt"
		fi
	} | tee "${OUT}/summary.md"
fi

if [ -x /usr/local/bin/ccache ]; then
	{
		echo "==== ccache -s ===="
		ccache -s
	} >> "${OUT}/build.log" 2>&1 || true
fi

echo "Artifacts: ${OUT}"
cat "${OUT}/summary.md"
if grep -q 'failed=[1-9]' "${OUT}/summary.md" 2>/dev/null; then
	exit 1
fi
# also fail if kyua text summary shows failed
if grep -E '[1-9][0-9]* failed' "${OUT}/kyua-test.txt" 2>/dev/null; then
	exit 1
fi
exit 0
