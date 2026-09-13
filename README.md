# Enjoer — a cube on Dawn

Enjoer is now a deliberately small 3D cube playground. The old voxel world,
Minecraft-style menu, terrain generator, block editing and asset pipeline are
gone.

The game is split at a narrow C ABI boundary:

- **C** (`src/cube_game.c`) owns the lifecycle, elapsed time, input, orbit
  state and the Android event loop (`src/main.c`).
- **C++** (`src/dawn_cube.cpp`) owns the renderer. Its production path creates
  a WebGPU device, swapchain/surface, depth buffer, pipeline, vertex buffer,
  uniform buffer and WGSL shader through **Dawn**.

A dependency-free C++ raster fallback is kept only for the local HTTP preview
and for checking the C/C++ boundary before Dawn is downloaded. It renders the
same six-face cube; it is not the Android renderer when a Dawn build is
selected.

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

## Android + Dawn

Dawn is intentionally an external dependency because it is a full WebGPU
implementation and must be built for the same Android NDK/toolchain as the
application. The checkout does not hide a several-hundred-megabyte Dawn tree.

With a Dawn checkout and its CMake targets available:

```sh
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_NDK="$ANDROID_NDK_ROOT" \
  -DENJOER_USE_DAWN=ON \
  -DDAWN_ROOT=/path/to/dawn \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

If Dawn's target is not named `dawn_native`, pass it explicitly with
`-DENJOER_DAWN_TARGET=...`. `ENJOER_USE_DAWN` defaults to `ON`, so a normal
Android build is the Dawn build. Passing `-DENJOER_USE_DAWN=OFF` builds the
same Android activity with the small fallback, which is useful for smoke
building when Dawn is not available.

The native activity presents directly to an `ANativeWindow` through Dawn's
Android surface source. There is no software voxel renderer, texture pack,
world save, sound asset or JavaScript game logic left in the project.

## Layout

```text
src/engine.h       C public API and Buffer type
src/cube_game.c    C game state and controls
src/dawn_cube.h    C ABI to the renderer
src/dawn_cube.cpp  C++ Dawn/WebGPU renderer + local fallback
src/main.c         Android NativeActivity loop
game/              Android manifest and Activity
tools/preview/     HTTP frame/input preview
tools/tests/       C/C++ regression test
```
