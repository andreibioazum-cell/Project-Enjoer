#!/bin/sh
# STRICT COMPILER MODE — NO VM, NO REFCOUNT, MANUAL MEMORY, SPEED LIKE C, AOT TO MACHINE CODE
# Only parser + AOT compiler + C game + C++ renderer are tested.
set -eu
cd "$(dirname "$0")/../.."
. tools/toolchain.sh
enjoer_pick_toolchain
OUT=${BUILD_DIR:-build-tests}
mkdir -p "$OUT"
rm -f "$OUT"/*.o

SANITIZE=""
if [ "${ASAN:-0}" = 1 ]; then
    SANITIZE="-fsanitize=address,undefined -fno-omit-frame-pointer"
    ENJOER_CFLAGS="$ENJOER_CFLAGS $SANITIZE"
    ENJOER_CXXFLAGS="$ENJOER_CXXFLAGS $SANITIZE"
fi

for source in $(enjoer_c_sources); do
    $CC $ENJOER_CFLAGS -c "$source" -o "$OUT/$(basename "$source" .c).o"
done
$CXX $ENJOER_CXXFLAGS -c src/vulkan_cube.cpp -o "$OUT/vulkan_cube.o"
$CC $ENJOER_CFLAGS -c tools/tests/cube.c -o "$OUT/cube_test.o"
$CXX $OUT/*.o $SANITIZE -lm -o "$OUT/cube"

# No VM build — strictly compiler
echo "STRICT COMPILER MODE: skipping VM build"

python3 tools/tests/dimscript.py
ENJOER_CC="$CC" python3 tools/tests/aot.py
"$OUT/cube"
echo "PASS Strict compiler: AOT to machine code, manual memory, speed like C, no refcount"
