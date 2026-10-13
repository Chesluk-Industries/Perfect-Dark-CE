#!/usr/bin/env bash
# Build the Perfect Dark PC port (ntsc-final, x86_64) and refresh ../../pd_pc_build
# (or the folder in PD_OUT_DIR). Run from the MSYS2 "MINGW64" shell (not the
# plain "MSYS" one):
#   cd "<the port folder>"
#   ./build_windows.sh            # incremental build
#   ./build_windows.sh clean      # wipe the build folder first
#   PD_OUT_DIR=../out ./build_windows.sh   # put the exe somewhere else
set -e
cd "$(dirname "$0")"

if [ "$MSYSTEM" != "MINGW64" ]; then
  echo "Please run this from the MSYS2 MINGW64 shell (current: ${MSYSTEM:-unknown})."
  exit 1
fi

BUILD_DIR=build_win
OUT_DIR="${PD_OUT_DIR:-../../pd_pc_build}"

if [ "$1" = "clean" ]; then
  rm -rf "$BUILD_DIR"
fi

cmake -G"Unix Makefiles" -B "$BUILD_DIR" .
cmake --build "$BUILD_DIR" -j"$(nproc)" -- -O

mkdir -p "$OUT_DIR/data"
cp "$BUILD_DIR/pd.x86_64.exe" "$OUT_DIR/"
cp /mingw64/bin/{SDL2.dll,zlib1.dll,libwinpthread-1.dll,libgcc_s_seh-1.dll} "$OUT_DIR/"
if [ -d port/data ]; then
  cp -r port/data/. "$OUT_DIR/data/" # the game's extra data (data/sounds/: the ragdolls' sounds)
fi

echo
echo "Done: $(cd "$OUT_DIR" && pwd)/pd.x86_64.exe"
