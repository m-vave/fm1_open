#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
#
# Build the FM1 firmware (what `sdk/Build.ps1 -FirmwareOnly` builds, minus the package) in a
# linux/amd64 container with JieLi's Linux toolchain, then run the Build.ps1 symbol gates and the
# app-image size limit on the result. Needs Docker and Node 18+; no PowerShell, no Windows.
#
#   tools/docker/build.sh [-o OUT_DIR] [SRC_DIR]
#
# SRC_DIR is a tree laid out like this repository (sdk/, app/, third_party/, ...); it defaults to
# this repository. It is mounted read-only: nothing in it is written. OUT_DIR defaults to
# SRC_DIR/build/dx7, the folder sdk/Build.ps1 uses on Windows (build/ is gitignored).
#
# Environment:
#   FM1_LINUX_TOOLCHAIN  extracted jieli-linux-toolchains-* directory
#                        (default <FM1_CACHE, ~/.cache/fm1-open>/jieli-toolchain/<release in toolchain.json>)
#   FM1_BUILD_IMAGE      image tag (default fm1-open-build)
#
# Nothing is downloaded here: fetch the toolchain once with tools/docker/fetch-toolchain.sh --download.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
RELEASE="$(sed -n 's/.*"release": "\(.*\)",/\1/p' "$HERE/toolchain.json")"
TOOLCHAIN="${FM1_LINUX_TOOLCHAIN:-${FM1_CACHE:-$HOME/.cache/fm1-open}/jieli-toolchain/$RELEASE}"
IMAGE="${FM1_BUILD_IMAGE:-fm1-open-build}"
OUT=""

usage() { sed -n '4,18p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 2; }
while getopts "o:h" opt; do
  case "$opt" in
    o) OUT="$OPTARG" ;;
    *) usage ;;
  esac
done
shift $((OPTIND - 1))
[ $# -le 1 ] || usage

SRC="$(cd "${1:-$REPO}" && pwd)"
[ -f "$SRC/sdk/Build.ps1" ] && [ -f "$SRC/app/version.h" ] || { echo "error: $SRC does not look like an fm1_open tree" >&2; exit 1; }
[ -x "$TOOLCHAIN/common/bin/clang" ] || { echo "error: the Linux toolchain is not at $TOOLCHAIN: run tools/docker/fetch-toolchain.sh --download (or set FM1_LINUX_TOOLCHAIN)" >&2; exit 1; }
command -v node >/dev/null || { echo "error: node is required for the gates" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "error: the Docker daemon is not running (start Docker Desktop or the docker service)" >&2; exit 1; }

OUT="${OUT:-$SRC/build/dx7}"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
echo "source:    $SRC"
echo "toolchain: $TOOLCHAIN"
echo "output:    $OUT"

# Invalidate stale results first, like Build.ps1.
rm -f "$OUT/firmware-report.json" "$OUT/sdk.elf" "$OUT/sdk.map" "$OUT/nm.txt"

node "$HERE/gates.mjs" inputs "$SRC" "$TOOLCHAIN"
node "$HERE/gates.mjs" prepare "$SRC" "$OUT"

docker build --platform linux/amd64 --quiet -t "$IMAGE" -f "$HERE/Dockerfile" "$HERE" >/dev/null
start=$SECONDS
docker run --rm --platform linux/amd64 \
  --user "$(id -u):$(id -g)" \
  --ulimit nofile=8192:8192 \
  -v "$SRC:/src:ro" \
  -v "$OUT:/out" \
  -v "$TOOLCHAIN:/opt/jieli:ro" \
  -v "$HERE/container-build.sh:/opt/fm1/container-build.sh:ro" \
  "$IMAGE" 2>&1 | tee "$OUT/build.log"
echo "container build: $((SECONDS - start)) s"

node "$HERE/gates.mjs" firmware "$SRC" "$OUT" | tee "$OUT/gates.log"
