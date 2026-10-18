#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Re-render the 16 factory demos (8 s each) and compare with the golden hashes.
#   check_golden.sh                compare against tests/host/golden/demos.sha256
#   check_golden.sh --record       rewrite the golden file from the current tree
#   check_golden.sh --determinism  render twice in separate processes and compare
#                                  (needs no golden file; valid on any compiler/CPU)
set -eu
here=$(cd "$(dirname "$0")" && pwd)
bin=${OUT:-$here/../../build/host}/fm1_host
golden=$here/golden/demos.sha256
if command -v sha256sum >/dev/null 2>&1; then sha() { sha256sum | cut -d' ' -f1; }
else sha() { shasum -a 256 | cut -d' ' -f1; }; fi
"$here/build.sh" >/dev/null
tmp=$(mktemp)
tmp2=$(mktemp)
trap 'rm -f "$tmp" "$tmp2"' EXIT
render_all() { # render_all <outfile>
    n=0
    while [ $n -lt 16 ]; do
        h=$("$bin" render $n 2756 - | sha)
        echo "demo$n $h" >>"$1"
        n=$((n + 1))
    done
}
render_all "$tmp"
case "${1:-}" in
--record)
    cp "$tmp" "$golden"; echo "recorded $golden"; exit 0 ;;
--determinism)
    render_all "$tmp2"
    if diff -u "$tmp" "$tmp2"; then echo "determinism: 16 demos identical across two runs"; exit 0; fi
    echo "determinism: MISMATCH" >&2; exit 1 ;;
esac
if diff -u "$golden" "$tmp"; then echo "golden: all 16 demos match"; else echo "golden: MISMATCH" >&2; exit 1; fi
