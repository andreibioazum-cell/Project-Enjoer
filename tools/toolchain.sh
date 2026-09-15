# Shared toolchain selection for every native build script in this repository.
#
# The project is built with clang at -O3: the Android NDK *is* clang, so a host
# build uses the same compiler family as the shipping APK, and -O3 is what the
# AOT-compiled game code ships with.  gcc is only a last resort for machines
# with no clang at all, and `python3 -m ziglang cc` is used when clang is not
# installed but Zig is — that keeps `tools/preview` working in a bare container.
#
# When ccache is installed it wraps the compiler automatically, so a rebuild
# after touching one file recompiles only that file.  Set ENJOER_NO_CCACHE=1
# to opt out.

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
    if [ "${ENJOER_NO_CCACHE:-0}" != 1 ] && command -v ccache >/dev/null 2>&1; then
        case " $CC " in
            *" ccache "*) ;;
            *) CC="ccache $CC"; CXX="ccache $CXX";;
        esac
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
src/ds_font.c
src/ds_ttf.c
src/generated/clicker.c
src/game.c
SOURCES
}

# Compile $1 (a .c/.cpp path) into $OUT only when the object is missing or older
# than the source.  ENJOER_CLEAN=1 forces a full rebuild.  The C++ renderer is
# the slowest unit by far, so skipping it makes iteration noticeably faster.
enjoer_compile_one() {
    source_path=$1
    object_path="$OUT/$(basename "$source_path" | sed 's/\.[^.]*$/.o/')"
    case "$source_path" in
        *.cpp) compile_flags=$ENJOER_CXXFLAGS; compile_cc=$CXX;;
        *) compile_flags=$ENJOER_CFLAGS; compile_cc=$CC;;
    esac
    if [ "${ENJOER_CLEAN:-0}" = 1 ] || [ ! -f "$object_path" ] || [ "$source_path" -nt "$object_path" ]; then
        # shellcheck disable=SC2086
        $compile_cc $compile_flags -c "$source_path" -o "$object_path"
    fi
}
