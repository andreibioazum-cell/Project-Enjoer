# Enjoer — a cube on Vulkan

Enjoer is a deliberately small 3D cube playground. The old voxel world,
Minecraft-style menu, terrain generator, block editing and asset pipeline are
gone.

The game is split at a narrow C ABI boundary:

- **C** (`src/cube_game.c`) owns the lifecycle, elapsed time, input, orbit
  state and the Android event loop (`src/main.c`).
- **C++** (`src/vulkan_cube.cpp`) owns the renderer. Its production path
  creates a Vulkan instance, Android surface, logical device, swapchain,
  depth buffer, render pass, graphics pipeline and vertex buffer, and pushes
  the MVP matrix through push constants. The shaders live in `src/shaders/`
  as GLSL and are embedded as SPIR-V (`src/shaders/cube_spv.h`).

A dependency-free C++ raster fallback is kept only for the local HTTP preview
and for checking the C/C++ boundary on a host without a GPU. It renders the
same six-face cube; it is not the Android renderer.

## Preview

The preview is a native process, not a JavaScript renderer. It sends the
native frame to a small browser canvas and forwards keyboard/pointer events
back to the C game layer.

```sh
tools/preview/build.sh
./preview --port 8090
# open http://localhost:8090
```

The server binds to `0.0.0.0`, and the page uses relative URLs, so the live
preview also works behind an external HTTPS proxy.

Controls:

- drag with the mouse or a finger to orbit the cube;
- `W A S D` or arrow keys adjust the view;
- `Space` reverses the automatic spin;
- `R` resets the cube.

## Tests

```sh
tools/tests/run.sh
ASAN=1 tools/tests/run.sh
```

The regression test exercises the C game, C++ renderer, pointer/key input,
resize handling and the rendered frame checksum.

## Android + Vulkan

Vulkan ships with the Android NDK (headers and `libvulkan.so`), so there is
no external GPU dependency to download. A normal Android build is the Vulkan
build:

```sh
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_NDK="$ANDROID_NDK_ROOT" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`ENJOER_USE_VULKAN` defaults to `ON`. Passing `-DENJOER_USE_VULKAN=OFF`
builds the same Android activity with the small software fallback drawn
through `ANativeWindow_lock`, which is useful for smoke testing on emulators
without a Vulkan driver.

The native activity presents directly to an `ANativeWindow` through
`VK_KHR_android_surface` and a FIFO swapchain. The manifest requires Vulkan
1.0.3 hardware.

### Shaders

`src/shaders/cube.vert` and `src/shaders/cube.frag` are the source of truth.
After editing them, regenerate the embedded SPIR-V with
`glslangValidator` from the Vulkan SDK or the `glslang-tools` package:

```sh
tools/shaders/compile.sh
```

## Layout

```text
src/engine.h            C public API and Buffer type
src/cube_game.c         C game state and controls
src/vulkan_cube.h       C ABI to the renderer
src/vulkan_cube.cpp     C++ Vulkan renderer + local fallback
src/shaders/            GLSL sources and generated SPIR-V header
src/main.c              Android NativeActivity loop
game/                   Android manifest and Activity
tools/preview/          HTTP frame/input preview
tools/shaders/          SPIR-V regeneration script
tools/tests/            C/C++ regression test
```
