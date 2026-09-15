#!/bin/sh
# Build the HTTP preview with clang.  The C engine owns the game state, the C++
# file owns the renderer, and both are compiled at -O3 exactly like the APK.
set -eu
cd "$(dirname "$0")/../.."
. tools/toolchain.sh
enjoer_pick_toolchain
OUT=${BUILD_DIR:-build-preview}
mkdir -p "$OUT"
rm -f "$OUT"/*.o

for source in $(enjoer_c_sources); do
    $CC $ENJOER_CFLAGS -c "$source" -o "$OUT/$(basename "$source" .c).o"
done
$CXX $ENJOER_CXXFLAGS -c src/vulkan_cube.cpp -o "$OUT/vulkan_cube.o"
$CC $ENJOER_CFLAGS -c tools/preview/host_main.c -o "$OUT/host_main.o"
$CC $ENJOER_CFLAGS -c tools/preview/frame_bmp.c -o "$OUT/frame.o"

$CXX "$OUT"/*.o -lm -o preview
printf '%s\n' "Built ./preview with: $CC $OPT_level"
