#!/bin/sh
# Native regression for the whole engine: the Python front-end, the DimScript
# VM in C, the C game layer and the C++ renderer.  ASAN=1 enables sanitizers.
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

$CC $ENJOER_CFLAGS -c tools/tests/vm.c -o "$OUT/vm_test.o"
$CC $OUT/ds_vm.o $OUT/ds_vm_heap.o $OUT/ds_vm_lang.o $OUT/ds_vm_exec.o \
    $OUT/dimscript_runtime.o $OUT/enjoer_draw.o $OUT/vm_test.o $OUT/log.o $OUT/state.o \
    $SANITIZE -lm -o "$OUT/vm"

$CC $ENJOER_CFLAGS -c tools/tests/frame_dump.c -o "$OUT/frame_dump.o"
$CC $OUT/ds_vm.o $OUT/ds_vm_heap.o $OUT/ds_vm_lang.o $OUT/ds_vm_exec.o \
    $OUT/dimscript_runtime.o $OUT/enjoer_draw.o $OUT/ds_manifest.o $OUT/ds_files.o \
    $OUT/frame_dump.o $OUT/log.o $OUT/state.o $SANITIZE -lm -o "$OUT/frame_dump"

python3 tools/tests/dimscript.py
"$OUT/vm"
python3 tools/tests/parity.py
ENJOER_CC="$CC" python3 tools/tests/aot.py
"$OUT/cube"
