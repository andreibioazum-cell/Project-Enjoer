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

The same C++ file also carries a dependency-free CPU rasterizer. It renders
the same six-face cube for the local HTTP preview and for checking the
C/C++ boundary on a host without a GPU, and it is the runtime safety net on
Android: if the device has no working Vulkan driver, or loses it mid-session,
the cube keeps spinning on the CPU instead of showing a black screen.

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
builds the same Android activity CPU-only, which is useful for smoke testing
on emulators without a Vulkan driver.

The native activity presents directly to an `ANativeWindow` through
`VK_KHR_android_surface` and a FIFO swapchain.

## When the device has no Vulkan

Vulkan is a preference now, not a requirement, so the app installs and runs
on phones without a driver:

- `cube_renderer_init` builds the Vulkan instance, surface, device and
  swapchain. If any step fails, the renderer logs which step it was
  (`adb logcat -s Enjoer`) and keeps going on the CPU.
- Vulkan can also die mid-session (device lost, surface lost, a driver that
  stops presenting). After ten failed frames in a row the renderer tears the
  GPU objects down and switches to the CPU path; the next `INIT_WINDOW`
  gives Vulkan another chance.
- The CPU path rasterizes the same cube into a small buffer (longest edge
  400 px, ~160k pixels) and stretches it over the window with a
  nearest-neighbour blit, which keeps a software-only phone interactive
  instead of rendering 2.5 M pixels per frame. It converts to RGB565 when a
  device only hands out a 16-bit window buffer, and paces itself at ~30 fps
  so it does not burn a full core.
- The manifest declares `android.hardware.vulkan.*` as `required="false"`,
  so the APK is no longer filtered out on devices without Vulkan.

`cube_renderer_backend()` reports which path is live, and
`cube_renderer_software_active()` tells the game layer directly.

### Forcing a backend

Useful to reproduce a friend's phone without owning it:

```sh
adb shell setprop debug.enjoer.renderer software   # always CPU
adb shell setprop debug.enjoer.renderer vulkan     # GPU only: fail loudly if unavailable
adb shell setprop debug.enjoer.renderer auto       # default: Vulkan, CPU when it fails
adb logcat -s Enjoer
```

`ENJOER_RENDERER=software|vulkan|auto` (environment variable) does the same
thing for the host preview and for `wrap.com.cb4` on a device.

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
src/vulkan_cube.cpp     C++ Vulkan renderer + CPU fallback
src/shaders/            GLSL sources and generated SPIR-V header
src/main.c              Android NativeActivity loop
game/                   Android manifest and Activity
tools/preview/          HTTP frame/input preview
tools/shaders/          SPIR-V regeneration script
tools/tests/            C/C++ regression test
```
