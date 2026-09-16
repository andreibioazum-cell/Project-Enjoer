#!/bin/sh
# STRICT COMPILER MODE — NO VM, NO REFCOUNT, MANUAL MEMORY, SPEED LIKE C, AOT TO MACHINE CODE
# Only parser + AOT compiler + C game + C++ renderer are tested.
# Incremental: only changed files recompile (ENJOER_CLEAN=1 forces a rebuild).
set -eu
cd "$(dirname "$0")/../.."
. tools/toolchain.sh
enjoer_pick_toolchain
OUT=${BUILD_DIR:-build-tests}
mkdir -p "$OUT"
if [ "${ENJOER_CLEAN:-0}" = 1 ]; then
    rm -f "$OUT"/*.o
fi

SANITIZE=""
if [ "${ASAN:-0}" = 1 ]; then
    SANITIZE="-fsanitize=address,undefined -fno-omit-frame-pointer"
    ENJOER_CFLAGS="$ENJOER_CFLAGS $SANITIZE"
    ENJOER_CXXFLAGS="$ENJOER_CXXFLAGS $SANITIZE"
    # The AOT and gamepack drivers build their own binaries: they honor the
    # same flags through this variable, so ASAN=1 covers every C binary.
    export ENJOER_SANITIZE="$SANITIZE"
fi

for source in $(enjoer_c_sources); do
    enjoer_compile_one "$source"
done
enjoer_compile_one src/vulkan_2d.cpp
enjoer_compile_one tools/tests/engine.c
# The link below globs every object of the suite into the engine test binary;
# aot.py builds its own driver separately, so no stray main() collides here.
$CXX "$OUT"/log.o "$OUT"/state.o "$OUT"/enjoer_draw.o "$OUT"/dimscript_runtime.o \
    "$OUT"/ds_manifest.o "$OUT"/ds_files.o "$OUT"/ds_image.o "$OUT"/ds_png.o \
    "$OUT"/ds_font.o "$OUT"/ds_ttf.o "$OUT"/clicker.o "$OUT"/game.o "$OUT"/vulkan_2d.o \
    "$OUT"/engine.o $SANITIZE -lm -o "$OUT/engine"

# No VM build — strictly compiler
echo "STRICT COMPILER MODE: skipping VM build"

python3 tools/tests/dimscript.py
ENJOER_CC="$CC" python3 tools/tests/aot.py
python3 tools/tests/pack.py
ENJOER_CC="$CC" python3 tools/tests/temps.py
"$OUT/engine"
echo "PASS Strict compiler: AOT to machine code, manual memory, speed like C, no refcount"
