#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# CI only. A fresh checkout gives every file the current time, so cargo and Ninja rebuild our
# own sources against a restored build tree even when nothing changed. Back-date the tracked
# files that are unchanged since the commit the cached tree was built from (recorded in
# <build dir>/ci-sha); changed files keep the checkout time and rebuild. Without a usable
# record nothing is touched (full rebuild of our sources, as before).
#   sh tools/ci_mtime.sh build/<host>
set -eu
rec=$1/ci-sha
[ -f "$rec" ] || { echo "no $rec: sources keep checkout times"; exit 0; }
old=$(cat "$rec")
if ! git cat-file -e "$old^{commit}" 2>/dev/null; then
    echo "cached build commit $old not in history: sources keep checkout times"; exit 0
fi
changed=${RUNNER_TEMP:-/tmp}/ci-mtime-changed
git -c core.quotepath=off diff --name-only --no-renames "$old" HEAD > "$changed"
git -c core.quotepath=off ls-files | grep -vxF -f "$changed" | tr '\n' '\0' | xargs -0 -r touch -c -d "2000-01-01 00:00:00"
echo "cached build from $old: $(wc -l < "$changed") changed files keep checkout times"
