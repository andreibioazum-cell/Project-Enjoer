#!/bin/sh
# Build the HTTP preview. C owns the game state; C++ owns the renderer.
set -eu
cd "$(dirname "$0")/../.."
CC=${CC:-gcc}
CXX=${CXX:-g++}
OUT=${BUILD_DIR:-build-preview}
mkdir -p "$OUT"
CFLAGS="-std=c99 -O2 -Wall -Wextra -Werror -I./src"
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Werror -I./src"

$CC $CFLAGS -c src/core/log.c -o "$OUT/log.o"
$CC $CFLAGS -c src/core/state.c -o "$OUT/state.o"
$CC $CFLAGS -c src/cube_game.c -o "$OUT/cube_game.o"
$CXX $CXXFLAGS -c src/vulkan_cube.cpp -o "$OUT/vulkan_cube.o"
$CC $CFLAGS -c tools/preview/host_main.c -o "$OUT/host_main.o"
$CC $CFLAGS -c tools/preview/frame_bmp.c -o "$OUT/frame.o"
$CXX "$OUT/log.o" "$OUT/state.o" "$OUT/cube_game.o" "$OUT/vulkan_cube.o" \
    "$OUT/host_main.o" "$OUT/frame.o" -lm -o preview
printf '%s\n' 'Built ./preview'
