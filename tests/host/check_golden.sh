#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Re-render the 16 factory demos (8 s each) and compare with the golden hashes.
#   check_golden.sh           compare against tests/host/golden/demos.sha256
#   check_golden.sh --record  rewrite the golden file from the current tree
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin=${OUT:-$here/../../build/host}/fm1_host
golden=$here/golden/demos.sha256
"$here/build.sh" >/dev/null
tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
n=0
while [ $n -lt 16 ]; do
    h=$("$bin" render $n 2756 - | shasum -a 256 | cut -d' ' -f1)
    echo "demo$n $h" >>"$tmp"
    n=$((n + 1))
done
if [ "${1:-}" = "--record" ]; then
    cp "$tmp" "$golden"; echo "recorded $golden"; exit 0
fi
if diff -u "$golden" "$tmp"; then echo "golden: all 16 demos match"; else echo "golden: MISMATCH" >&2; exit 1; fi
