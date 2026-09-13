#!/bin/sh
# Native regression for the small C/C++ cube. ASAN=1 enables sanitizers.
set -eu
cd "$(dirname "$0")/../.."
CC=${CC:-gcc}
CXX=${CXX:-g++}
OUT=${BUILD_DIR:-build-tests/cube}
mkdir -p "$OUT"
CFLAGS="-std=c99 -O2 -Wall -Wextra -Werror -I./src"
CXXFLAGS="-std=c++17 -O2 -Wall -Wextra -Werror -I./src"
LDFLAGS=""
if [ "${ASAN:-0}" = 1 ]; then
    CFLAGS="$CFLAGS -g -fsanitize=address,undefined -fno-omit-frame-pointer"
    CXXFLAGS="$CXXFLAGS -g -fsanitize=address,undefined -fno-omit-frame-pointer"
    LDFLAGS="$LDFLAGS -fsanitize=address,undefined"
fi
$CC $CFLAGS -c src/core/log.c -o "$OUT/log.o"
$CC $CFLAGS -c src/core/state.c -o "$OUT/state.o"
$CC $CFLAGS -c src/cube_game.c -o "$OUT/cube_game.o"
$CXX $CXXFLAGS -c src/dawn_cube.cpp -o "$OUT/dawn_cube.o"
$CC $CFLAGS -c tools/tests/cube.c -o "$OUT/cube_test.o"
$CXX "$OUT/log.o" "$OUT/state.o" "$OUT/cube_game.o" "$OUT/dawn_cube.o" \
    "$OUT/cube_test.o" $LDFLAGS -lm -o "$OUT/cube"
"$OUT/cube"
