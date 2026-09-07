# Enjoer — a small C99 3D engine

Enjoer is a general-purpose 3D engine in **C99**: projects with text scene
files, typed nodes, physics and C scripts, rendered by a software rasterizer
with a streamed voxel-world backend. There is no hard-coded game behind it
anymore — the app *is* the engine. At startup a launcher lists every engine
project it finds; picking one loads its scene and runs it. The same binary
runs on Android (APK) and in the PC/browser preview.

Three ready projects ship in `projects/`:

- **Demo Scene** (`projects/demo`) — meshes, a sun and a lamp, an orbiting
  camera and two C scripts (`spin`, `orbit`).
- **Physics Playground** (`projects/bounce`) — a `RigidBody3D` ball you push
  around `StaticBody3D` colliders with WASD/arrows; Space jumps, a pointer
  press pops it. Driven by the `roller` script through the engine input API.
- **Voxel World** (`projects/voxel_world`) — the streamed block terrain as a
  `VoxelWorld3D` node seen from a scene camera.

Everything in the app — launcher, labels, error screens — is English.

## Repository layout

```
assets/        runtime assets: textures/*.png, fonts/*.ttf, sounds/*.wav
game/          Android packaging: AndroidManifest.xml and the Java activity
projects/      engine projects (project.eng + scenes/*.escn + scripts/*.c)
src/           all C sources
  main.c       Android entry point (native activity)
  engine.h     compact platform/asset/immediate-HUD/lifecycle API
  core/        app state, error handling, logging, asset reads,
               and game.c — the engine launcher/router every platform calls
  graphics/    2D renderer: primitives, textures, text (+ ttf/)
  sound/       audio: PCM16 mixer and AudioTrack output
  engine/      the engine: projects, scenes, nodes, physics, input, scripting
    render/    the 3D/voxel render backend (rasterizer, materials, terrain,
               streamed world, skylight, water)
third_party/   stb_image / stb_image_write
tools/         preview server, regression tests, offline art tool, scaffolds
stage_assets.py  stages assets/ and projects/ into the APK asset tree
```

`stage_assets.py` also writes `projects/index.txt` (one project per line)
because the Android asset manager cannot enumerate subdirectories; the engine
reads that index inside the APK and simply scans the folder on the host.

## Quick start (PC / browser)

```sh
tools/preview/build.sh
./preview --port 8090
# open http://localhost:8090 — the launcher appears; tap a project
```

`./preview --project projects/demo` skips the launcher and starts one project
directly; `--projects <dir>` points the launcher at another project root.
Default frame is **960×540**; `--w`, `--h`, `--port`, `--assets`, `--storage`
are available. The server listens on `0.0.0.0` and the page uses relative
URLs, so it works through an external HTTPS proxy.

## Controls

The launcher: tap/click a card to run that project. The round button in the
top-left corner — or `Esc` / `M` — opens it again at any time.

While a project runs:

| Input | Effect |
| --- | --- |
| `W A S D` / arrows | move (scripts that poll `ENG_KEY_*`, or the free camera) |
| drag / move pointer | handed to scripts (`eng_input_pointer`), or orbits the free camera |
| `Space` / `Shift` | up / down for the free camera |

Scenes whose current `Camera3D` carries **no** script get the engine's free
camera automatically: dragging orbits it, `W A S D` translates it in the view
plane, `Space`/`Shift` change altitude. A scripted camera (as in `demo` and
`bounce`) keeps full control and is never touched by the free camera.

On a phone: touch a card to pick a project; one finger drags the camera; the
on-screen round button, `Esc` or `M` open the launcher.

## Projects

A project is a directory with a `project.eng` manifest:

```
name = Demo Scene
main_scene = scenes/main.escn
```

`tools/project/create.sh <dir>` scaffolds a fresh project (manifest, a scene
with an unscripted camera and a spinning-cube C script). Any project runs
with `./preview --project <dir>` and shows up in the launcher when it lives
under the scanned root (default `projects/`, or the APK asset folder of the
same name). The launcher lists projects in folder-name order.

## Scene files

`.escn` is plain text, one section per node:

```
[node name="Hero" type="MeshInstance3D" parent="."]
mesh = cube
position = 0 2 0
yaw = 1.57
script = "spin"

[node name="Ground" type="StaticBody3D" parent="."]
shape = box
size = 40
position = 0 -20 0

[node name="Ball" type="RigidBody3D" parent="."]
mesh = sphere
color = 0.95 0.62 0.18
mass = 1
gravity_scale = 1
velocity = 0 0 0
position = 0 3 0
script = "roller"
```

`parent` is a path from the root (`.` = root). Values are floats, vectors,
quoted strings or `true`/`false`; `#` starts a comment. Node types:

| Type | Purpose |
| --- | --- |
| `Node` | plain container (scene root) |
| `Node3D` | positioned container: position, yaw/pitch/roll, uniform scale |
| `MeshInstance3D` | cube / plane / sphere / cylinder primitives |
| `Camera3D` | viewpoint; `current = true` activates it, `fov` in degrees |
| `DirectionalLight3D` | sun-like light along the node's −Z |
| `OmniLight3D` | point light with `range` falloff |
| `VoxelWorld3D` | the streamed block terrain (one per scene) |
| `StaticBody3D` | a fixed collider; `shape = box\|sphere`, `size` = full edge |
| `RigidBody3D` | a dynamic sphere collider (gravity, impulses, sleeping) |

Transforms are hierarchical Euler angles (`Ry*Rx*Rz`, parent scale composed
into children). `MeshInstance3D` — and a body that carries a `mesh` — take
either a flat `color = r g b` (a palette material built at runtime) or
`material = grass|dirt|stone|sand|water|log|leaves` to reuse the voxel PNG
tiles. `VoxelWorld3D` accepts `seed = N`. Lights tint and shade every mesh
face; with no lights at all meshes render fully lit.

## C scripting

A script is ordinary C with two optional entry points, both receiving the
node the script is attached to:

```c
#include "eng_api.h"
void eng_script_ready(EngNode *self);             /* once, after load */
void eng_script_process(EngNode *self, float dt); /* every frame */
```

On the host the engine compiles the project's `.c` to a shared object when it
is newer than the `.so` and loads it with `dlopen` (the binary exports the
API via `-rdynamic`; scripts build with `cc -shared -fPIC -Isrc/engine`).
Compiled `.so` files are Git-ignored.

A phone has neither compiler nor writable `.so`, so the scripts that ship
with the sample projects (`spin`, `orbit`, `roller`) also exist **built into
the engine** (`src/engine/eng_script_builtin.c`) and are used whenever the
compiled version is unavailable. A scene that names an unknown script on a
device logs a warning and runs without it.

`src/engine/eng_api.h` is the whole scripting surface: create/attach/free
nodes, find them by path, get/set position, rotation and scale, switch the
current camera, set light energy/colour/range, choose a mesh primitive,
colour and size, the body API (`eng_body_set_shape`, `eng_body_set_mass`,
`eng_body_apply_impulse`, `eng_body_set/get_linear_velocity`,
`eng_body_is_grounded`), the input API (`eng_input_is_pressed`,
`eng_input_pointer`), `eng_time()`/`eng_delta()`/`eng_print()` and the voxel
cell API (`eng_voxel_get_cell` / `eng_voxel_set_cell` — read and write single
half-block cells of a `VoxelWorld3D` with the material names above).

Input is a polled state: the platform glue feeds keys and the pointer into
`eng_input_feed_key` / `eng_input_feed_pointer`; scripts and the free camera
read it once per frame. The browser page never calls `localhost` — it posts
events to the preview server, which feeds the same API.

## Physics

`StaticBody3D` nodes are fixed colliders: `box` (the default, an
axis-aligned box whose half-extent is `size`/2) or `sphere`. A `RigidBody3D`
is a dynamic **sphere** whose radius matches its drawn sphere mesh
(`size`/2), so the physics body and the ball you see always coincide. Every
frame the engine applies gravity, integrates velocity (fixed substeps of at
most 1/30 s), resolves penetration against the static bodies, and puts a
still, grounded body to sleep. Bodies move in world space, so a `RigidBody3D`
must hang from the root or a plain `Node` container (no moving transform
parent). Rigid/rigid contact is intentionally left out so the solver stays
predictable. Body nodes also take `mass`, `gravity_scale`, `velocity`,
`restitution` and `friction` keys.

## The render backend

The engine draws through a software 3D rasterizer with the voxel world as one
of its backends (`src/engine/render/`):

- `assets/textures/` holds nine **original 32×32 RGBA PNGs**; a quarter of a
  face receives the matching 16×16 pixels, UVs are bound to the world grid
  (including negative coordinates), merged faces repeat the PNG once per
  source block. `python3 tools/art/make_assets.py` regenerates the PNGs and
  the PCM16 effects offline.
- **Diffuse skylight** instead of binary sun/shadow: open columns get
  daylight, sideways spread decays, sealed spaces stay dark, leaves transmit
  partially, water casts no black shadow. Four light values per quad
  interpolate perspectively with `1/z`; greedy meshing only merges faces with
  constant light, so gradients never stretch.
- Terrain: two soft value-noise octaves, 3D-noise **caves** under a sealed
  crust, bedrock bottom layers, lake beds never carved.
- **Transparent water**: opaque geometry first, then ~55 % water blending; a
  blocky source/stream simulation (down first, then lateral with support, no
  uphill or diagonal flow) steps at 0.1 s with a bounded active-cell queue.
- Chunk cache of **11×11** chunks (visible 9×9 plus a prefetch
  ring), nearest-first meshing with a soft per-frame budget, fog retreating
  as chunks become ready. Fog on dark surfaces stays dark.
- Perspective `1/z` depth/UV, signed colour arithmetic, backface culling
  before frustum tests; a bilinear upscale with an exact 2× fast path. The
  internal resolution adapts from measured render time with hysteresis
  (starts at a ~2.2 MP budget); the top-right of the Android HUD and the
  launcher status line show the measured FPS.

## Tests

```sh
tools/tests/run.sh
SANITIZE=1 tools/tests/run.sh
```

Suites: the rasterizer/material regressions (`render`), voxel generation,
streaming and mesh coverage (`world`), the water simulation and saves
(`water`), the PCM mixer (`audio`), the engine layer (`engine`: node-tree
math, scene parsing, C scripting via `dlopen`, physics — 9 groups), and the
app (`launcher`: the project list, tap-to-run, script animation, input
routing, launcher pausing, project switching, the free camera, reset and
`--project`-style direct open) at two resolutions. The Android-flavoured
code paths (APK asset IO, no `dlopen`, native-activity glue) are compiled
with `-D__ANDROID__` against a minimal NDK header stub so they cannot rot.
The sanitizer build enables ASan, UBSan and float-cast-overflow.

The host suite drives the same `game_*` entry points the platforms call, so
the launcher, input routing and pausing are covered without a browser.

## Android

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_NDK=$ANDROID_NDK_ROOT
cmake --build build -j
python3 stage_assets.py assets staging/assets   # also stages projects/ + index.txt
```

The CI workflow (`.github/workflows/main.yml`) builds both ABIs, stages
assets/projects and packages `EnjoerMessenger.apk` (the legacy library name
`libds_game.so` and the manifest are kept for the existing pipeline). Inside
the APK the launcher enumerates projects from the staged `projects/index.txt`
and reads manifests/scenes through the asset manager; the sample projects run
with the built-in scripts. Targets are `arm64-v8a` and `armeabi-v7a`,
Android 10+.

Every `.c` is its own translation unit; `.c` files are never pulled in via
`#include`. External libraries live in `third_party/`.
