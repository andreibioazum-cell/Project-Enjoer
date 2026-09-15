# Shared toolchain selection for every native build script in this repository.
#
# The project is built with clang at -O3: the Android NDK *is* clang, so a host
# build uses the same compiler family as the shipping APK, and -O3 is what makes
# the interpreted DimScript VM fast enough to run a game at 60 fps.  gcc is only
# a last resort for machines with no clang at all, and `python3 -m ziglang cc`
# is used when clang is not installed but Zig is — that keeps `tools/preview`
# working inside a bare container.

: "${OPT_level:=-O3}"

enjoer_pick_toolchain() {
    if command -v clang >/dev/null 2>&1; then
        : "${CC:=clang}"
        : "${CXX:=clang++}"
    elif python3 -c "import ziglang" >/dev/null 2>&1; then
        : "${CC:=python3 -m ziglang cc}"
        : "${CXX:=python3 -m ziglang c++}"
    else
        : "${CC:=gcc}"
        : "${CXX:=g++}"
    fi
    ENJOER_CFLAGS="-std=c99 ${OPT_level} -g -Wall -Wextra -Werror -I./src"
    # -Wno-nullability-completeness: libc++ headers from Zig's bundled standard
    # library are not annotated for Clang's nullability pass, which would
    # otherwise drown the build in warnings from third-party headers.
    ENJOER_CXXFLAGS="-std=c++17 ${OPT_level} -g -Wall -Wextra -Werror -Wno-nullability-completeness -I./src"
    export CC CXX ENJOER_CFLAGS ENJOER_CXXFLAGS
}

# Every C translation unit of the engine, in dependency order.  STRICT COMPILER MODE
# No VM, no refcount, manual memory, speed like C, AOT to machine code.
enjoer_c_sources() {
    cat <<'SOURCES'
src/core/log.c
src/core/state.c
src/enjoer_draw.c
src/dimscript_runtime.c
src/ds_manifest.c
src/ds_files.c
src/ds_image.c
src/ds_png.c
src/generated/clicker.c
src/cube_game.c
SOURCES
}
