# Enjoer + DimScript

Enjoer is a small Vulkan **2D** game host, and **DimScript** is the language for
2D games it runs. A game is a folder of `.ds` files plus a `game.manifest`;
there is no 3D in the language and no 3D scene behind the game — what the game
draws each frame is the whole picture.

There are two DimScript implementations and they are kept identical on purpose:

| | what it is | where |
|---|---|---|
| **reference interpreter** | same AST, same frame: used by tools, tests and `--run` | `dimscript/interpreter.py` |
| **AOT compiler** | the same syntax lowered to plain C99 with manual memory — this is what ships | `dimscript/compiler.py`, `tools/dimscriptc.py` |

There is no VM and no refcount: strictly the compiler, the C runtime and the
renderer. Everything is drawn through the same triangle batch that the Vulkan
pipeline consumes: shapes from `render.*` become vertices in
`src/enjoer_draw.c`, and a game that looks right in the host preview looks
right on the device.

## The language

```dimscript
-- games/brick/main.ds
struct Paddle {
    x: float
    y: float
    width: float
    height: float
}

struct Ball {
    x: float
    y: float
    vx: float
    vy: float
    radius: float
    stuck: bool
}

struct BrickGame {
    blocks: list
    lives: int
    score: int
    state: int
    paddle: Paddle
    ball: Ball
}

game = new BrickGame
target_x = 0.0

-- Every number in this game is a design unit of a 640x360 screen, times ui():
-- a field 360 px tall is a strip across the middle of a phone unless it scales,
-- and so is its text.
ui(): float {
    return math.max(0.6, math.min(4.0,
        math.min(engine.width(), engine.height()) / 360.0))
}

require "blocks"
require "hud"

load() {
    game.blocks = build_blocks(11)
    game.lives = 3
    game.score = 0
    game.state = 0
    center_paddle()
    launch_ball()
    render.clear(0.05, 0.08, 0.15)
}

resized(width: float, height: float) {
    center_paddle()
}

update(dt: float) {
    follow_pointer(dt)
    if game.state == 1 then {
        step_ball(game, dt)
    }
}

touchpressed(id, touch_x, touch_y) {
    target_x = touch_x
    if game.state == 0 then {
        game.state = 1
        game.ball.stuck = false
        game.ball.vy = -330.0
    }
}

draw() {
    render.color(0.86, 0.95, 1.0)
    render.rect(game.paddle.x - game.paddle.width / 2.0, game.paddle.y,
                game.paddle.width, game.paddle.height)
    render.color(1.0, 0.78, 0.32)
    render.circle(game.ball.x, game.ball.y, game.ball.radius)
    u = ui()
    render.text("Счёт: " .. game.score, 20.0 * u, 48.0 * u, 1.0 * u)
    draw_hud()
}

quit() {
    delete game
}
```

### Syntax

* `struct Name { field: type ... }`, `new Name`, `delete value`.
* Global declarations are top-level `name = expression`.
* Functions are `name(param: type, ...) : return_type { ... }`; untyped
  parameters are allowed and default to `float` (`id`-like names to `int`).
* Blocks are `{ ... }` closed by `}`. `if`/`while`/`for` may drop the braces and
  use `then`/`do`; `end` is accepted as an alias for `}`.
* `if ... then`, `else`, `else if`.
* `while condition do`, `for i = first, last do` (inclusive) and
  `for i = first, last, step do`, `break`, `continue`.
* `local scratch = 0` declares a variable even when a global has the same name.
* `require "module"` loads `module.ds` from the same game folder; files are
  linked in dependency order, so `build_blocks` defined in `blocks.ds` is
  callable from `main.ds`.
* Comments start with `--`. Statements are newline separated; an operator may
  end a line but never start one, so a wrapped expression must keep its `+`,
  `and`, `..` etc. at the end of the previous line.
* `&gt;`/`&lt;` HTML entities are unescaped by the lexer, so a snippet pasted
  from a web page still parses.

### Types and operators

| type | notes |
|---|---|
| `int` | 64-bit signed |
| `float` | `1.0`, `2.5e3`; a literal without a dot is an `int` |
| `string` | UTF-8, immutable, `..` concatenates |
| `bool` | `true`/`false`; `and`/`or` short-circuit and yield a `bool` |
| `nil` | unset struct field, or a deleted object |
| `list` | growable array of any values |

`+ - *` keep the integer type when both sides are integers; `/` always produces
a `float`; `%` is a floored remainder. Division or remainder by zero is a script
error, not `inf`. `==`/`~=` compare numbers and strings, and compare objects by
identity. `+` on strings is an error on purpose — use `..`. String comparison
with `<`/`>` is lexicographic.

Lists: `x = []`, `x = [1, 2, 3]`, `x.push(v)`, `x.insert(i, v)`, `x.delete(i)`,
`x.clear()`, `x.index_of(v)`, `x.join(", ")`, `x.count`, `x[i]` (negative
indices count from the end), and `x[i] = v`.

### Builtins

* `render.clear(r, g, b)`, `render.color(r, g, b)`, `render.color_alpha(r, g, b, a)`
* `render.rect(x, y, w, h)`, `render.frame(x, y, w, h, thickness)`,
  `render.circle(x, y, radius)`, `render.ring(x, y, radius, thickness)`,
  `render.line(x0, y0, x1, y1, thickness)`, `render.tri(x0, y0, x1, y1, x2, y2)`
* `render.text(text, x, y, scale)` — coordinates are pixels from the top left.
  The call is recorded in the frame together with the current font (see
  `render.font`); `src/ds_ttf.c` then rasterizes the glyphs on the CPU
  (scanline fill, nonzero winding, 3x3 box antialiasing) into a per-font
  atlas, and the renderer draws them as tinted glyph quads — plain textured
  triangles, so Vulkan and the software fallback show the same pixels.
  Scale contract: 1.0 is a 16 px em box, so body text on phones wants 2.0 and
  up, and a game that scales its layout by the screen scales its text with it
  (see "Screen, orientation and text size").  A glyph is baked at the em it is
  drawn at, so a small label is its own crisp raster and never a shrunk copy of
  a big one; its box is rounded to whole pixels and its uv inset by half a
  texel, so a wall of text keeps a steady baseline and no glyph bleeds into its
  neighbour.  Texts with the default face (`-1`, or a broken face) stay recorded
  but undrawn — a game that wants pixels must load a font first.  Growing an
  atlas is a pixel change to one layer of the texture array, not a new texture
  set (see `src/ds_image.h`), so the tap that first writes a `9` into a score
  costs one small upload instead of a rebuild.
* `render.image(handle, x, y, w, h)`,
  `render.image_region(handle, x, y, w, h, u0, v0, u1, v1)` — sprites from
  `image.load`, tinted by the current `render.color`.
* `render.font(handle)` — the face for the following `render.text` calls;
  `-1` is the default face.
* `image.load("sprites.png")`, `image.width(h)`, `image.height(h)`,
  `image.count()` — PNG sprite sheets, decoded once, sampled by the renderer
  as a texture array. PNG is the only format: JPEG, GIF, BMP, WebP and TIFF
  are refused at every layer — the manifest parser rejects non-`.png` names,
  the packer checks the magic bytes, and the loader names the format
  (`photo.jpg: only PNG images are supported (JPEG data)`) and returns `-1`.
  A missing or broken PNG is `-1` too, never a crash.
* `font.load("font.ttf")`, `font.count()` — game fonts (`.ttf`/`.otf`).
  A missing file or a non-font is handle `-1`, not a crash; a file with valid
  magic but broken tables keeps its handle while its texts stay undrawn.
  Loading the same file twice returns the first handle. Both shipped games
  bundle DejaVu Sans Bold — see "Bundled font" below.
* `math.floor/ceil/round/abs/sign/sqrt/sin/cos/tan/min/max/mod/pow/lerp/random`,
  `math.pi`, `math.e`
* `engine.width()`, `engine.height()`, `engine.time()`, `engine.delta()`,
  `engine.fps()`, `engine.frame()`, `engine.quit()`
* `input.touches()`, `input.touch_x(i)`, `input.touch_y(i)`, `input.touch_down(i)`,
  `input.key("a")`
* `print(...)`, `log(...)`, `str(v)`, `len(v)`, `num(text)`

### Engine callbacks

A game implements any subset of: `load()`, `resized(width, height)`,
`touchpressed(id, touch_x, touch_y)`, `touchmoved(...)`, `touchreleased(...)`,
`keypressed(name)`, `keyreleased(name)`, `update(dt)`, `draw()`, `quit()`.
They run in order per frame: input callbacks first, then `update`, then `draw`.
A script error reports the file, line and column, and aborts. There is no
per-frame statement budget: a mistaken
`while true do` in a game really hangs, so mind your loops.

### Screen, orientation and text size

A game draws in screen pixels and nothing else: `engine.width()` /
`engine.height()` are the truth every frame, `resized(width, height)` says they
changed, and touches arrive in the same units — so a layout written against them
cannot disagree with the window.

`orientation` in the manifest is what the *window* asks the OS for
(`sensorLandscape`, …).  A phone held upside down keeps its window size while
its panel wants the picture turned the other way, so the Vulkan renderer
pre-rotates what the game draws and says so with `preTransform`: whichever hand
the player holds a landscape phone in, the game lands upright instead of
sideways, and a rotation is never mistaken for a resize.  The mapping is
`src/surface_transform.h` — header-only, so a host test can check the matrix
without Vulkan.  A driver that reports a rotation it will not take on a
swapchain is answered with identity instead, letting the compositor turn the
picture: that costs one blit a frame, where asking the driver for a transform it
does not accept costs a swapchain that never builds.

The rule for a game's own numbers follows from that: never hardcode a position
for one window size.  Pick a design size (both bundled games use a 360 px short
side), scale by `min(width, height)`, and use the same factor for `render.text`
— a fixed `1.0` is a 16 px glyph, a fine size for a preview window and a blur on
a phone.  And a text stays where it is: an animation that changes a text's scale
or nudges its origin is what a player reads as a shaking screen, so the bundled
games answer a tap with rings and borders instead.

## Game folder and manifest

```text
games/brick/
├── game.manifest
├── main.ds      -- structs, globals, the engine callbacks
├── blocks.ds    -- level building and physics
├── hud.ds       -- everything on screen
├── font.ttf     -- DejaVu Sans Bold (see "Bundled font")
└── FONT-LICENSE.txt
```

(`games/clicker/` has the same shape with a single `main.ds`.)

`game.manifest` is a flat `key = value` file with `--` comments:

```ini
title = "Кирпич"
author = "Enjoer"
package = "com.cb4.brick"
version = "1.0"
version_code = 2
orientation = "sensorLandscape"
target_fps = 60
resizeable = true
clear_color = 0.05 0.08 0.15
scripts = ["main.ds", "blocks.ds", "hud.ds"]
```

`clear_color` also accepts `"#rrggbb"`. Any `icon`, `icons` or `icon_*` key is
**rejected with an error**: launcher icons are not supported yet, and a silently
ignored key is worse than a build that refuses to lie about it. The same rules
are implemented twice — `src/ds_manifest.c` (device) and `dimscript/manifest.py`
(host tooling) — and both produce the same defaults.

```ini
images = ["sprites.png", "ui/coin.png"]
fonts = ["font.ttf"]
```

Image and font files live next to the scripts (subfolders allowed) and are
staged into the APK by `tools/gamepack.py`, so `image.load`/`font.load` find
them on the device. A listed file must exist and be a real PNG / TrueType /
OpenType file — the packer checks the magic bytes on the host.

Files that the manifest does not list are still staged, after the listed ones,
so a forgotten entry is not a silent no-op on disk. An Android build only sees
what is packaged, which is why `tools/gamepack.py` writes the complete lists
into the packaged copy of the manifest.

### Bundled font

`games/clicker/font.ttf` and `games/brick/font.ttf` are both **DejaVu Sans
Bold** — the same file in each folder, because every game folder packs on its
own and fonts travel with the game, never by symlink. It was picked for full
Cyrillic coverage (the clicker's text is Russian) and bold legibility at
small sizes. DejaVu's changes are public domain; the Bitstream Vera base
allows bundling in commercial builds as long as the notice travels with it —
that is what `FONT-LICENSE.txt` next to each copy is for. To swap the font,
drop another `.ttf`/`.otf` into the game folder and list it under
`fonts = [...]` in `game.manifest`; `font.load` resolves it from the staged
assets on the device exactly like on the host.

### Packing an APK's data

```sh
python3 tools/gamepack.py games/brick --staging staging
python3 tools/gamepack.py games/brick --check        # validate, write nothing
python3 tools/gamepack.py games/brick --print-manifest
# when aapt packs the folder, keep the manifest outside of it:
python3 tools/gamepack.py games/brick --staging staging --manifest-out apk-manifest/AndroidManifest.xml
```

`staging/` then holds `assets/game/*.ds`, `assets/game/game.manifest`, the
staged images and fonts (subfolders preserved), and a generated
`AndroidManifest.xml` (package, label, orientation, versions,
`resizeableActivity`) is derived from the game manifest, so the two never
disagree. `--manifest-out` writes that file elsewhere, which is what the APK
step does — `aapt` walks the folder it is given and would otherwise try to store
a second `AndroidManifest.xml` as a plain file. `CMakeLists.txt` runs the same
tool when `ENJOER_GAME_DIR` is set (the variable is a `PATH` cache entry, so
CMake has already made a relative path absolute), but only when a game file
changed.

## Building

Everything is built with **clang at `-O3`** — the NDK *is* clang, so host and
device see the same compiler and optimisation level. `tools/toolchain.sh` picks
`clang`, falls back to `python3 -m ziglang cc`, and only then to `gcc`.

```sh
tools/preview/build.sh          # -> ./preview
sh tools/tests/run.sh           # frontend + AOT + gamepack + string temps + engine
ASAN=1 sh tools/tests/run.sh    # same tests under -fsanitize=address,undefined
```

### APK (fast path)

```sh
# Needs ANDROID_SDK_ROOT (+ NDK), a JDK and python3. No Gradle: gamepack +
# CMake + aapt2/d8/apksigner directly, skipping every step whose outputs are
# already fresh, so a no-change rebuild finishes in seconds.
tools/apk/build.sh                                 # games/clicker, arm64-v8a
tools/apk/build.sh --game games/brick --abi all    # + armeabi-v7a
tools/apk/build.sh --install --launch             # install and start on device
```

Install `ninja` and `ccache` for the full speed: the script prefers Ninja and
CMake wraps the compiler in ccache automatically. The manual equivalent of the
native step is:

```sh
# Android (Vulkan is the default and needs no external SDK)
cmake -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_BUILD_TYPE=Release -DENJOER_GAME_DIR=games/clicker
cmake --build build -j
```

On a device the game folder comes from the APK's `assets/game`; the host reads
the directory given by `--game` (or `$ENJOER_GAME`).

### Preview

```sh
sh tools/preview/build.sh
./preview --port 8090 --w 960 --h 540 --game games/clicker
```

The server binds `0.0.0.0`, uses relative URLs, and renders on demand: each
`/frame.jpg` advances one frame, so a backgrounded tab costs nothing.
`GET /info` reports the title, the mode, the renderer, the fonts the game
loaded, the last script error and a debug mirror of the last frame's
`render.text` calls (each names its font handle). The pixels themselves are
real rasterized glyphs drawn by the same C renderer the APK runs — no
overlay, no browser text.

The game logic is always the ahead-of-time compiled clicker; `--game`
(`ENJOER_GAME`) only picks the asset folder — manifest, fonts, images — and
defaults to `games/clicker`, so a fresh checkout is never a blank window.

## Ahead-of-time compilation

```sh
python3 tools/dimscriptc.py games/clicker --check
python3 tools/dimscriptc.py games/clicker --emit-c \
  -o src/generated/clicker.c --header src/generated/clicker.h
python3 tools/dimscriptc.py games/brick --run --frames 3     # whole folder
python3 tools/dimscriptc.py games/brick --check
```

The C backend lowers a program to plain C99 against `src/dimscript_runtime.h`:
structs become C structs, `render.*` becomes the same batch calls the reference
interpreter records,
and the engine callbacks become `dimscript_load`, `dimscript_update`, … Wrappers
are emitted for every callback, so a game that implements only `update` still
links.

The backend covers the whole language — lists, structs, strings, methods —
lowered to manual memory with no refcount. The ownership rules fit in one
paragraph: strings are immutable values (storing one into a variable, field
or slot replaces the old value; `return` and list push/insert/set hand over a
duplicate), fresh strings consumed in place live in statement temps (`__ds_tN`,
declared above the call, released below it), lists and structs are identity
(stored raw, freed by their owner), and function parameters are borrowed.
`src/generated/clicker.c` is the checked-in output for `games/clicker` —
regenerate it with the command above after any compiler or game change — and
is what the linked game runs. `tools/tests/temps.py` tortures every one of
these paths under ASan+UBSan.

## Tests

```sh
sh tools/tests/run.sh
```

1. `tools/tests/dimscript.py` — lexer, parser, interpreter, type checker and the
   generated C of the example, compared against the checked-in file.
2. `tools/tests/aot.py` — `examples/shapes.ds` compiled to C99 and linked
   against the runtime (`for`/`while`/`break`/`local`,
   `math.`/`engine.`/`input.`, every render primitive): 1071 vertices.
3. `tools/tests/pack.py` — `gamepack.py` staging images + fonts (listed and
   discovered, subfolders preserved), the failure paths, and an AOT game that
   loads the staged PNG and font through `image.load`/`font.load`.
4. `tools/tests/temps.py` — the string-ownership torture test: one script
   nesting every fresh-string path (concats, converts, compares, stores,
   returns, calls, loop conditions, list elements) is compiled to C99 and run
   under ASan+UBSan, with all 13 recorded texts asserted exact.
5. `tools/tests/engine.c` — the engine boundary: manifest parsing (including
   the removed `cube` key failing loudly), the 2D batch → rasterizer,
   input reaching a script, resize, image/font registries, and the compiled
   fallback.  It also checks the screen rotation on its own — window size to
   framebuffer extent and the projection matrix, for all four rotations — the
   text pass (glyph quads on whole pixels, one atlas cell per size drawn) and
   the two counters of the image registry (a layer whose pixels moved vs a new
   set of images).

`ASAN=1 sh tools/tests/run.sh` runs all five under
`-fsanitize=address,undefined`. The object cache is mode-sensitive: switching
between plain and `ASAN=1` runs needs `ENJOER_CLEAN=1` first.

## Layout

```text
dimscript/                Python front-end: lexer, AST, parser, checker,
                          reference interpreter, C backend, manifest, project
examples/shapes.ds        loops + builtins + shapes: the AOT fixture
games/clicker/            the shipped clicker: manifest + main.ds + font
games/brick/              a real multi-file game: manifest + 3 .ds files + font
tools/dimscriptc.py       compiler/interpreter CLI
tools/gamepack.py         packs scripts, images and fonts, generates AndroidManifest.xml
tools/apk/build.sh        fast no-Gradle APK build (gamepack + NDK + aapt2/d8/apksigner)
tools/toolchain.sh        clang -O3 selection, shared by every build script
src/ds_manifest.*         game.manifest reader used at startup
src/ds_files.*            game folder access: disk on host, assets on Android
src/ds_image.*            PNG registry behind image.load, its dirty layers and
                          the generation a renderer rebuilds on
src/ds_png.c              dependency-free PNG decoder
src/ds_font.*             font registry behind font.load
src/ds_ttf.*              TrueType text pass: glyph rasterizer + per-font atlas
src/enjoer_draw.*         the triangle batch both renderers consume
src/dimscript_runtime.*   native ABI the generated C links against
src/surface_transform.h   screen rotation: framebuffer size and projection,
                          header-only so host tests can reach it
src/generated/            C + header generated from games/clicker
src/engine.h              C game API and Buffer type
src/game.c                lifecycle, input, manifest, asset folder
src/renderer.h            C ABI of the 2D renderer
src/vulkan_2d.cpp         Vulkan 2D renderer (device) and software fallback (host)
src/shaders/              GLSL sources and the generated SPIR-V header
src/main.c                Android NativeActivity loop, asset manager wiring
game/                     Android Activity and the fallback manifest
tools/preview/            HTTP frame/input preview (glyphs in /frame.jpg, mirror in /info)
tools/tests/              engine (rotation, text pass, images), AOT, gamepack,
                          string-temps and frontend tests
tools/ci/annotate.py      turns a build log into check annotations (CI helper)
```
