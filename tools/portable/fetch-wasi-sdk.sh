#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
#
# One-time fetch of the WASI SDK 27 that compiles the browser core (the same release tools/Build-Web.ps1
# downloads on Windows), from the WebAssembly/wasi-sdk GitHub release into ~/.cache/fm1-open. The archive
# is verified against the SHA-256 that GitHub publishes for the asset, then extracted into its own
# directory. Downloads only with --download; without it, it just verifies what is already there.
#
#   tools/portable/fetch-wasi-sdk.sh [--download]
#
# Prints the extracted directory (what FM1_WASI_SDK should point at). Environment: FM1_CACHE (default
# ~/.cache/fm1-open).
#
# Tested: macOS on Apple Silicon. The other three platforms use the digests GitHub publishes for the same
# release and have not been run by the authors of this script.
set -euo pipefail

CACHE="${FM1_CACHE:-$HOME/.cache/fm1-open}"
case "$(uname -s)-$(uname -m)" in
  Darwin-arm64)   name="wasi-sdk-27.0-arm64-macos";   want="055c3dc2766772c38e71a05d353e35c322c7b2c6458a36a26a836f9808a550f8" ;;
  Darwin-x86_64)  name="wasi-sdk-27.0-x86_64-macos";  want="163dfd47f989b1a682744c1ae1f0e09a83ff5c4bbac9dcd8546909ab54cda5a1" ;;
  Linux-x86_64)   name="wasi-sdk-27.0-x86_64-linux";  want="b7d4d944c88503e4f21d84af07ac293e3440b1b6210bfd7fe78e0afd92c23bc2" ;;
  Linux-aarch64 | Linux-arm64) name="wasi-sdk-27.0-arm64-linux"; want="4cf4c553c4640e63e780442146f87d83fdff5737f988c06a6e3b2f0228e37665" ;;
  *) echo "error: no WASI SDK 27 build is pinned for $(uname -s) $(uname -m); download one from https://github.com/WebAssembly/wasi-sdk/releases/tag/wasi-sdk-27 and set FM1_WASI_SDK" >&2; exit 1 ;;
esac
url="https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-27/$name.tar.gz"
archive="$CACHE/dl-wasi-sdk/$name.tar.gz"
target="$CACHE/wasi-sdk"
sha256() { if command -v sha256sum >/dev/null; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi; }

if [ ! -f "$archive" ]; then
  [ "${1:-}" = "--download" ] || { echo "missing $archive; re-run with --download to fetch $url" >&2; exit 1; }
  mkdir -p "$(dirname "$archive")"
  curl -fL --retry 2 -o "$archive" "$url"
fi
got="$(sha256 "$archive")"
[ "$got" = "$want" ] || { echo "error: $archive sha256 $got != pinned $want" >&2; exit 1; }
if [ ! -x "$target/$name/bin/clang" ]; then
  mkdir -p "$target"
  tar -xzf "$archive" -C "$target"
fi
echo "$target/$name"
