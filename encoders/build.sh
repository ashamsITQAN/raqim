#!/bin/sh
# Rebuilds the two WebAssembly encoders embedded in raqim.html:
#   lame.wasm  — LAME 3.100 (MP3), with Raqim's wrapper (encoders/lame/raqim_lame.c)
#   aac.wasm   — Fraunhofer FDK AAC (AAC-LC), with Raqim's wrapper (encoders/fdk-aac/raqim_aac.c)
# Then run:  python3 encoders/embed.py raqim.html  to put the new builds into the tool.
#
# Requirements: git, python3, and the Zig C compiler (pip install ziglang).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
WORK="$HERE/.build"
OUT="$HERE/dist"
mkdir -p "$WORK" "$OUT"
ZCC="python3 -m ziglang cc -target wasm32-wasi -O3"
ZCXX="python3 -m ziglang c++ -target wasm32-wasi -O3 -fno-exceptions -fno-rtti"

# --- sources, pinned to the exact commits used for Raqim ---
LAME_REPO=https://github.com/arseneyr/lame.git          # LAME 3.100 mirror
LAME_COMMIT=98db548e8e851defbba3184125ce10725355c332
FDK_REPO=https://github.com/mstorsjo/fdk-aac.git
FDK_COMMIT=ec82feb39662ac77e24ee9b324d5962e91bc79c5

fetch() { # repo commit dir
  if [ ! -d "$3/.git" ]; then git clone "$1" "$3"; fi
  git -C "$3" fetch --depth 1 origin "$2" 2>/dev/null || true
  git -C "$3" checkout -q "$2"
}
fetch "$LAME_REPO" "$LAME_COMMIT" "$WORK/lame"
fetch "$FDK_REPO"  "$FDK_COMMIT"  "$WORK/fdk-aac"

# --- LAME -> lame.wasm ---
( cd "$WORK/lame"
  $ZCC -DHAVE_CONFIG_H -I"$HERE/lame" -Iinclude -Ilibmp3lame \
    $(ls libmp3lame/*.c | grep -v mpglib_interface) "$HERE/lame/raqim_lame.c" \
    -mexec-model=reactor \
    -Wl,--export=enc_init,--export=enc_buf,--export=enc_out,--export=enc_encode,--export=enc_flush,--export=enc_free,--export=enc_quality,--export=enc_mode,--export=malloc \
    -Wl,--strip-all -o "$OUT/lame.wasm" )

# --- FDK AAC -> aac.wasm ---
( cd "$WORK/fdk-aac/cpp" 2>/dev/null || cd "$WORK/fdk-aac"
  INC="-IlibAACenc/include -IlibSYS/include -IlibFDK/include -IlibMpegTPEnc/include -IlibSBRenc/include -IlibPCMutils/include -IlibSACenc/include -IlibArithCoding/include -IlibSACdec/include -IlibDRCdec/include -IlibSBRdec/include -IlibAACdec/include -IlibMpegTPDec/include"
  mkdir -p "$WORK/aacobj"
  for f in $(cat "$HERE/fdk-aac/sources.txt"); do
    $ZCXX -Wno-everything $INC -c "$f" -o "$WORK/aacobj/$(echo "$f" | tr '/' '_').o"
  done
  $ZCC -Wno-everything $INC -c "$HERE/fdk-aac/raqim_aac.c" -o "$WORK/aacobj/wrapper.o"
  $ZCXX "$WORK"/aacobj/*.o -mexec-model=reactor \
    -Wl,--export=aac_init,--export=aac_buf,--export=aac_frame_len,--export=aac_delay,--export=aac_asc_len,--export=aac_asc,--export=aac_out,--export=aac_consumed,--export=aac_encode,--export=aac_free \
    -Wl,--strip-all -o "$OUT/aac.wasm" )

ls -la "$OUT"
