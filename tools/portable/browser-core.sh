#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
#
# Build the browser core (simulator.wasm) of the workbench page. A shell port of Build-Simulator in
# tools/Build-Web.ps1: same sources, include paths, flags and link options, with a macOS or Linux
# WASI SDK 27 instead of the Windows one.
#
#   tools/portable/browser-core.sh [-o OUT_DIR] [SRC_DIR]
#
# SRC_DIR is a tree laid out like this repository; it defaults to this repository and is only read.
# OUT_DIR defaults to SRC_DIR/build/web, the folder Build-Web.ps1 uses. Prints the path of simulator.wasm
# on the last line.
#
# Environment:
#   FM1_WASI_SDK  extracted wasi-sdk-27.0-* directory; by default the one tools/portable/fetch-wasi-sdk.sh
#                 puts in ~/.cache/fm1-open (FM1_CACHE).
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
OUT=""

usage() { sed -n '4,15p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }
while getopts "o:h" opt; do
  case "$opt" in
    o) OUT="$OPTARG" ;;
    *) usage ;;
  esac
done
shift $((OPTIND - 1))
[ $# -le 1 ] || usage

SRC="$(cd "${1:-$REPO}" && pwd)"
[ -f "$SRC/web/engine/Simulator.c" ] || { echo "error: $SRC has no web/engine/Simulator.c (not an fm1_open tree)" >&2; exit 1; }
WASI="${FM1_WASI_SDK:-}"
if [ -z "$WASI" ]; then
  WASI="$("$HERE/fetch-wasi-sdk.sh")" || { echo "error: WASI SDK 27 not found: run tools/portable/fetch-wasi-sdk.sh --download (or set FM1_WASI_SDK)" >&2; exit 1; }
fi
CLANG="$WASI/bin/clang"
[ -x "$CLANG" ] && [ -d "$WASI/share/wasi-sysroot" ] || { echo "error: set FM1_WASI_SDK to an extracted WASI SDK 27 directory (looked in $WASI)" >&2; exit 1; }
case "$("$CLANG" --version | head -1)" in
  *wasi-sdk*) ;;
  *) echo "error: $CLANG is not a WASI SDK clang" >&2; exit 1 ;;
esac

OUT="${OUT:-$SRC/build/web}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
WASM="$OUT/simulator.wasm"

# Sources, like Build-Web.ps1: every .c under app/ (except app/board/, which is hardware only), web/ and
# third_party/synth/, in ordinal order.
cd "$SRC"
SOURCES=()
while IFS= read -r f; do SOURCES+=("$f"); done < <(
  { find app web third_party/synth -type f -name '*.c' ! -path 'app/board/*'; } | LC_ALL=C sort)

options=(--sysroot="$WASI/share/wasi-sysroot"
  -std=c11 -O2 -fwrapv -fno-fast-math -ffp-contract=off
  -DFM1_HOST_TEST -DFM1_OPEN_WEB -Wall -Wextra -Werror
  -Wno-unused-function -Wno-array-parameter -mexec-model=reactor)
for dir in web/engine web/synth third_party/synth sdk/include app/synth app; do options+=("-I$SRC/$dir"); done
for f in "${SOURCES[@]}"; do options+=("$SRC/$f"); done
options+=(-Wl,--strip-all -Wl,-z,stack-size=131072 -Wl,--initial-memory=4194304 -Wl,--max-memory=33554432 -o "$WASM")

rm -f "$WASM"
echo "Compile the browser application (${#SOURCES[@]} sources, $("$CLANG" --version | head -1 | cut -d' ' -f3))" >&2
if ! "$CLANG" "${options[@]}"; then rm -f "$WASM"; exit 1; fi
echo "$WASM"
