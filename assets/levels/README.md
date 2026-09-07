# Platformium level files

Drop a text file here (for example `extra1.txt`) and the APK ships a new
platformer — no C changes. On startup the engine loads `bonus.txt` and then
`extra1.txt` … `extra8.txt` in order; each valid file becomes one more
world in the launcher's level chips. Files named `README.md` are never
packaged.

## Format

```
name River Run      # world title shown in the HUD and toasts
theme grass         # grass | stone | sand (palette + background)
time 200            # countdown in seconds (30..9999, default 240)
<map rows>
```

Header lines may appear in any order before the map. Every line that is not
a header is one map row; one character per tile, top row first. Rows may be
different lengths (short rows are padded with empty tiles), the map is at
most 256 tiles wide and 64 rows high. A level is rejected unless it has a
player start `P` and a goal `G`.

## Tile legend

| Char | Tile |
| --- | --- |
| `#` | solid block (textured per theme) |
| `=` | one-way platform: jump through from below, land on top |
| `^` | spikes; only the lower half of the tile hurts |
| `~` | lava / water hazard |
| `o` | coin (+50) |
| `S` | spring; walk onto it for a ~6-tile launch |
| `C` | checkpoint; respawns after death |
| `G` | goal flag; touch it to clear the world (time bonus = seconds × 5) |
| `P` | player start (first one wins) |
| `W` | patrolling walker enemy; stomp it from above (+100) |
| `F` | flying enemy that bobs vertically around its tile |
| `M` | horizontal moving lift (±3 tiles) that carries the player |
| `V` | vertical lift (±2.5 tiles) |
| `.` or space | empty |

## Physics budget (design rules of thumb)

- Run speed 7 tiles/s, jump height ≈ 2.6 tiles, spring ≈ 6 tiles.
- A full-speed running jump clears a 3-tile gap; keep gaps ≤ 3.
- Steps of one tile are trivially walk-jumped; keep required steps ≤ 1.
- Walkers patrol ±0.8 tiles around their spawn at 1.2 tiles/s; give them
  flat ground with a tile of clearance on both sides.
- Springs replace a surface tile (embed them in the floor, not on top of
  it) and launch whoever walks over them — leave ~8 tiles of safe landing
  room in the running direction.
- Anything below the map is void death in the sky theme, so floating
  islands must be reachable by the chains above.

`tools/tests/platformer.c` boots every shipped level and steers a simple
bot (hold right, jump at gaps, walls, hazards and walkers) to the flag; if
your level defeats that bot, it is probably too hard for thumbs too.
