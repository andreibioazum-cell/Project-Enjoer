# Enjoer + DimScript

This repository contains the small Vulkan game host and **DimScript**, a
compact game scripting language. DimScript source is written with the syntax
from `examples/clicker.ds`; the Python frontend parses and type-checks it, then
emits ordinary C99. Python is not part of the running game loop: after
compilation the callbacks are native C and can be built with the Android NDK.

The current example is intentionally font-less. `render.text(...)` is part of
the stable API and is recorded by the runtime, but the Vulkan renderer does
not rasterize glyphs yet. This keeps the language/runtime boundary ready for a
font atlas without pretending that a font already exists.

## DimScript

A minimal program looks like this:

```dimscript
struct ClickerGame {
    score: int
    text_scale: float
}

game = new ClickerGame

load() {
    game.score = 0
    game.text_scale = 1.0
}

touchpressed(id, touch_x, touch_y) {
    game.score = game.score + 1
    game.text_scale = 1.6
}

update(dt: float) {
    if game.text_scale > 1.0 then
        game.text_scale = game.text_scale - (4.0 * dt)
    }
}

draw() {
    render.color(1.0, 1.0, 1.0)
    render.text("Счет: " .. game.score, 50.0, 150.0, game.text_scale)
}

quit() {
    delete game
}
```

Supported in this first version:

- `struct` fields with `int`, `float`, `string` and `bool` types;
- `new`/`delete`, member access with `.`, assignments and local variables;
- arithmetic, comparisons, boolean operators and string concatenation with
  `..`;
- `if ... then` blocks closed by `}` (an explicit `{ ... }` after `then` is
  accepted too), optional `else`, comments beginning with `--`;
- lifecycle callbacks `load`, `touchpressed`, `update`, `draw` and `quit`;
- `render.color(r, g, b)` and `render.text(text, x, y, scale)`.

The compiler also accepts `&gt;`/`&lt;` copied from HTML, which is useful when
pasting the original example.

### Compile or interpret a script

No third-party Python packages are needed:

```sh
# Static parse/type check
python3 tools/dimscriptc.py examples/clicker.ds --check

# Emit C99 and the callback header
python3 tools/dimscriptc.py examples/clicker.ds --emit-c \
  -o /tmp/clicker.c --header /tmp/clicker.h

# Or use the module entry point
python3 -m dimscript examples/clicker.ds -o /tmp/clicker.c

# Development/reference interpreter; text commands are shown as JSON
python3 tools/dimscriptc.py examples/clicker.ds --run --click 2 --frames 2
```

The generated file includes `dimscript_runtime.h`. A standalone native build
therefore only needs the generated C, `src/dimscript_runtime.c`, and
`-Isrc`; the language runtime itself has no Python dependency.

The tracked `src/generated/clicker.c` and `src/generated/clicker.h` are
produced from `examples/clicker.ds` and are compiled into the Android game.
After changing the example, regenerate them with the command above.

## Native Vulkan game

The game is split at a narrow C ABI boundary:

- **C** (`src/cube_game.c`) owns lifecycle, time, input and calls the generated
  DimScript callbacks.
- **DimScript-generated C** (`src/generated/clicker.c`) owns the clicker state
  and game callback logic.
- **C++** (`src/vulkan_cube.cpp`) owns the renderer. Its production path
  creates a Vulkan instance, Android surface, logical device, swapchain, depth
  buffer, render pass, graphics pipeline and vertex buffer, and pushes the MVP
  matrix through push constants. The shaders live in `src/shaders/` as GLSL
  and are embedded as SPIR-V (`src/shaders/cube_spv.h`).
- **C runtime** (`src/dimscript_runtime.c`) supplies allocation, fast native
  values, string concatenation and the font-less render ABI.

The preview has a dependency-free C++ raster fallback because a host checkout
usually has no Android `ANativeWindow`. Android builds use Vulkan when
`ENJOER_USE_VULKAN=ON` (the default).

### Preview

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
- `R` resets the cube;
- a touch-down also enters the DimScript `touchpressed` callback.

### Android + Vulkan

Vulkan ships with the Android NDK (headers and `libvulkan.so`), so there is no
external GPU dependency to download. A normal Android build is:

```sh
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-29 \
  -DANDROID_NDK="$ANDROID_NDK_ROOT" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

`ENJOER_USE_VULKAN` defaults to `ON`. Passing
`-DENJOER_USE_VULKAN=OFF` builds the same Android activity with the small
software fallback drawn through `ANativeWindow_lock`, useful for smoke testing
on emulators without a Vulkan driver.

### Shaders

`src/shaders/cube.vert` and `src/shaders/cube.frag` are the source of truth.
After editing them, regenerate the embedded SPIR-V with `glslangValidator`:

```sh
tools/shaders/compile.sh
```

## Tests

```sh
tools/tests/run.sh
ASAN=1 tools/tests/run.sh
```

The regression test covers the DimScript lexer/parser/interpreter/C emitter,
the C game layer, C++ renderer, pointer/key input, resize handling and the
rendered frame checksum. The C test also verifies that three text draw calls
reach the runtime even though the font backend is intentionally disabled.

## Layout

```text
dimscript/               Python lexer, AST, parser, interpreter and C compiler
examples/clicker.ds      DimScript source of the native example
tools/dimscriptc.py       compiler CLI
src/dimscript_runtime.*   native runtime ABI (no font implementation yet)
src/generated/            C/header generated from the example
src/engine.h              C public game API and Buffer type
src/cube_game.c           lifecycle/input + DimScript callback host
src/vulkan_cube.*         C ABI and C++ Vulkan renderer + local fallback
src/shaders/              GLSL sources and generated SPIR-V header
src/main.c                Android NativeActivity loop
game/                     Android manifest and Activity
tools/preview/            HTTP frame/input preview
tools/shaders/            SPIR-V regeneration script
tools/tests/              native and DimScript regression tests
```
