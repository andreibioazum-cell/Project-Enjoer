# Enjoer — Cubic Battle 4

Enjoer is a small Vulkan 2D game host and DimScript runtime. This repository
ships **one game only: Cubic Battle 4**. Its source, assets, manifest and
Android host live under `game/`; there is no second game folder or selectable
fallback game.

The same game is used in every target:

- the reference interpreter runs `game/` for frontend tests and inspection;
- the AOT compiler emits `src/generated/cubicbattle.c` and
  `src/generated/cubicbattle.h`;
- the native host links that generated C code;
- the preview and APK pack the exact same `game/` directory.

There is no VM or 3D scene. DimScript draws the complete 2D frame through the
same triangle batch used by the software preview and Vulkan renderer.

## Repository layout

```text
game/
├── game.manifest          # Cubic Battle 4 package metadata
├── main.ds                # lifecycle, lobby, warning and dispatch
├── config.ds              # palette, shared state and constants
├── tuning.ds              # game tuning and asset handles
├── battle*.ds             # solo battle and input logic
├── menu.ds, classes.ds    # lobby, shop, classes and progression
├── ui.ds, battledraw.ds   # UI and drawing helpers
├── *.png, font.ttf        # Cubic Battle 4 assets
├── AndroidManifest.xml    # fallback host manifest
└── java/                  # Android NativeActivity wrapper

dimscript/                # lexer, parser, interpreter and C99 AOT compiler
src/generated/             # checked-in AOT output for game/
src/                       # native runtime, renderer and Android lifecycle
tools/gamepack.py          # validates and stages game/ into assets/game/
tools/preview/             # native HTTP preview
tools/apk/                 # direct no-Gradle APK build
tools/tests/               # frontend, AOT, packer and renderer tests
```

`game.manifest` is the source of truth for the title, package, orientation,
script list, images and font. The packer generates the Android manifest from
it, so the package and the runtime cannot drift apart.

## Cubic Battle 4

The game opens with a safety warning, then provides the Cubic Battle lobby,
solo battle, classes, levels, achievements, battle pass, shop and settings.
Online buttons are intentionally shown as unavailable because the current
DimScript runtime has no network API. All UI and battle modules are part of
this one game and are linked from the manifest in load order.

The manifest currently defines:

```ini
title = "Cubic Battle 4"
package = "com.cb4.cubicbattle"
version = "1.0"
version_code = 1
orientation = "sensorLandscape"
target_fps = 60
resizeable = true
```

## Build and test

The host toolchain prefers clang with `-O3`, then Zig's clang-compatible
compiler, then gcc. The Android NDK is clang as well, so the AOT output is
compiled with the same language and optimisation model on both targets.

```sh
# Validate the only game without writing files
python3 tools/gamepack.py game --check
python3 tools/dimscriptc.py game --check

# Regenerate the checked-in native game when game/*.ds changes
python3 tools/dimscriptc.py game --emit-c \
  -o src/generated/cubicbattle.c \
  --header src/generated/cubicbattle.h

# Build and run all host regression tests
ENJOER_CLEAN=1 sh tools/tests/run.sh

# Build the native HTTP preview
sh tools/preview/build.sh
./preview --port 8090 --w 960 --h 540 --game game
```

The preview serves `/info` and `/frame.jpg`. It reports `Cubic Battle 4`,
uses the AOT game, and renders text and sprites in native code rather than
putting a browser overlay over the frame.

For an APK, provide an Android SDK, NDK and JDK:

```sh
tools/apk/build.sh
# The optional argument is the same sole game directory:
tools/apk/build.sh --game game --abi all
```

The APK pipeline validates and stages `game/`, builds `libds_game.so` from the
checked-in Cubic Battle AOT output, compiles the Android activity, and packages
the generated manifest and assets.

## DimScript essentials

A game folder contains a flat manifest and `.ds` modules. `require "module"`
loads another module from the same folder. The supported callbacks are
`load`, `resized`, `touchpressed`, `touchmoved`, `touchreleased`, `keypressed`,
`keyreleased`, `update`, `draw` and `quit`.

The language provides structs, lists, strings, booleans, integer and floating
point arithmetic, and the following native groups:

- `render.clear`, `color`, `rect`, `frame`, `circle`, `ring`, `line`, `tri`,
  `text`, `image` and `font`;
- `image.load` and `font.load` for the PNG and TrueType assets in `game/`;
- `engine.width`, `height`, `time`, `delta`, `fps`, `frame` and `quit`;
- `input.touches`, touch coordinates, touch state and keyboard state;
- `math` helpers plus `print`, `log`, `str`, `len` and `num`.

The interpreter and AOT compiler use the same AST and callback order. Native
code owns the frame batch and asset registries; no alternate game runtime is
selected at startup.

## Pull request policy

Changes to this repository are made on the session branch, then opened as a
pull request against `main`. The branch must pass `tools/tests/run.sh` and the
single `game/` folder must remain the only shipped game source.
