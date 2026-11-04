#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
#
# One-time fetch of JieLi's Linux toolchain (26,027,508 bytes, pinned by SHA-256
# in toolchain.json) into ~/.cache/fm1-open, verified and extracted into its own
# directory. Downloads only with --download; without it, it just verifies what is
# already there. The extracted binaries are never run on the host (build.sh runs
# them inside the linux/amd64 container).
#
#   tools/docker/fetch-toolchain.sh [--download]
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CACHE="${FM1_CACHE:-$HOME/.cache/fm1-open}"
release="$(sed -n 's/.*"release": "\(.*\)",/\1/p' "$HERE/toolchain.json")"
url="$(sed -n 's/.*"archive": "\(.*\)",/\1/p' "$HERE/toolchain.json")"
want="$(sed -n 's/.*"archive_sha256": "\(.*\)",/\1/p' "$HERE/toolchain.json")"
archive="$CACHE/dl-jieli-toolchain/$release.tar.xz"
target="$CACHE/jieli-toolchain"

if [ ! -f "$archive" ]; then
  [ "${1:-}" = "--download" ] || { echo "missing $archive; re-run with --download to fetch $url" >&2; exit 1; }
  mkdir -p "$(dirname "$archive")"
  curl -fL --retry 2 -o "$archive" "$url"
fi
got="$(shasum -a 256 "$archive" | cut -d' ' -f1)"
[ "$got" = "$want" ] || { echo "error: $archive sha256 $got != pinned $want" >&2; exit 1; }
if [ ! -x "$target/$release/common/bin/clang" ]; then
  [ -z "$(ls -A "$target" 2>/dev/null)" ] || { echo "error: $target exists and is not an extracted $release" >&2; exit 1; }
  mkdir -p "$target"
  tar -xJf "$archive" -C "$target"
fi
echo "$target/$release"
