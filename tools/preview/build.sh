#!/bin/sh
# Build the HTTP preview with clang.  The C engine owns the game state, the C++
# file owns the renderer, and both are compiled at -O3 exactly like the APK.
# Incremental: only changed files recompile (ENJOER_CLEAN=1 forces a rebuild).
set -eu
cd "$(dirname "$0")/../.."
. tools/toolchain.sh
enjoer_pick_toolchain
OUT=${BUILD_DIR:-build-preview}
mkdir -p "$OUT"
if [ "${ENJOER_CLEAN:-0}" = 1 ]; then
    rm -f "$OUT"/*.o
fi

for source in $(enjoer_c_sources); do
    enjoer_compile_one "$source"
done
enjoer_compile_one src/vulkan_2d.cpp
enjoer_compile_one tools/preview/host_main.c
enjoer_compile_one tools/preview/frame_bmp.c

$CXX "$OUT"/*.o -lm -o preview
printf '%s\n' "Built ./preview with: $CC $OPT_level"
