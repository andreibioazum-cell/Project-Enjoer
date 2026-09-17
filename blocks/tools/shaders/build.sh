#!/bin/sh
# Compiles the Vulkan shaders in src/vk/shaders/*.vert|.frag into SPIR-V and
# embeds them as C arrays in src/vk/shaders/spv/shaders_spv.h.
#
# The generated header is committed, so building the game (including the APK)
# never needs a shader compiler. Run this script after editing GLSL:
#
#   tools/shaders/build.sh
#   GLSLANG=/path/to/glslangValidator tools/shaders/build.sh
set -eu
cd "$(dirname "$0")/../.."

GLSLANG=${GLSLANG:-}
if [ -z "$GLSLANG" ]; then
    for candidate in glslangValidator glslang; do
        if command -v "$candidate" >/dev/null 2>&1; then GLSLANG=$(command -v "$candidate"); break; fi
    done
fi

if [ -z "$GLSLANG" ] || [ ! -d src/vk/shaders ]; then
    echo "note: glslangValidator not found; keeping the committed SPIR-V" >&2
    exit 0
fi

mkdir -p src/vk/shaders/spv
for src in src/vk/shaders/*.vert src/vk/shaders/*.frag; do
    [ -e "$src" ] || continue
    out="src/vk/shaders/spv/$(basename "$src").spv"
    "$GLSLANG" -V --target-env vulkan1.0 -o "$out" "$src"
    echo "compiled $src -> $out"
done

python3 tools/shaders/embed.py src/vk/shaders/spv src/vk/shaders/spv/shaders_spv.h
echo "wrote src/vk/shaders/spv/shaders_spv.h"
