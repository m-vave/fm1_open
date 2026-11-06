#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
#
# Runs INSIDE the linux/amd64 container (see build.sh). Port of the compile and
# link steps of sdk/Build.ps1 (-FirmwareOnly, profile dx7) for the JieLi Linux
# toolchain. Flags, per-source options, object order and link line are kept
# identical to Build.ps1; only tool names and path spelling differ.
#
#   /src         source tree, read-only (sdk/, app/, ...)
#   /out         build directory; build.sh has already put generated/sources.txt,
#                generated/build_identity.c and generated/linker there
#   /opt/jieli   extracted jieli-linux-toolchains release, read-only
#
# Everything checkable without the toolchain (SDK hashes, symbol gates, image cap)
# lives in gates.mjs and runs on the host.
set -euo pipefail

SRC=/src
OUT=/out
TC=/opt/jieli
CLANG=$TC/common/bin/clang
GOLD=$TC/common/bin/LLVMgold.so
LD=$TC/pi32v2/bin/ld
NM=$TC/pi32v2/bin/nm

# JieLi's README: linking can open many files at once.
ulimit -n 8192 2>/dev/null || echo "warning: could not raise the open-file limit" >&2

cd "$OUT"
rm -rf obj
mkdir -p obj
rm -f sdk.elf sdk.map nm.txt commands.log

# Windows Build.ps1 flags, verbatim.
flags=(-target pi32v2 -integrated-as -mcpu=r3 -mfprev1 -Wuninitialized
  -Wno-invalid-noreturn -fno-common -flto -fallow-pointer-null -fprefer-gnu-section
  -femulated-tls -Wno-shift-negative-value -Wframe-larger-than=2560 -fms-extensions
  -fno-unwind-tables -ffunction-sections -fdata-sections -fmessage-length=0
  -cl-single-precision-constant -mllvm -pi32v2-large-program=true -D__GCC_PI32V2__ -O2)
for dir in sdk/include sdk/include/libc app/synth sdk/config app app/board; do
  flags+=("-I$SRC/$dir")
done

log() { printf '%q ' "$@" >>commands.log; printf '\n' >>commands.log; }
run() { log "$@"; "$@"; }

{
  echo "clang: $("$CLANG" --version | head -1)"
  echo "ld:    $("$LD" --version | head -1)"
  echo "nm:    $("$NM" --version | head -1)"
  echo "host:  $(uname -m) (container), Rosetta/QEMU is transparent to the tools"
} >toolchain-versions.txt

objects=()
index=0
while IFS= read -r source; do
  [ -n "$source" ] || continue
  object="obj/${index}_${source//[^a-zA-Z0-9._-]/_}.obj"
  index=$((index + 1))
  options=("${flags[@]}")
  # The PI32 LTO plugin can discard application definitions when a native SDK
  # object has weak defaults: keep these entry units native.
  case "$source" in
    app/screen.c | app/transfer.c | app/midi_router.c) options+=(-fno-lto) ;;
  esac
  [ "$source" = app/board/panel.c ] && options+=(-fno-lto -fno-pic)
  options+=(-fwrapv -fno-fast-math -ffp-contract=off)
  # Storage and screen run only from control; audio DSP stays on -O2.
  case "$source" in
    app/project.c | app/transfer.c | app/samples.c | app/screen.c | app/presets.c | \
      app/midi_router.c | app/fm_factory.c | app/fm_library.c) options+=(-Oz) ;;
  esac
  [ "$source" = app/screen.c ] && options+=(-fno-inline-functions)
  echo "Compile $source"
  run "$CLANG" "${options[@]}" -c "$SRC/$source" -o "$object"
  objects+=("$object")
done <generated/sources.txt

echo "Compile generated/build_identity.c"
run "$CLANG" "${flags[@]}" -c generated/build_identity.c -o obj/build_identity.obj
objects+=(obj/build_identity.obj)

link=(--plugin "$GOLD" -o sdk.elf "${objects[@]}"
  -flto --plugin-opt=-pi32v2-always-use-itblock=false --plugin-opt=-enable-ipra=true
  --plugin-opt=-pi32v2-merge-max-offset=4096 --plugin-opt=-pi32v2-enable-simd=true --plugin-opt=mcpu=r3
  --plugin-opt=-global-merge-on-const --plugin-opt=-inline-threshold=5 --plugin-opt=-inline-max-allocated-size=32
  --plugin-opt=-inline-normal-into-special-section=true
  --plugin-opt=-dont-used-symbol-list=malloc,free,sprintf,printf,puts,putchar
  --plugin-opt=-pi32v2-enable-rep-memop --plugin-opt=-emulated-tls --sort-common
  --plugin-opt=-pi32v2-large-program=true --plugin-opt=-pi32v2-csync-before-rti=true --start-group
  --whole-archive "$SRC/sdk/lib/libfm1_engine.a" --no-whole-archive
  "$SRC/sdk/lib/libc.a" "$SRC/sdk/lib/libcompiler_rt.a"
  --end-group --gc-sections -Tgenerated/linker -M=sdk.map
  --undefined=connectivity_wl82_bluetooth_start --undefined=audio_app_bluetooth_open
  --undefined=bt_get_local_name --undefined=bt_get_mac_addr)

echo "Link dx7 using prebuilt FM1 SDK"
run "$LD" "${link[@]}"

# Gates run on the host from these: Test-Firmware reads llvm-nm output.
"$NM" --print-size sdk.elf >nm.txt
echo "Firmware: $OUT/sdk.elf ($(wc -c <sdk.elf | tr -d ' ') bytes ELF, $(wc -l <nm.txt | tr -d ' ') symbols)"
