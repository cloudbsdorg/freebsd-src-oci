#!/bin/sh
# Copyright (c) 2026 REVYTECH, Inc.
# SPDX-License-Identifier: BSD-2-Clause
#
# generate.sh — build the static ocifbsd manual-page website.
#
# Renders every ocifbsd manual page (sections 8 and 3) to HTML with
# mandoc(1), then drops in the curated index.html and man.css that live
# alongside this script. The result is a self-contained directory of static
# files that can be opened directly in a browser or served by any web server.
#
# Usage:
#   sh generate.sh [output-dir]
#
# The output directory defaults to ./ocifbsd-man-site. The manual-page
# sources are discovered under the ocifbsd tree (the parent of docs/), so
# newly added pages are picked up automatically — no list to maintain.

set -eu

here=$(cd "$(dirname "$0")" && pwd)
ocifbsd_root=$(cd "$here/../.." && pwd)     # docs/mansite -> docs -> ocifbsd
out=${1:-"$here/ocifbsd-man-site"}

command -v mandoc >/dev/null 2>&1 || {
	echo "generate.sh: mandoc(1) is required (it ships in the FreeBSD base system)" >&2
	exit 1
}

mkdir -p "$out"

# Curated, hand-authored front page, stylesheet, and hero banner. Stamp the
# landing page with the build time (UTC) so it is clear when it was generated.
stamp=$(date -u '+%Y-%m-%d %H:%M UTC')
sed "s/@@GENERATED@@/$stamp/" "$here/index.html" > "$out/index.html"
cp "$here/man.css"    "$out/man.css"
[ -f "$here/hero.png" ] && cp "$here/hero.png" "$out/hero.png"

# Render every manual page in the tree, skipping the vendored contrib/ copies.
count=0
for mp in $(find "$ocifbsd_root" -path "$ocifbsd_root/contrib" -prune -o \
    \( -name '*.8' -o -name '*.3' \) -print | sort); do
	base=$(basename "$mp")
	mandoc -T html -O style=man.css "$mp" > "$out/$base.html"
	count=$((count + 1))
done

echo "generate.sh: rendered $count manual pages into $out"
echo "generate.sh: open $out/index.html in a browser"
