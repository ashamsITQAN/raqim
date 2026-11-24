#!/bin/sh
# Builds Raqim.exe (Windows x64) with the Zig C compiler.
#   - two engine DLLs: generic x86-64 and x86-64-v3 (AVX2/FMA/BMI2), chosen at run time
#   - the DLLs and raqim.html are embedded in the exe (C23 #embed)
#
# Usage (from the repository root):
#   sh encoders/build.sh            # fetches the pinned LAME and FDK AAC sources into encoders/.build
#   sh native/build_windows.sh      # -> native/dist/Raqim-<version>.exe
# Requirements: python3 and the Zig C compiler (pip install ziglang).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
VER=$(sed -n 's/^#define VERSION "\(.*\)"/\1/p' "$HERE/src/main.c")
ROOT=$(cd "$HERE/.." && pwd)
ENC="$ROOT/encoders"
LAME=$(cd "${1:-$ENC/.build/lame}" && pwd)
FDKSRC=$(cd "${2:-$ENC/.build/fdk-aac/cpp}" && pwd)
HTML="${3:-$ROOT/raqim.html}"
OUT=$(mkdir -p "${4:-$HERE/dist}" && cd "${4:-$HERE/dist}" && pwd)
WORK="$OUT/.obj"; mkdir -p "$WORK"
# FDK AAC: use its portable (non-x86) code path, the same one the browser (WebAssembly) build uses,
# so the desktop program writes byte-for-byte the same M4A files as the browser tool.
FDK="$WORK/fdk"; rm -rf "$FDK"; cp -r "$FDKSRC" "$FDK"
sed -i 's/(defined(_MSC_VER) \&\& defined(_M_X64)) || defined(__x86_64__)$/(defined(_MSC_VER) \&\& defined(_M_X64)) || (defined(__x86_64__) \&\& !defined(RAQIM_PORTABLE_FDK))/' "$FDK/libFDK/include/FDK_archdef.h"
grep -q RAQIM_PORTABLE_FDK "$FDK/libFDK/include/FDK_archdef.h"
T=x86_64-windows-gnu
# -ffp-contract=off: no fused multiply-add, so LAME gives exactly the browser's results even on AVX2/FMA
CC="python3 -m ziglang cc -target $T -O3 -ffp-contract=off -DRAQIM_PORTABLE_FDK"
CXX="python3 -m ziglang c++ -target $T -O3 -ffp-contract=off -DRAQIM_PORTABLE_FDK -fno-exceptions -fno-rtti"
FDKINC="-I$FDK/libAACenc/include -I$FDK/libSYS/include -I$FDK/libFDK/include -I$FDK/libMpegTPEnc/include -I$FDK/libSBRenc/include -I$FDK/libPCMutils/include -I$FDK/libSACenc/include -I$FDK/libArithCoding/include -I$FDK/libSACdec/include -I$FDK/libDRCdec/include -I$FDK/libSBRdec/include -I$FDK/libAACdec/include -I$FDK/libMpegTPDec/include"
JOBS=$(nproc 2>/dev/null || echo 4)

engine() { # variant cpu
  V=$1; CPU=$2; O="$WORK/$V"; mkdir -p "$O"
  # LAME (C) and the Raqim wrappers
  for f in $(ls "$LAME"/libmp3lame/*.c | grep -v mpglib_interface); do echo "$CC -mcpu=$CPU -DHAVE_CONFIG_H -I$ENC/lame -I$LAME/include -I$LAME/libmp3lame -Wno-everything -c $f -o $O/lame_$(basename $f).o"; done > "$O/cmds"
  echo "$CC -mcpu=$CPU -DHAVE_CONFIG_H -I$ENC/lame -I$LAME/include -I$LAME/libmp3lame -Wno-everything -c $ENC/lame/raqim_lame.c -o $O/w_lame.o" >> "$O/cmds"
  echo "$CC -mcpu=$CPU $FDKINC -Wno-everything -c $ENC/fdk-aac/raqim_aac.c -o $O/w_aac.o" >> "$O/cmds"
  echo "$CC -mcpu=$CPU -std=c2x -DRAQIM_BUILD_DLL -I$HERE/src -c $HERE/src/engine.c -o $O/engine.o" >> "$O/cmds"
  for f in $(cat "$ENC/fdk-aac/sources.txt"); do echo "$CXX -mcpu=$CPU $FDKINC -Wno-everything -c $FDK/$f -o $O/fdk_$(echo $f | tr / _).o"; done >> "$O/cmds"
  xargs -P "$JOBS" -I{} sh -c '{}' < "$O/cmds"
  $CXX -shared "$O"/*.o -s -o "$HERE/src/raqim_engine_$V.dll"
}
engine x64 x86_64
engine avx2 x86_64_v3

cp "$HTML" "$HERE/src/raqim.html"
python3 -m ziglang cc -target $T -O2 -std=c2x -municode -Wl,--subsystem,windows -I"$HERE/src" \
  "$HERE/src/main.c" "$HERE/src/raqim.rc" -lws2_32 -lshell32 -ladvapi32 -luser32 -s -o "$OUT/Raqim-$VER.exe"
ls -la "$OUT/Raqim-$VER.exe" "$HERE"/src/raqim_engine_*.dll
