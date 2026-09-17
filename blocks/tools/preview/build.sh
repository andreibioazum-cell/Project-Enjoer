#!/bin/sh
# Builds the PC preview server from the same C sources as the APK.
#
#   tools/preview/build.sh            -> ./preview      (Vulkan when the Vulkan
#                                                       headers are present,
#                                                       else software only)
#                                     -> ./preview-sw   (always software)
#   WITH_VULKAN=0 tools/preview/build.sh   force the software build
#   VULKAN_HEADERS_DIR=/usr/include ...
#
# The loader is opened at runtime with dlopen("libvulkan.so.1"), so no libvulkan
# is needed to link: a build with headers compiles the Vulkan renderer and works
# on machines that have a driver. Chromium/Android ship the loader that way too.
#
# The preview binary picks a backend at runtime: Vulkan when a device is
# available, otherwise it logs the reason and falls back to the software
# rasterizer (see --render in tools/preview/host_main.c).
set -eu
cd "$(dirname "$0")/../.."
ROOT=$PWD

list() { # list <section>
    awk -v want="$1" '/^\[/ { on = ($0 == "[" want "]"); next } /^[[:space:]]*(#|$)/ { next } on { print }' sources.list
}

COMMON=$(list common)
SOFTWARE=$(list software)
VULKAN=$(list vulkan)
HOST="tools/preview/host_compat.c tools/preview/host_main.c tools/preview/frame_jpeg.c"

# Vulkan headers: system path, the NDK, or an override.
find_headers() {
    if [ -n "${VULKAN_HEADERS_DIR:-}" ] && [ -f "$VULKAN_HEADERS_DIR/vulkan/vulkan.h" ]; then
        echo "$VULKAN_HEADERS_DIR"; return 0
    fi
    for d in /usr/include /usr/local/include /opt/vulkan/include; do
        if [ -f "$d/vulkan/vulkan.h" ]; then echo "$d"; return 0; fi
    done
    return 1
}

CC=${CC:-gcc}
CFLAGS=${CFLAGS:--O2 -std=c99 -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Itools/preview/compat -Isrc -I.}

# Vulkan shaders must be compiled before the backend that embeds them.
if [ "${WITH_VULKAN:-1}" = "1" ]; then
    sh tools/shaders/build.sh || echo "warn: SPIR-V regeneration failed; using the committed shaders" >&2
fi

build_vulkan=0
if [ "${WITH_VULKAN:-1}" = "1" ]; then
    if HEADERS=$(find_headers); then
        build_vulkan=1
    else
        echo "note: Vulkan headers not found; building the software preview only" >&2
    fi
fi

if [ "$build_vulkan" = "1" ]; then
    # shellcheck disable=SC2086
    $CC $CFLAGS -DENJOER_VULKAN=1 -I"$HEADERS" \
        $COMMON $SOFTWARE $VULKAN $HOST \
        -lm -lpthread -ldl -o preview
    echo "built ./preview (Vulkan + software fallback, headers=$HEADERS)"
else
    # shellcheck disable=SC2086
    $CC $CFLAGS $COMMON $SOFTWARE $HOST -lm -lpthread -o preview
    echo "built ./preview (software renderer)"
fi

# The software build always exists so the fallback stays testable on a machine
# without any Vulkan driver at all.
# shellcheck disable=SC2086
$CC $CFLAGS $COMMON $SOFTWARE $HOST -lm -lpthread -o preview-sw
echo "built ./preview-sw (software renderer, always)"
