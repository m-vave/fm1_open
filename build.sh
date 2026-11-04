#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
#
# build.sh is the macOS and Linux counterpart of build.bat: the same three builds, the same output folders,
# without Windows. It needs Docker and Node 18+. See docs/BUILDING.md.
#
#   ./build.sh                       firmware: build, run the SDK's checks, make build/dx7/generated/FM1_OFFICIAL_OPEN.fwsc
#   ./build.sh --web                 browser preview: build/preview/index.html
#   ./build.sh --package             firmware and the complete workbench page: build/official-web/
#   ./build.sh --setup               one-time downloads (JieLi's Linux toolchain, 26 MB; WASI SDK 27, 114-125 MB)
#
#   --source-url URL    the HTTPS address of the matching source archive (--package; or set FM1_SOURCE_URL),
#                       for example "https://github.com/<you>/fm1_open/archive/$(git rev-parse HEAD).zip"
#   --from-git REF      build a clean export of a commit instead of the working tree, in build/git-<commit>/
#
# The Windows build (build.bat, sdk/Build.ps1) stays the reference. What the checks and the comparison with
# it cover is in docs/BUILDING.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODE=firmware
REF=""
URL="${FM1_SOURCE_URL:-}"

usage() { sed -n '4,17p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
while [ $# -gt 0 ]; do
  case "$1" in
    --web) MODE=web; shift ;;
    --package) MODE=package; shift ;;
    --setup) MODE=setup; shift ;;
    --source-url) URL="${2:?--source-url needs a value}"; shift 2 ;;
    --from-git) REF="${2:?--from-git needs a commit}"; shift 2 ;;
    -h | --help) usage; exit 0 ;;
    *) echo "Use ./build.sh, ./build.sh --web, ./build.sh --package or ./build.sh --setup (./build.sh --help)." >&2; exit 2 ;;
  esac
done

command -v node >/dev/null || { echo "error: Node 18 or newer is required (https://nodejs.org)" >&2; exit 1; }

if [ "$MODE" = setup ]; then
  "$HERE/tools/docker/fetch-toolchain.sh" --download
  "$HERE/tools/portable/fetch-wasi-sdk.sh" --download
  exit 0
fi

if [ "$MODE" = package ] && [ -z "$URL" ]; then
  echo "error: set FM1_SOURCE_URL or pass --source-url with the HTTPS address of the matching source archive, e.g." >&2
  echo "  ./build.sh --package --source-url \"https://github.com/<you>/fm1_open/archive/\$(git rev-parse HEAD).zip\"" >&2
  exit 1
fi

if [ -n "$REF" ]; then
  COMMIT="$(git -C "$HERE" rev-parse --verify "$REF^{commit}")"
  BUILD="$HERE/build/git-${COMMIT:0:12}"
  SRC="$BUILD/src"
  rm -rf "$SRC"; mkdir -p "$SRC"
  git -C "$HERE" archive "$COMMIT" | tar -x -C "$SRC"
  echo "== building commit $COMMIT"
else
  BUILD="$HERE/build"
  SRC="$HERE"
fi

firmware() {
  echo "== firmware (JieLi Linux toolchain in Docker)"
  "$HERE/tools/docker/build.sh" -o "$BUILD/dx7" "$SRC"
  mkdir -p "$BUILD/dx7/generated"
  node "$HERE/tools/portable/fwsc.mjs" "$BUILD/dx7/sdk.elf" "$BUILD/dx7/generated/FM1_OFFICIAL_OPEN.fwsc" --root "$SRC"
}

case "$MODE" in
  firmware)
    firmware
    echo "== done: $BUILD/dx7/generated/FM1_OFFICIAL_OPEN.fwsc" ;;
  web)
    echo "== browser core"
    "$HERE/tools/portable/browser-core.sh" -o "$BUILD/web" "$SRC" >/dev/null
    node "$HERE/tools/portable/page.mjs" --root "$SRC" --wasm "$BUILD/web/simulator.wasm" --preview -o "$BUILD/preview"
    echo "== done: $BUILD/preview/index.html" ;;
  package)
    firmware
    echo "== browser core"
    "$HERE/tools/portable/browser-core.sh" -o "$BUILD/web" "$SRC" >/dev/null
    echo "== workbench page"
    node "$HERE/tools/portable/page.mjs" --root "$SRC" --fwsc "$BUILD/dx7/generated/FM1_OFFICIAL_OPEN.fwsc" \
      --identity "$BUILD/dx7/build-identity.json" --wasm "$BUILD/web/simulator.wasm" --source-url "$URL" -o "$BUILD/official-web"
    echo "== done: $BUILD/official-web/FM1-Official-Open-Source.html" ;;
esac
