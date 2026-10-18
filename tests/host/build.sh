#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
# Native build of the open engine mirror plus the offline test driver.
# Same sources, include order and defines as tools/Build-Web.ps1 (minus wasm).
set -eu
root=$(cd "$(dirname "$0")/../.." && pwd)
out=${OUT:-$root/build/host}
cc=${CC:-$(command -v clang || command -v cc)}
mkdir -p "$out"
cd "$root"
srcs=$(find app web third_party/synth -name '*.c' -not -path 'app/board/*' | LC_ALL=C sort)
# Unquoted $srcs is intentional: repo paths contain no spaces.
# shellcheck disable=SC2086
build() { # build <driver.c> <output>
    # shellcheck disable=SC2086
    $cc -std=c11 -O2 -fwrapv -fno-fast-math -ffp-contract=off \
        -DFM1_HOST_TEST -DFM1_OPEN_WEB -Wall -Wextra -Werror \
        -Wno-unused-function -Wno-array-parameter \
        -I web/engine -I web/synth -I third_party/synth -I sdk/include -I app/synth -I app \
        $srcs "$1" -lm -o "$2"
    echo "built $2"
}
build tests/host/render.c "$out/fm1_host"
build tests/host/workbench_bridge.c "$out/workbench_bridge"
# Each standalone test is tests/host/<name>_test.c with its own main().
for t in tests/host/*_test.c; do
    [ -e "$t" ] || continue
    build "$t" "$out/$(basename "$t" .c)"
done
