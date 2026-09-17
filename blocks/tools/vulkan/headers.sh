#!/bin/sh
# Fetches the Khronos Vulkan-Headers for a desktop build (the Android NDK
# already ships its own copy). Nothing in the game links libvulkan; the headers
# are all a build needs, because the loader is dlopen()ed at runtime.
set -eu
cd "$(dirname "$0")/../.."
target=${1:-third_party/vulkan-headers}
if [ -d "$target/include/vulkan" ]; then
    echo "$target/include/vulkan already present" >&2
else
    git clone --depth 1 https://github.com/KhronosGroup/Vulkan-Headers.git "$target" >&2
fi
echo "VULKAN_HEADERS_DIR=$PWD/$target/include"
