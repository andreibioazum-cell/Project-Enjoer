# Enjoer + DimScript

Enjoer is a small Vulkan game host, and **DimScript** is the game language it
runs. A game is a folder of `.ds` files plus a `game.manifest`; the engine opens
that folder and interprets it, so a game changes without recompiling anything.

There are two DimScript implementations and they are kept identical on purpose:

| | what it is | where |
|---|---|---|
| **VM** | the interpreter that ships: arena + mark/sweep GC, lists, full language | `src/ds_vm.c`, `src/ds_vm_lang.c`, `src/ds_vm_exec.c`, `src/ds_vm_heap.c` |
| **reference interpreter** | same AST, same frame, no GC: used by tools, tests and `--run` | `dimscript/interpreter.py` |
| **AOT compiler** | the same syntax lowered to plain C99 (smaller subset, see below) | `dimscript/compiler.py`, `tools/dimscriptc.py` |

`tools/tests/parity.py` runs the brick game on both interpreters and diffs every
vertex and every text command, so the two cannot drift apart silently.

Everything is drawn through the same triangle batch that the Vulkan pipeline
consumes: shapes from `render.*` become vertices in `src/enjoer_draw.c`, and a
game that looks right in the browser preview looks right on the device.

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
    render.text("Счёт: " .. game.score, 20.0, 48.0, 1.0)
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
| `list` | growable array of any values (interpreter only) |

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
  **There is no font backend yet**: the call is recorded in the frame and shown
  by the preview as real browser text, and a future text pass will read the same
  record. Nothing rasterizes glyphs in C or Vulkan today.
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
A script error stops the game loop for that callback, reports the file, line and
column, and the preview shows it in a banner instead of a black screen.

The VM also enforces a per-frame statement budget and a recursion limit, so a
mistaken `while true do` in a game fails with a readable error instead of
freezing the device.

## Game folder and manifest

```text
games/brick/
├── game.manifest
├── main.ds      -- structs, globals, the engine callbacks
├── blocks.ds    -- level building and physics
└── hud.ds       -- everything on screen
```

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
**rejected with an error**: icons are not supported yet, and a silently ignored
key is worse than a build that refuses to lie about it. The same rules are
implemented twice — `src/ds_manifest.c` (device) and `dimscript/manifest.py`
(host tooling) — and both produce the same defaults.

Files that the manifest does not list are still loaded, after the listed ones,
so a forgotten entry is not a silent no-op on disk. An Android build only sees
what is packaged, which is why `tools/gamepack.py` writes the complete list into
the packaged copy of the manifest.

### Packing an APK's data

```sh
python3 tools/gamepack.py games/brick --staging staging
python3 tools/gamepack.py games/brick --check        # validate, write nothing
python3 tools/gamepack.py games/brick --print-manifest
# when aapt packs the folder, keep the manifest outside of it:
python3 tools/gamepack.py games/brick --staging staging --manifest-out apk-manifest/AndroidManifest.xml
```

`staging/` then holds `assets/game/*.ds` and `assets/game/game.manifest`, and a
generated `AndroidManifest.xml` (package, label, orientation, versions,
`resizeableActivity`) is derived from the game manifest, so the two never
disagree. `--manifest-out` writes that file elsewhere, which is what the APK
step does — `aapt` walks the folder it is given and would otherwise try to store
a second `AndroidManifest.xml` as a plain file. `CMakeLists.txt` runs the same
tool when `ENJOER_GAME_DIR` is set (the variable is a `PATH` cache entry, so
CMake has already made a relative path absolute).

## Building

Everything is built with **clang at `-O3`** — the NDK *is* clang, so host and
device see the same compiler and optimisation level. `tools/toolchain.sh` picks
`clang`, falls back to `python3 -m ziglang cc`, and only then to `gcc`.

```sh
tools/preview/build.sh          # -> ./preview
sh tools/tests/run.sh           # python + VM + parity + engine/renderer tests
ASAN=1 sh tools/tests/run.sh    # same tests under -fsanitize=address,undefined
```

```sh
# Android (Vulkan is the default and needs no external SDK)
cmake -B build \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 \
  -DCMAKE_BUILD_TYPE=Release -DENJOER_GAME_DIR=games/brick
cmake --build build -j
```

On a device the game folder comes from the APK's `assets/game`; the host reads
the directory given by `--game` (or `$ENJOER_GAME`).

### Preview

```sh
./preview --port 8090 --w 960 --h 540 --game games/brick
```

The server binds `0.0.0.0`, uses relative URLs, and renders on demand: each
`/frame.jpg` advances one frame, so a backgrounded tab costs nothing.
`GET /info` reports the title, the mode (`interpreted`/`compiled`), the
renderer, the last script error and the recorded `render.text` commands, which
the page overlays in the browser font — the font-less engine and a readable
debug view at the same time.

Without a `game.manifest` in the folder, the preview runs the ahead-of-time
compiled `examples/clicker.ds` and shows the cube instead, so a fresh checkout
is never a blank window.

## Ahead-of-time compilation

```sh
python3 tools/dimscriptc.py examples/clicker.ds --check
python3 tools/dimscriptc.py examples/clicker.ds --emit-c \
  -o src/generated/clicker.c --header src/generated/clicker.h
python3 tools/dimscriptc.py games/brick --run --frames 3     # whole folder
python3 tools/dimscriptc.py games/brick --check
```

The C backend lowers a program to plain C99 against `src/dimscript_runtime.h`:
structs become C structs, `render.*` becomes the same batch calls the VM makes,
and the engine callbacks become `dimscript_load`, `dimscript_update`, … Wrappers
are emitted for every callback, so a game that implements only `update` still
links.

`list`, indexing and list methods exist in the interpreter only. Compiling a
game that uses them stops with an explicit message pointing at `src/ds_vm.c`
rather than emitting half a file — which is exactly the case for `games/brick`.
`src/generated/clicker.c` is the checked-in output of the list-free example and
is what the fallback path runs.

## Tests

```sh
sh tools/tests/run.sh
```

1. `tools/tests/dimscript.py` — lexer, parser, interpreter, type checker and the
   generated C of the example, compared against the checked-in file.
2. `tools/tests/vm.c` — the native VM: multi-file linking and `require` order,
   lists, loops, builtins, the render batch, GC staying under a heap bound, and
   five error paths (unknown name, bad callback arity, missing file, division by
   zero, statement budget).
3. `tools/tests/parity.py` — the same brick game on both interpreters, vertex by
   vertex (1068 vertices, three texts).
4. `tools/tests/aot.py` — `examples/shapes.ds` compiled to C *and* interpreted,
   linked as two binaries whose dumps are diffed (`for`/`while`/`break`/`local`,
   `math.`/`engine.`/`input.`, every render primitive): identical, 1071 vertices.
5. `tools/tests/cube.c` — the engine boundary: manifest → VM → batch →
   rasterizer, input reaching a script, resize, and the compiled fallback.

`ASAN=1 sh tools/tests/run.sh` runs all five under
`-fsanitize=address,undefined`, which is the check the GC and the arena get.

## Layout

```text
dimscript/                Python front-end: lexer, AST, parser, checker,
                          reference interpreter, C backend, manifest, project
examples/clicker.ds       the original example, also the AOT source
examples/shapes.ds        loops + builtins + shapes: the AOT/VM parity fixture
games/brick/              a real multi-file game: manifest + 3 .ds files
tools/dimscriptc.py       compiler/interpreter CLI
tools/gamepack.py         packs a game folder and generates AndroidManifest.xml
tools/toolchain.sh        clang -O3 selection, shared by every build script
src/ds_vm.*               the shipping interpreter (VM, GC, lists, builtins)
src/ds_manifest.*         game.manifest reader used at startup
src/ds_files.*            game folder access: disk on host, assets on Android
src/enjoer_draw.*         the triangle batch both renderers consume
src/dimscript_runtime.*   native ABI shared by the VM and generated C
src/generated/            C + header generated from examples/clicker.ds
src/engine.h              C game API and Buffer type
src/cube_game.c           lifecycle, input, manifest, interpreter-vs-AOT choice
src/vulkan_cube.*         Vulkan renderer (device) and software fallback (host)
src/shaders/              GLSL sources and the generated SPIR-V header
src/main.c                Android NativeActivity loop, asset manager wiring
game/                     Android Activity and the fallback manifest
tools/preview/            HTTP frame/input preview with the text overlay
tools/tests/              native, VM, parity and AOT regression tests
tools/ci/annotate.py      turns a build log into check annotations (CI helper)
```
