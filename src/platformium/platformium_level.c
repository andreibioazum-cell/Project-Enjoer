/* Level data: the plain-text map format, three built-in worlds and optional
 * asset levels (assets/levels/...), which is how new platformers ship
 * without recompiling. See assets/levels/README.md for the format.
 *
 *   name <title>      level title shown in the HUD
 *   theme grass|stone|sand
 *   time <seconds>    countdown timer (default 240)
 *   <map rows>        one character per tile, top row first:
 *     # solid         = one-way platform     ^ spikes        ~ lava
 *     o coin          S spring               C checkpoint    G goal flag
 *     P player start  W walking enemy        F flying enemy
 *     M horizontal lift                      V vertical lift
 *     . or space      empty
 * Header lines may appear in any order before the map; every line that is
 * not a header is a map row. */
#include "platformium_internal.h"
#include <stdio.h>

static PmLevelDef defs[PM_MAX_LEVELS];
static int def_count;
static PmLevelDef live;
static int live_valid;
static unsigned char taken[PM_MAX_W * PM_MAX_H]; /* coins/checkpoints used */

/* ── built-in worlds ─────────────────────────────────────────────────── */
/* Every gap is at most 3 tiles and every required step at most 1 tile, so
 * the levels stay fair on touch controls (and for the regression bot). */

static const char *level_green_hills[] = {
    "name Green Hills",
    "theme grass",
    "time 240",
    "................................................................................................................",
    "................................................................................................................",
    "................................................................................................................",
    "................................................................................................................",
    "................................................................................................................",
    "................................................................................................................",
    ".....................................o...........................................o..ooooo.......................",
    "....................................................................................=====.......................",
    ".....................................o.........ooo...............................o..............................",
    "..............................................=====.............................................................",
    "...........................oo........o...........................................o..............................",
    "............ooo............................................ooo.......ooo.............................ooo........",
    "...P..................W....##.......................C....W............W...................W.................G...",
    "################..################..#S###############..##########..##########..##S###############..#############",
    "################..################..#################..##########..##########..##################..#############",
    "################..################..#################..##########..##########..##################..#############",
    NULL
};

static const char *level_cave_depths[] = {
    "name Cave Depths",
    "theme stone",
    "time 300",
    "........................................................................................................................",
    "........................................................................................................................",
    "........................................................................................................................",
    "........................................................................................................................",
    "........................................................................................................................",
    "..................................................................................................ooo...................",
    "....................................................................................F.....o.............................",
    "...............................oo.............F.........................................................................",
    "..........................................................oo..............................o.............................",
    "...............................M..........................==.......................................V....................",
    "..........................................................................................o...................ooo.G.....",
    "............................................ooo........==........................ooo..............ooo.......############",
    "..P...............^^W........##..##......^^.....................C...##..##.##...W......^^...........W...################",
    "############...###########...##..##...############...############...##..##.##.############S##...########################",
    "############~~~###########~~~##~~##~~~############~~~############~~~##~~##~##~###############~~~########################",
    "############~~~###########~~~##~~##~~~############~~~############~~~##~~##~##~###############~~~########################",
    NULL
};

static const char *level_sky_ruins[] = {
    "name Sky Ruins",
    "theme sand",
    "time 300",
    "................................................................................................................................",
    "................................................................................................................................",
    "................................................................................................................................",
    "................................................................................................................................",
    "..........................................ooo.............F.......oooW..........................................................",
    "........................................########................########..........................................o.............",
    "........................................########====..ooo....===########................F..oo...................................",
    "........................................########....########....########.oo.......................................o.............",
    "........................................########....########....########====...............M....................................",
    "...............................W..ooo...............########..................ooW.................................o.............",
    "...........................###########S#............########................##########.====.====......^^....W........ooo...G....",
    "..........................##############....................................##########..........##################S#############",
    ".........................###############....................................##########..........################################",
    "..P..ooo..........oooW..################....................................##########..........################################",
    "##############..############....................................................................################################",
    "##############..############....................................................................################################",
    "##############..############....................................................................################################",
    "##############..############....................................................................################################",
    NULL
};

/* ── parsing ─────────────────────────────────────────────────────────── */

static void def_reset(PmLevelDef *def) {
    memset(def, 0, sizeof(*def));
    def->time_limit = 240;
    def->spawn_tx = def->spawn_ty = -1;
    def->goal_tx = def->goal_ty = -1;
}

static int valid_row_char(int c) {
    return c == '#' || c == '=' || c == '^' || c == '~' || c == 'o' || c == 'S' ||
           c == 'C' || c == 'G' || c == 'P' || c == 'W' || c == 'F' || c == 'M' ||
           c == 'V' || c == '.' || c == ' ';
}

int pm_level_parse(const char *text, size_t size, PmLevelDef *out) {
    if (!text || !size || !out) return 0;
    def_reset(out);
    size_t pos = 0;
    int rows = 0;
    while (pos < size && rows < PM_MAX_H) {
        size_t eol = pos;
        while (eol < size && text[eol] != '\n') eol++;
        const char *line = text + pos;
        size_t len = eol - pos;
        while (len > 0 && (line[len-1] == '\r' || line[len-1] == '\n')) len--;
        pos = eol + 1;
        if (len == 0) continue;
        if (len >= 5 && !strncmp(line, "name ", 5)) {
            size_t n = len - 5;
            if (n >= sizeof(out->name)) n = sizeof(out->name) - 1;
            memcpy(out->name, line + 5, n);
            out->name[n] = 0;
            continue;
        }
        if (len >= 6 && !strncmp(line, "theme ", 6)) {
            if (len >= 11 && !strncmp(line + 6, "stone", 5)) out->theme = PM_THEME_STONE;
            else if (len >= 10 && !strncmp(line + 6, "sand", 4)) out->theme = PM_THEME_SAND;
            else out->theme = PM_THEME_GRASS;
            continue;
        }
        if (len >= 5 && !strncmp(line, "time ", 5)) {
            int t = atoi(line + 5);
            out->time_limit = t >= 30 && t <= 9999 ? t : 240;
            continue;
        }
        int map_chars = 0;
        for (size_t i = 0; i < len; i++) if (valid_row_char((unsigned char)line[i])) map_chars++;
        if (map_chars * 2 < (int)len) continue; /* not a map row */
        int tx = 0;
        unsigned char *row = out->tiles + (size_t)rows * PM_MAX_W;
        for (size_t i = 0; i < len && tx < PM_MAX_W; i++) {
            unsigned char c = (unsigned char)line[i];
            int code = PM_EMPTY;
            switch (c) {
                case '#': code = PM_SOLID; break;
                case '=': code = PM_ONEWAY; break;
                case '^': code = PM_SPIKE; break;
                case '~': code = PM_LAVA; break;
                case 'o': code = PM_COIN; out->coins_total++; break;
                case 'S': code = PM_SPRING; break;
                case 'C': code = PM_CHECK; break;
                case 'G': code = PM_GOAL; out->goal_tx = tx; out->goal_ty = rows; break;
                case 'P': if (out->spawn_tx < 0) { out->spawn_tx = tx; out->spawn_ty = rows; } break;
                case 'W': case 'F':
                    if (out->enemy_count < PM_MAX_ENEMIES) {
                        out->enemies[out->enemy_count].tx = tx;
                        out->enemies[out->enemy_count].ty = rows;
                        out->enemies[out->enemy_count].kind =
                            c == 'W' ? PM_ENEMY_WALKER : PM_ENEMY_FLYER;
                        out->enemy_count++;
                    }
                    break;
                case 'M': case 'V':
                    if (out->lift_count < PM_MAX_PLATFORMS) {
                        out->lifts[out->lift_count].tx = tx;
                        out->lifts[out->lift_count].ty = rows;
                        out->lifts[out->lift_count].vertical = c == 'V';
                        out->lift_count++;
                    }
                    break;
                default: break;
            }
            row[tx++] = (unsigned char)code;
        }
        if (tx > out->w) out->w = tx;
        rows++;
    }
    out->h = rows;
    if (rows < 6 || out->w < 8) return 0;
    if (out->spawn_tx < 0 || out->goal_tx < 0) return 0;
    if (!out->name[0]) snprintf(out->name, sizeof(out->name), "Level %d", def_count + 1);
    return 1;
}

static int def_add_lines(const char **lines) {
    if (def_count >= PM_MAX_LEVELS) return 0;
    char buffer[16384];
    size_t used = 0;
    for (int i = 0; lines[i] && used < sizeof(buffer) - 2; i++) {
        int n = snprintf(buffer + used, sizeof(buffer) - used, "%s\n", lines[i]);
        if (n <= 0) break;
        used += (size_t)n;
    }
    if (!pm_level_parse(buffer, used, &defs[def_count])) return 0;
    def_count++;
    return 1;
}

static int def_add_asset(AAssetManager *assets, const char *path) {
    if (def_count >= PM_MAX_LEVELS || !assets) return 0;
    uint8_t *data; size_t size;
    if (!asset_read(assets, path, &data, &size)) return 0;
    int ok = pm_level_parse((const char *)data, size, &defs[def_count]);
    free(data);
    if (!ok) { app_log_error("platformium: rejecting malformed level %s", path); return 0; }
    app_log("platformium: loaded level %s", path);
    def_count++;
    return 1;
}

int pm_levels_build(AAssetManager *assets) {
    def_count = 0;
    if (!def_add_lines(level_green_hills) ||
        !def_add_lines(level_cave_depths) ||
        !def_add_lines(level_sky_ruins)) {
        app_fail("Platformium: built-in level data is malformed");
        return 0;
    }
    /* Optional extra platformers shipped as text assets. */
    def_add_asset(assets, "levels/bonus.txt");
    for (int i = 1; i <= 8; i++) {
        char path[64];
        snprintf(path, sizeof(path), "levels/extra%d.txt", i);
        def_add_asset(assets, path);
    }
    live_valid = 0;
    return 1;
}

int pm_level_def_count(void) { return def_count; }

/* Tests and tools may append a parsed level to the table. */
int pm_level_install(const PmLevelDef *def) {
    if (!def || def_count >= PM_MAX_LEVELS) return -1;
    memcpy(&defs[def_count], def, sizeof(*def));
    return def_count++;
}
const PmLevelDef *pm_level_def(int index) {
    if (index < 0 || index >= def_count) return NULL;
    return &defs[index];
}

void pm_level_begin(int index) {
    live_valid = 0;
    const PmLevelDef *def = pm_level_def(index);
    if (!def) return;
    memcpy(&live, def, sizeof(live));
    memset(taken, 0, sizeof(taken));
    live_valid = 1;
}

PmLevelDef *pm_level_live(void) { return live_valid ? &live : NULL; }

int pm_tile(int tx, int ty) {
    if (!live_valid || tx < 0 || ty < 0 || tx >= live.w || ty >= live.h) return PM_EMPTY;
    return live.tiles[(size_t)ty * PM_MAX_W + tx];
}

int pm_tile_solid(int tx, int ty) {
    int t = pm_tile(tx, ty);
    return t == PM_SOLID || t == PM_SPRING;
}

int pm_tile_taken(int tx, int ty) {
    if (!live_valid || tx < 0 || ty < 0 || tx >= live.w || ty >= live.h) return 1;
    return taken[(size_t)ty * PM_MAX_W + tx];
}

void pm_tile_take(int tx, int ty) {
    if (!live_valid || tx < 0 || ty < 0 || tx >= live.w || ty >= live.h) return;
    taken[(size_t)ty * PM_MAX_W + tx] = 1;
}

int pm_box_hits(float x, float y, float hw, float hh, float prev_bottom, int oneway) {
    int x0 = (int)floorf(x - hw), x1 = (int)floorf(x + hw);
    int y0 = (int)floorf(y - hh), y1 = (int)floorf(y + hh);
    for (int ty = y0; ty <= y1; ty++) {
        for (int tx = x0; tx <= x1; tx++) {
            int t = pm_tile(tx, ty);
            if (t == PM_SOLID || t == PM_SPRING) return 1;
            if (oneway && t == PM_ONEWAY && prev_bottom <= (float)ty + .001f) return 1;
        }
    }
    return 0;
}
