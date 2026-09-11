/* App screens: the Minecraft-style main menu and the Geometrium block world.
 *
 * game_* is the lifecycle every platform entry point drives (Android native
 * activity, PC preview, host tests). The app boots into the main menu —
 * flat beveled buttons, Play / Servers / Options / Quit — with the block
 * world frozen behind a dark overlay. "Play" starts the first-person
 * world; the round button, Esc, M or the Android back key return to the
 * menu. All UI text is English: the bundled font carries Latin only. */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "engine.h"
#include "engine/render/rend_internal.h"
#include "engine/render/voxel_internal.h"
#include "geometrium/geometrium.h"
#include "geometrium/geometrium_internal.h"
#include "core/settings_internal.h"

enum { SCREEN_MAIN = 0, SCREEN_SERVERS, SCREEN_SETTINGS, SCREEN_GAME };

enum {
    ACT_NONE = -1,
    ACT_PLAY = 0,     /* main: Play -> game            */
    ACT_SERVERS,      /* main: Servers                 */
    ACT_OPTIONS,      /* main: Options                 */
    ACT_QUIT,         /* main: Quit -> app_quit        */
    ACT_BACK,         /* sub-screens: Back -> main     */
    ACT_VOLUME,       /* options: Sound N% (cycle 5%)  */
    ACT_QUALITY,      /* options: Quality (cycle)      */
};

#define MENU_BUTTON_MAX 6

typedef struct {
    float x, y, w, h;
    int action;
} MenuButton;

static AAssetManager *assets;
static int screen = SCREEN_MAIN;
static int sel = 0;          /* keyboard selection index            */
static int hover = -1;       /* pointer hover index                 */
static int pointer_down;     /* press started on a menu button      */
static int ready;
static int geometrium_ready;

static float ui(void) {
    float u = fminf(screen_w / 960.0f, screen_h / 540.0f);
    return u > 0 ? u : 0;
}

/*
 * The main world stays built and warm while the menu is open, so "Play"
 * drops into a fully streamed field and the menu background shows it.
 */
static void warm_world(void) {
    for (int i = 0; i < 400; i++) {
        voxel_water_update(.1f);
        voxel_world_update(8.5f, 8.5f);
    }
}

void game_init(AAssetManager *am) {
    assets = am;
    settings_load();
    if (rend_materials_load(assets)) {
        geometrium_game_init(assets);
        geometrium_ready = 1;
    }
    if (geometrium_ready) warm_world();
    screen = SCREEN_MAIN;
    sel = 0;
    hover = -1;
    pointer_down = 0;
    ready = 1;
}

int game_menu_open(void) { return screen != SCREEN_GAME; }
int game_screen(void) { return screen; }
int game_in_geometrium(void) { return screen == SCREEN_GAME; }
int game_settings_volume(void) { return settings_volume(); }
int game_settings_quality(void) { return settings_quality(); }

const char *game_screen_name(void) {
    switch (screen) {
    case SCREEN_SERVERS: return "servers";
    case SCREEN_SETTINGS: return "options";
    case SCREEN_GAME: return "game";
    default: return "main";
    }
}

/*
 * Flat Minecraft-style button: a beveled square face (light top/left, dark
 * bottom/right edges), centered label with a small drop shadow. No rounding.
 */
static void draw_button(const MenuButton *b, int active, const char *label) {
    uint32_t face = active ? 0xff7d8cc0u : 0xff6f6f6fu;
    uint32_t light = active ? 0xff9aa6d8u : 0xff9c9c9cu;
    uint32_t dark = active ? 0xff4a5a8au : 0xff3d3d3du;
    rect(b->x, b->y, (float)b->w, (float)b->h, face);
    rect(b->x, b->y, (float)b->w, 2.f, light);
    rect(b->x, b->y, 2.f, (float)b->h, light);
    rect(b->x, b->y + (float)b->h - 2.f, (float)b->w, 2.f, dark);
    rect(b->x + (float)b->w - 2.f, b->y, 2.f, (float)b->h, dark);
    float sc = .5f * ui();
    float tw = text_width(label) * sc;
    float tx = b->x + ((float)b->w - tw) * .5f;
    float ty = b->y + (float)b->h * .5f + sc * .35f;   /* baseline */
    text_scaled(label, tx + 2.f, ty + 2.f, 0xff3f3f3fu, sc);
    text_scaled(label, tx, ty, active ? 0xffffee9cu : 0xffffffffu, sc);
}

/* The current screen's flat buttons, in keyboard-selection order. */
static int menu_buttons(MenuButton *out, int cap) {
    int n = 0;
    float u = ui();
    float w = (float)screen_w, h = (float)screen_h;
    if (screen == SCREEN_MAIN) {
        float bw = 400.f * u, bh = 44.f * u, gap = 10.f * u;
        float total = 4.f * bh + 3.f * gap;
        float x = w * .5f - bw * .5f;
        float y = h * .5f - total * .5f + 24.f * u;
        for (int i = 0; i < 4; i++) {
            if (n < cap) {
                out[n].x = x; out[n].y = y + (float)i * (bh + gap);
                out[n].w = (int)bw; out[n].h = (int)bh;
                out[n].action = ACT_PLAY + i;
            }
            n++;
        }
    } else if (screen == SCREEN_SERVERS) {
        if (n < cap) {
            out[n].x = w - 168.f * u; out[n].y = h - 64.f * u;
            out[n].w = (int)(150.f * u); out[n].h = (int)(36.f * u);
            out[n].action = ACT_BACK;
        }
        n++;
    } else { /* SCREEN_SETTINGS */
        float bw = 460.f * u, bh = 44.f * u, gap = 10.f * u;
        float x = w * .5f - bw * .5f;
        float y = h * .5f - (2.f * bh + gap) * .5f + 12.f * u;
        if (n < cap) {
            out[n].x = x; out[n].y = y; out[n].w = (int)bw; out[n].h = (int)bh;
            out[n].action = ACT_VOLUME;
        }
        n++;
        if (n < cap) {
            out[n].x = x; out[n].y = y + bh + gap; out[n].w = (int)bw; out[n].h = (int)bh;
            out[n].action = ACT_QUALITY;
        }
        n++;
        if (n < cap) {
            out[n].x = w - 168.f * u; out[n].y = h - 64.f * u;
            out[n].w = (int)(150.f * u); out[n].h = (int)(36.f * u);
            out[n].action = ACT_BACK;
        }
        n++;
    }
    return n;
}

void game_menu_button_geom(int index, float *x, float *y, float *w, float *h) {
    MenuButton b[MENU_BUTTON_MAX];
    int n = menu_buttons(b, MENU_BUTTON_MAX);
    if (index < 0 || index >= n) {
        if (x) *x = 0;
        if (y) *y = 0;
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    if (x) *x = b[index].x;
    if (y) *y = b[index].y;
    if (w) *w = (float)b[index].w;
    if (h) *h = (float)b[index].h;
}

/* The round in-game menu button (top-left, Minecraft pause style). */
void game_menu_button_geom2(float *x, float *y, float *r) {
    float u = ui();
    if (x) *x = 26.f * u;
    if (y) *y = 26.f * u;
    if (r) *r = 17.f * u;
}

/* Label of a button action; SETTINGS rows fill "value" with the live text. */
static void action_label(int action, char *value, size_t value_cap, const char **label) {
    switch (action) {
    case ACT_PLAY: *label = "Play"; break;
    case ACT_SERVERS: *label = "Servers"; break;
    case ACT_OPTIONS: *label = "Options"; break;
    case ACT_QUIT: *label = "Quit"; break;
    case ACT_BACK: *label = "Back"; break;
    case ACT_VOLUME:
        snprintf(value, value_cap, "Sound: %d%%", settings_volume());
        *label = value;
        break;
    case ACT_QUALITY:
        snprintf(value, value_cap, "Render quality: %s",
                 settings_quality() == REND_QUALITY_HIGH ? "High"
                 : settings_quality() == REND_QUALITY_MEDIUM ? "Medium"
                 : settings_quality() == REND_QUALITY_LOW ? "Low" : "Auto");
        *label = value;
        break;
    default: *label = ""; break;
    }
}

static void draw_bottom_fps(void) {
    float u = ui();
    char line[24];
    snprintf(line, sizeof(line), "FPS %.0f", rend_fps() < 0 ? 0. : rend_fps());
    text_scaled(line, 58.f * u, (float)screen_h - 16.f * u, 0xaaffffffu, .34f * u);
}

/* The round hamburger button (top-left): opens the main menu. */
static void draw_menu_button(void) {
    float x, y, r;
    game_menu_button_geom2(&x, &y, &r);
    if (r <= 0) return;
    circle(x, y, r, 0xb3ffffffu);
    for (int i = -1; i <= 1; i++)
        rect(x - r * .45f, y + (float)i * r * .36f - r * .09f, r * .9f, r * .18f, 0xff232a35u);
}

static void draw_screen_title(const char *title, float u) {
    float sc = .9f * u;
    float tx = (float)screen_w * .5f - text_width(title) * sc * .5f;
    text_scaled(title, tx + 2.f, 66.f * u + 2.f, 0xff1a1a1au, sc);
    text_scaled(title, tx, 66.f * u, 0xffffffffu, sc);
}

static void draw_menu_screen(void) {
    float u = ui();
    float w = (float)screen_w, h = (float)screen_h;
    MenuButton b[MENU_BUTTON_MAX];
    int n = menu_buttons(b, MENU_BUTTON_MAX);
    if (screen == SCREEN_MAIN) {
        float sc = 1.25f * u;
        const char *title = "ENJOER";
        float tx = w * .5f - text_width(title) * sc * .5f;
        text_scaled(title, tx + 3.f, 92.f * u + 3.f, 0xff141414u, sc);
        text_scaled(title, tx, 92.f * u, 0xffffffffu, sc);
        text_scaled("holes everywhere!", w * .5f + 150.f * u, 108.f * u, 0xffff55u, .42f * u);
        text_scaled("Enjoer 1.0", 10.f * u, h - 10.f * u, 0xff9a9a9au, .34f * u);
    } else if (screen == SCREEN_SERVERS) {
        draw_screen_title("Servers", u);
        const char *msg = "No servers - single player only";
        text_scaled(msg, w * .5f - text_width(msg) * .25f * u, h * .5f, 0xffffffffu, .5f * u);
    } else { /* SCREEN_SETTINGS */
        draw_screen_title("Options", u);
        const char *hint = "arrows: change  enter: select  esc: back";
        text_scaled(hint, w * .5f - text_width(hint) * .17f * u, h * .5f + 96.f * u, 0xb8ffffffu, .34f * u);
    }
    for (int i = 0; i < n; i++) {
        char value[64];
        const char *label;
        action_label(b[i].action, value, sizeof(value), &label);
        draw_button(&b[i], i == sel || i == hover, label);
    }
}

static void set_vol_wrap(int v) {
    if (v < 0) v = 100;
    if (v > 100) v = 0;
    settings_set_volume(v);
}

static void set_qual_wrap(int step) {
    int q = settings_quality() + step;
    if (q < REND_QUALITY_AUTO) q = REND_QUALITY_LOW;
    if (q > REND_QUALITY_LOW) q = REND_QUALITY_AUTO;
    settings_set_quality(q);
}

/*
 * Menu action dispatch (keyboard Enter/space or touch release).
 */
static void activate(int action) {
    if (action == ACT_NONE) return;
    switch (action) {
    case ACT_PLAY:
        if (geometrium_ready) { screen = SCREEN_GAME; sel = 0; }
        break;
    case ACT_SERVERS: screen = SCREEN_SERVERS; sel = 0; break;
    case ACT_OPTIONS: screen = SCREEN_SETTINGS; sel = 0; break;
    case ACT_QUIT: app_quit(); break;
    case ACT_BACK: screen = SCREEN_MAIN; sel = 0; break;
    case ACT_VOLUME:
        set_vol_wrap(settings_volume() + 5);
        settings_save();
        break;
    case ACT_QUALITY:
        set_qual_wrap(1);
        settings_save();
        break;
    default: break;
    }
}

void game_update(void) {
    if (!ready || !geometrium_ready) return;
    if (screen == SCREEN_GAME) {
        geometrium_game_update();
    } else {
        /* keep perf stats honest while the menu holds the frozen world */
        rend_perf_frame(.016);
    }
}

void game_draw(Buffer *buffer) {
    if (!buffer || !ready) return;
    if (screen == SCREEN_GAME) {
        geometrium_game_draw(buffer);
        draw_menu_button();
        draw_bottom_fps();
        return;
    }
    /* Frozen world behind a dark overlay, then the flat menu. */
    if (geometrium_ready) geometrium_scene_draw(buffer);
    rect(0.f, 0.f, (float)screen_w, (float)screen_h, 0xb8000000u);
    draw_menu_screen();
}

static void to_menu(void) {
    geometrium_cancel_input();
    screen = SCREEN_MAIN;
    sel = 0;
    hover = -1;
    pointer_down = 0;
}

void game_key(const char *k, int d) {
    if (!ready || !k) return;
    if (screen == SCREEN_GAME) {
        if (d && (strcmp(k, "Escape") == 0 || strcmp(k, "m") == 0)) {
            to_menu();
            return;
        }
        geometrium_key(k, d);
        return;
    }
    if (!d) return;
    MenuButton tmp[1];
    int count = menu_buttons(tmp, 1);
    if (strcmp(k, "Escape") == 0 || strcmp(k, "m") == 0) {
        /* Sub-screens step back to the main menu; the main menu is the entry
         * point and stays put (Android's Back key is routed here too). */
        if (screen != SCREEN_MAIN) to_menu();
        return;
    }
    if (strcmp(k, "ArrowDown") == 0 || strcmp(k, "s") == 0 || strcmp(k, "S") == 0) {
        sel = (sel + 1) % count;
    } else if (strcmp(k, "ArrowUp") == 0 || strcmp(k, "w") == 0 || strcmp(k, "W") == 0) {
        sel = (sel + count - 1) % count;
    } else if (strcmp(k, "ArrowLeft") == 0 || strcmp(k, "a") == 0 || strcmp(k, "A") == 0 ||
               strcmp(k, "ArrowRight") == 0 || strcmp(k, "d") == 0 || strcmp(k, "D") == 0) {
        MenuButton b[MENU_BUTTON_MAX];
        int n = menu_buttons(b, MENU_BUTTON_MAX);
        if (sel < n) {
            int left = strcmp(k, "ArrowLeft") == 0 || strcmp(k, "a") == 0 || strcmp(k, "A") == 0;
            if (b[sel].action == ACT_VOLUME)
                set_vol_wrap(settings_volume() + (left ? -5 : 5));
            else if (b[sel].action == ACT_QUALITY)
                set_qual_wrap(left ? -1 : 1);
            else return;
            settings_save();
        }
    } else if (strcmp(k, "Enter") == 0 || strcmp(k, " ") == 0 || strcmp(k, "space") == 0) {
        MenuButton b[MENU_BUTTON_MAX];
        int n = menu_buttons(b, MENU_BUTTON_MAX);
        if (sel < n) activate(b[sel].action);
    }
}

void game_touch(float x, float y, int action, int pointer_id) {
    if (!ready) return;
    if (screen == SCREEN_GAME) {
        float bx, by, br;
        game_menu_button_geom2(&bx, &by, &br);
        if (x >= bx - br && x <= bx + br && y >= by - br && y <= by + br) {
            if (action == 0) to_menu();
            return;
        }
        geometrium_game_touch(x, y, action, pointer_id);
        return;
    }
    MenuButton b[MENU_BUTTON_MAX];
    int n = menu_buttons(b, MENU_BUTTON_MAX);
    int hit = -1;
    for (int i = 0; i < n; i++)
        if (x >= b[i].x && x < b[i].x + b[i].w && y >= b[i].y && y < b[i].y + b[i].h) {
            hit = i;
            break;
        }
    /* Android/preview semantics: 0 down, 1 up, 2 move. */
    if (action == 0) {
        hover = hit;
        pointer_down = hit >= 0;
    } else if (action == 2) {
        if (pointer_down) hover = hit;
    } else if (action == 1 && pointer_down) {
        pointer_down = 0;
        hover = -1;
        if (hit >= 0 && hit < n) activate(b[hit].action);
    }
}

void game_reset(void) {
    if (!ready) return;
    if (screen == SCREEN_GAME && geometrium_ready) geometrium_game_reset();
}

void game_save(void) {
    settings_save();
    if (screen == SCREEN_GAME && geometrium_ready) voxel_edits_save();
}

void game_cancel_input(void) {
    if (screen == SCREEN_GAME) geometrium_cancel_input();
}
