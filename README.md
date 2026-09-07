# Enjoer — block world

A landscape first-person game in **C99**: procedural terrain with generated
caves, trees, transparent water, walking, jumping, toggleable flight and
half-cube building. The 3D world module is called **Geometrium**
(`src/geometrium/`). Everything in the game — HUD, labels, error screens —
is English.

## Repository layout

```
assets/        runtime assets: textures/*.png, fonts/*.ttf, sounds/*.wav
game/          Android packaging: AndroidManifest.xml and the Java activity
projects/      engine projects: demo scene and the voxel world as a project
src/           all C sources
  main.c       Android entry point (native activity)
  engine.h     compact platform/asset/HUD/lifecycle API
  core/        app state, error handling, logging, asset reads
  graphics/    2D renderer: primitives, textures, text (+ ttf/)
  sound/       audio: PCM16 mixer and AudioTrack output
  geometrium/  the 3D playset: render, terrain, water, player, input, HUD
  engine/      engine layer: node tree, scenes, projects, C scripting
third_party/   stb_image / stb_image_write
tools/         preview server, regression tests, offline art tool, scaffolds
```

Sounds live with the other assets in `assets/sounds/` (not in a game
folder); `stage_assets.py` stages the whole `assets/` tree into the APK.

## Controls

### Phone

- Black joystick on the left — movement. Transparent backing, knob without an
  outline, thick black outer ring.
- Swipe on the right — camera. A short tap of this area breaks the selected
  part; swiping or holding the camera alone breaks nothing.
- **"Break" / "Place"** — text-only round buttons under the white crosshair.
  Holding a button repeats the action; a short press is processed even
  between frames.
- Six slots at the bottom select the material: grass, dirt, stone, sand,
  wood, leaves.
- Gravity and collisions are on by default; **"Jump"** sits on the right.
- The white **"Flight"** button at the top toggles flight. In flight the jump
  button hides and the joystick moves along the view, including up/down.
  Release to hover; turn flight off to fall again.
- Stick, camera, jump and building support independent fingers. Cancelling a
  gesture neither presses toggles nor leaves actions held.

Android uses `sensorLandscape` — both landscape orientations. The top-right
corner shows the **measured FPS**, averaged over about half a second. The
counter uses the real frame interval, not the clamped physics step.

### PC / browser

| Key / action | Binding |
| --- | --- |
| `W A S D` | Move |
| Drag on the right / arrows | Camera |
| Short left click on the right / `E` | Break part |
| Right mouse button / `R` | Place part |
| `1`–`6` / panel slots | Material |
| `Space` | Jump; swim up in water |
| `F` | Toggle flight |
| `Space` / `Shift` in flight | Up / down |

Right mouse works while dragging with the left one. Losing focus and window
resizes reset all holds.

## Eight parts and PNGs

One source block **1×1×1** consists of **2×2×2 parts of size 0.5×0.5×0.5**.
The camera ray selects exactly one part up to six source blocks away.
Placement goes into the neighbouring half cell; occupied space and player
intersection are forbidden. The bottom world layer is protected from
destruction; the build height is 64 source blocks. Water does not block the
ray that picks solid blocks.

`assets/textures/` contains nine **original 32×32 RGBA PNGs**. The game reads
these files instead of generating color patterns at startup. One texture
still covers one source block: a quarter of a face receives the matching
**16×16 pixels**, not a downscaled copy of the whole image. UVs are bound to
the world grid, including negative coordinates. Merged faces repeat the PNG
once per source block, never stretching it. A horizontal cut inside a grass
block shows dirt.

The optional offline authoring tool is `python3 tools/art/make_assets.py`.
It reproduces the PNGs and the short PCM16 jump/break/place effects. Sound
plays on Android; the browser preview is silent.

## Terrain: gentle hills and caves

- Heights come from two soft value-noise octaves; the slope between
  neighbouring columns stays well below one block, so the ground climbs in
  short single steps instead of whole-block walls.
- **Generated caves** ("holes") are carved by 3D noise inside the stone: a
  sealed four-block crust keeps the surface intact, the two bottom layers
  stay bedrock, and lake beds are never carved, so natural water cannot
  drain into the underground.
- A small safe clearing surrounds the spawn point.

## Soft lighting

- **Harsh sun and contact shadows are removed, lighting is kept.** Face
  volume reads through a gentle brightness difference, not black patches
  under blocks.
- Instead of binary sun/shadow the engine uses **diffuse skylight**. Open
  columns get daylight; sideways spread decays gradually. Walls and ceilings
  hold light back, so sealed mines are truly dark while entrances get a
  smooth transition. A 0.5×0.5 opening counts.
- Leaves transmit light partially. No sharp brightness jumps under canopies;
  water is transparent to light and casts no false black shadow.
- Light is baked on the half grid **only when opacity changes**. The chunk
  cache stores the needed height range between solid ground and sky; a
  shared scratch halo buffer is reused. No per-frame ray tracing.
- Four light values per quad interpolate perspectively, together with `1/z`.
  Greedy meshing merges faces only along directions where the light is
  constant, so gradients never stretch and no seam appears. Opaque neighbour
  cells are not averaged into black AO. Brightness and fog palettes are
  cached.
- Fog on dark surfaces is dark too: a far mine wall does not glow with the
  daytime sky colour. A small minimum brightness keeps outlines readable; it
  is not an artificial light source in the mine.

## First-person hand

- The arm is **one solid rectangular cuboid** running from the lower-right
  screen corner to the held block — a single box, not stacked flat slabs.
  The held block is a second closed cuboid with six offset faces.
- The whole model is built in camera space with orthonormal axes: view
  rotation neither spins the block separately nor distorts it. Textures are
  chosen by **local** face: grass tops stay on top at any camera rotation.
- The hand draws after the world with the same rasterizer; only its screen
  region of the z-buffer is cleared. It does not poke through walls, while
  its own parts overlap correctly.
- Swing, material change and walk bob are preserved. Bob fades out smoothly
  when stopping and is absent in flight. The whole hand's light follows the
  brightness at the player's eyes smoothly, with no separate harsh patches
  under trees and no fog on the held block.

## Transparent water

- Water renders **see-through**: all opaque geometry draws first, then water
  faces blend over the framebuffer (~55% water colour), so lake beds and
  pool walls show through. Waterfall and spreading behaviour are unchanged.
- Natural water is a source. Breaking a floor or wall part next to water
  **fills the freed half cell** instead of leaving a dry hole.
- The simulation steps at 0.1 s: down first, then lateral faces with support.
  Water never passes through blocks, never jumps diagonally and never flows
  uphill. Lateral flow reaches up to seven steps (0.5 block each); a new
  lateral stream starts at the foot of a waterfall. This is a **blocky
  source/stream simulation**, not a continuous fluid or volume-conserving
  one.
- Covering a source/channel removes the unsupported stream; reopening the
  channel restarts it. Changing the floor under a source updates neighbouring
  water as well.
- Only a queue of active cells near the player is processed, up to 512 per
  step. The queue is bounded and de-duplicated; overflow has a gradual
  recovery with its own small budget. Calm water performs no world scans.
  Spreading remeshes affected chunks **without recomputing light**.
- Sources, lateral flow and falling water are distinguished in the save.
  After loading, a stream does not become an infinite source; simulation
  resumes when returning to an evicted area.

## World, saves and performance

- Generation stores source blocks in **16×16×64** chunks. Splitting into
  eight parts **does not multiply the whole world in memory by eight**: only
  modified source blocks are stored separately, eight materials per record.
- The cache is limited to **11×11 chunks**: the visible 9×9 area plus one
  prefetch ring. Seed — `GEOMETRIUM_WORLD_SEED` in
  `src/geometrium/geometrium_internal.h`.
- View distance is one chunk shorter than before: fog to **64 source
  blocks**, far plane 80. The initial near area is prepared immediately; the
  far area completes gradually. Fog retreats as chunks become ready and
  never exposes unmeshed edges.
- Generation/meshing are serviced nearest-first: at most two jobs per frame
  with a soft 2.5 ms budget, no synchronous row rebuilds while moving.
- Exposed uniform faces merge with **greedy meshing**. Mixed parts and
  contacts with partially broken neighbours are handled exactly. After an
  edit only affected chunks and the necessary boundary neighbours rebuild.
- PNG mip levels and lighting palettes are cached. The detail level stays
  local even on large merged planes; fog is per pixel distance, not one
  colour per chunk.
- Perspective **1/z** for depth/UV and signed colour arithmetic are kept.
  The bilinear Q8 upscale reuses horizontal rows with the same rounding and
  no overflow; the exact 2× fast path uses 3:1 weights via adds/shifts.
  Back faces are culled before the frustum test; fully visible polygons skip
  extra clipping. The HUD draws at screen resolution.
- 3D starts with a ~2.2 MP budget and adapts the internal scale from render
  time with hysteresis. Preview network latency never controls Android
  quality.
- The shorter view distance and lighter terrain generator cut real work:
  the regression scenario now renders **121 chunks / 26 218 quads** instead
  of 169 / 40 414, and the 180-frame host run dropped from ~6.2 ms to
  ~5.5 ms per frame at 960×540 (11.0 → 10.3 ms at 2400×1080).
- Edits survive chunk eviction and restarts. The `world.edits` file is saved
  periodically and on minimize/close: temp file + atomic rename, version,
  seed, length and checksum validation. On Android this is the app-private
  folder; in the preview it is the Git-ignored `data/` folder. The limit is
  65 536 affected source blocks, including water changes; the player
  position is not saved.
- Save format is **EJVOX02**: eight materials and eight flow states per
  record. Old **EJVOX01 files are read automatically**; the next save writes
  the new version. An old APK cannot read the new format, so do not open
  this world in an old build after updating.

## Preview

```sh
tools/preview/build.sh
./preview --port 8090 --assets assets
# Open http://localhost:8090; run from the repository root.
```

With `--project <dir>` the preview runs the engine instead of the built-in
game and loads that project's scene tree, e.g.
`./preview --project projects/demo`.

Default **960×540**; `--w`, `--h`, `--port`, `--storage` are available. The
server listens on `0.0.0.0`; the browser uses relative URLs and works through
an external HTTPS proxy. The frame is letterboxed, not stretched. Transport
is JPEG with at most one frame ahead instead of multi-megabyte RGBA per
request; uncompressed `/frame.rgba` remains for diagnostics. Events keep
their order and finger IDs; redundant moves coalesce.

## Tests

```sh
tools/tests/run.sh
SANITIZE=1 tools/tests/run.sh
```

39 check groups: original PNGs/palettes/mips, quarter UVs, smooth
perspective light and fog, depth/clipping, bit-exact upscale/stride,
walk/flight/multitouch/FPS, exact mesh coverage, dark mines and half
openings, soft leaves and chunk borders, the closed two-box hand and its
pixel stability across 48 yaw/pitch combinations, water
fill/spread/cover/save, water queue overflow, edits/collisions/hash indices,
old-save migration, the PCM mixer and full frames at 960×540 and 2400×1080.
The sanitizer build enables ASan, UBSan and float-cast-overflow.

Diagnostic frames for the hand, the mine and the water can be written to a
folder:

```sh
mkdir -p build-tests/visual
ENJOER_TEST_IMAGES=build-tests/visual tools/tests/run.sh
# Result: PPM frames in the ignored build-tests/visual folder.
```

`tools/tests/browser.cjs` is an extra check of real Chromium through
Playwright: four fingers, cancel/blur, FPS, breaking and placing one part,
save contents, the material panel, mouse button combinations and window
sizes. Run it on a **separate test world**, never on the user's current game:

```sh
mkdir -p build-tests/half-voxel/browser-data
# A fresh empty folder for every independent run.
./preview --port 8091 --storage build-tests/half-voxel/browser-data
# In another terminal, with Playwright and Chromium installed:
NODE_PATH="$PWD/build-tests/node_modules" node tools/tests/browser.cjs
```

`TEST_URL`, `TEST_OUTPUT`, `TEST_SAVE`, `CHROMIUM_EXECUTABLE_PATH` and
`CHROMIUM_ARGUMENTS` (a JSON array) are supported for unusual browser
locations.

## Sources and Android

`engine.h` is the compact platform/asset/immediate-HUD/lifecycle API.
`src/core/`, `src/graphics/`, `src/sound/` and `src/geometrium/` hold the
separate C modules. Old APIs/state, DimScript arrays and strings, the text
IME/JNI layer, the draw command queue, the old sprite loader and unneeded
scene objects are gone. The Java activity only handles fullscreen.

Every `.c` is its own translation unit; lighting and water are split into
`geometrium_light.c` and `geometrium_water.c`. `.c` files are never pulled
in via `#include`. External libraries live in `third_party/`.

Android builds with the existing CMake/NDK flow and `stage_assets.py`; the
latter copies the whole `assets/` tree (PNGs, font and WAVs) and compiles the
Java activity from `game/java` into `classes.dex` when an SDK is present. The
CI workflow still calls `stage_assets.py game/assets staging/assets`; the
script accepts that legacy path as an alias for `assets/`. Targets are
`arm64-v8a` and `armeabi-v7a`, Android 10+. The service library name
`libds_game.so` and the matching manifest field are kept for compatibility
with the current CI packaging; it is only a library name, not an interpreter.
The existing workflow and APK artifact names were not renamed either.

## Engine

The block world doubles as a small general-purpose 3D engine
(`src/engine/`). The classic game stays the built-in default; the engine
adds **projects**, **typed scene nodes** and **C scripting** on top of the
same rasterizer, materials and terrain code.

### Projects

A project is a directory with a `project.eng` manifest:

```
name = Demo Scene
main_scene = scenes/main.escn
```

`tools/project/create.sh <dir>` scaffolds a fresh project (manifest, a scene
and a spinning-cube C script). Run any project with
`./preview --project <dir>`. Two ready projects ship in `projects/`:
`demo` (meshes, lights, an orbiting camera and two scripts) and
`voxel_world` (the streamed terrain seen through a scene camera).

### Node types

Scenes are trees of typed nodes, Godot-style:

| Type | Purpose |
| --- | --- |
| `Node` | plain container (scene root) |
| `Node3D` | positioned container: position, yaw/pitch/roll, uniform scale |
| `MeshInstance3D` | cube / plane / sphere / cylinder primitives |
| `Camera3D` | viewpoint; `current = true` activates it, `fov` in degrees |
| `DirectionalLight3D` | sun-like light along the node's −Z |
| `OmniLight3D` | point light with `range` falloff |
| `VoxelWorld3D` | the classic streamed block terrain |

Transforms are hierarchical Euler angles (`Ry*Rx*Rz`, parent scale composed
into children). `MeshInstance3D` takes either a flat `color = r g b`
(a palette material built at runtime) or `material = grass|dirt|stone|sand|
water|log|leaves` to reuse the voxel PNG tiles. Lights tint and shade every
mesh face; with no lights at all meshes render fully lit.

### Scene files

`.escn` is plain text, one section per node:

```
[node name="Hero" type="MeshInstance3D" parent="."]
mesh = cube
position = 0 2 0
yaw = 1.57
script = "spin"
```

`parent` is a path from the root (`.` = root). Values are floats, vectors,
quoted strings or `true`/`false`; `#` starts a comment.

### C scripting

A script is ordinary C compiled to a shared object and loaded with
`dlopen`; the host rebuilds the `.so` automatically when the source is
newer. Two optional entry points, both receiving the node the script is
attached to:

```c
#include "eng_api.h"
void eng_script_ready(EngNode *self);            /* once, after load */
void eng_script_process(EngNode *self, float dt);/* every frame */
```

`src/engine/eng_api.h` is the whole scripting surface: create/attach/free
nodes, find them by path, get/set position, rotation and scale, switch the
current camera, set light energy/colour/range, choose a mesh primitive,
colour and size, plus `eng_time()`/`eng_delta()` and `eng_print()`. The host
exports the symbols (`-rdynamic`), scripts compile with
`cc -shared -fPIC -Isrc/engine`. Compiled `.so` files are Git-ignored.

The engine currently runs in the PC preview; the Android target still ships
the classic game only. Regression coverage lives in `tools/tests/engine.c`.
