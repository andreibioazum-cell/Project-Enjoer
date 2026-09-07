/* App router: implements the generic game_* lifecycle by switching between
 * the two playsets — Geometrium (first-person block world) and Platformium
 * (2D platformer) — behind a launcher menu. The menu button lives in the
 * top-left corner in both modes; Escape / M also toggle it. */
#include <stdio.h>
#include "engine.h"
#include "geometrium/geometrium.h"
#include "platformium/platformium.h"

enum { MODE_GEOMETRIUM, MODE_PLATFORMIUM };
static int mode = MODE_GEOMETRIUM;
static int menu_open;
static int platformium_ready;

/* ── geometry shared with the HUD renderer and tests ─────────────────── */

static float ui(void) {
    float u = fminf(screen_w / 960.0f, screen_h / 540.0f);
    return u > 0 ? u : 0;
}

void game_menu_button_geom(float *x, float *y, float *r) {
    float u = ui();
    if (x) *x = 26 * u;
    if (y) *y = 26 * u;
    if (r) *r = 17 * u;
}

void game_menu_card_geom(int index, float *x, float *y, float *w, float *h) {
    float u = ui();
    float cw = fminf(620 * u, screen_w * .88f);
    float ch = 128 * u;
    float gap = 22 * u;
    float total = ch * 2 + gap;
    float top = screen_h * .5f - total * .5f + 34 * u;
    if (x) *x = screen_w * .5f - cw * .5f;
    if (y) *y = top + index * (ch + gap);
    if (w) *w = cw;
    if (h) *h = ch;
}

void game_menu_chip_geom(int index, float *x, float *y, float *w, float *h) {
    float cx, cy, cw, ch;
    game_menu_card_geom(1, &cx, &cy, &cw, &ch);
    float u = ui();
    float size = 40 * u, gap = 12 * u;
    if (x) *x = cx + 18 * u + index * (size + gap);
    if (y) *y = cy + ch - size - 14 * u;
    if (w) *w = size;
    if (h) *h = size;
}

int game_menu_open(void) { return menu_open; }
int game_current_mode(void) { return mode; }

/* ── menu visuals ────────────────────────────────────────────────────── */

static void text_center_shadow(const char *s, float x, float y, uint32_t c, float scale) {
    float w = text_width(s) * scale;
    text_scaled(s, x - w * .5f + scale * 4, y + scale * 4, 0x88000000u, scale);
    text_scaled(s, x - w * .5f, y, c, scale);
}

static void draw_menu_button(void) {
    float x, y, r;
    game_menu_button_geom(&x, &y, &r);
    if (r <= 0) return;
    circle(x, y, r, 0xb3ffffffu);
    for (int i = -1; i <= 1; i++)
        rect(x - r * .45f, y + i * r * .36f - r * .09f, r * .9f, r * .18f, 0xff232a35u);
}

static void draw_menu(void) {
    float u = ui();
    if (u <= 0) return;
    rect(0, 0, (float)screen_w, (float)screen_h, 0xe610141bu);
    text_center_shadow("ENJOER", screen_w * .5f, screen_h * .10f, 0xffffffffu, 1.1f * u);
    text_center_shadow("pick a playset", screen_w * .5f, screen_h * .10f + 56 * u, 0xaa9fb4c8u, .46f * u);

    for (int card = 0; card < 2; card++) {
        float x, y, w, h;
        game_menu_card_geom(card, &x, &y, &w, &h);
        int active = card == mode;
        roundrect(x, y, w, h, 12 * u, active ? 0xf22e3a49u : 0xf2232a35u);
        roundrect(x + 6 * u, y + 6 * u, 10 * u, h - 12 * u, 5 * u,
                  card == MODE_GEOMETRIUM ? 0xff58c8f0u : 0xff2ee6a8u);
        if (card == MODE_GEOMETRIUM) {
            text_center_shadow("Geometrium", x + w * .5f, y + 22 * u, 0xffffffffu, .62f * u);
            text_center_shadow("first-person block world — dig, build, explore",
                               x + w * .5f, y + 62 * u, 0xaa9fb4c8u, .40f * u);
        } else {
            text_center_shadow("Platformium", x + w * .5f, y + 16 * u, 0xffffffffu, .62f * u);
            text_center_shadow("2D platformer — coins, springs, lifts and flags",
                               x + w * .5f, y + 52 * u, 0xaa9fb4c8u, .40f * u);
            /* level chips */
            int count = platformium_ready ? platformium_level_count() : 0;
            for (int i = 0; i < count; i++) {
                float lx, ly, lw, lh;
                game_menu_chip_geom(i, &lx, &ly, &lw, &lh);
                int unlocked = platformium_level_unlocked(i);
                roundrect(lx, ly, lw, lh, 8 * u,
                          unlocked ? 0xff2ee6a8u : 0xff3a4453u);
                char label[4];
                static const char chip_digits[] = "123456789ABCDEFG";
                if (unlocked && i < (int)sizeof(chip_digits) - 1) {
                    label[0] = chip_digits[i];
                    label[1] = 0;
                } else {
                    label[0] = 'x';
                    label[1] = 0;
                }
                text_center_shadow(label, lx + lw * .5f, ly + lh * .5f - 12 * u,
                                   unlocked ? 0xff12241cu : 0xff7c8a99u, .44f * u);
            }
        }
        if (active)
            text_center_shadow("playing", x + w - 52 * u, y + 10 * u, 0xff2ee6a8u, .30f * u);
    }
    text_center_shadow("tap a card to play — level chips pick a Platformium level",
                       screen_w * .5f, screen_h - 44 * u, 0x889fb4c8u, .36f * u);
}

/* ── lifecycle ───────────────────────────────────────────────────────── */

void game_init(AAssetManager *assets) {
    geometrium_game_init(assets);
    platformium_ready = platformium_init(assets);
    mode = MODE_GEOMETRIUM;
    menu_open = 0;
}

void game_update(void) {
    if (platformium_ready && platformium_take_menu_request()) menu_open = 1;
    if (menu_open) return;   /* the world behind the menu stays frozen */
    if (mode == MODE_GEOMETRIUM) geometrium_game_update();
    else platformium_update((float)dt);
}

void game_draw(Buffer *buffer) {
    if (mode == MODE_GEOMETRIUM) geometrium_game_draw(buffer);
    else platformium_draw();
    if (menu_open) draw_menu();
    else draw_menu_button();
}

void game_touch(float x, float y, int action, int id) {
    float u = ui();
    if (u <= 0) return;
    if (menu_open) {
        if (action != 0) return;
        /* level chips first: they live inside the platformium card */
        if (platformium_ready) {
            int count = platformium_level_count();
            for (int i = 0; i < count; i++) {
                float lx, ly, lw, lh;
                game_menu_chip_geom(i, &lx, &ly, &lw, &lh);
                if (x >= lx && x <= lx + lw && y >= ly && y <= ly + lh) {
                    if (!platformium_level_unlocked(i)) return;
                    platformium_start_level(i);
                    mode = MODE_PLATFORMIUM;
                    menu_open = 0;
                    return;
                }
            }
        }
        for (int card = 0; card < 2; card++) {
            float cx, cy, cw, ch;
            game_menu_card_geom(card, &cx, &cy, &cw, &ch);
            if (x < cx || x > cx + cw || y < cy || y > cy + ch) continue;
            if (card == MODE_GEOMETRIUM) {
                mode = MODE_GEOMETRIUM;
            } else if (platformium_ready) {
                if (!platformium_started()) platformium_start_level(0);
                mode = MODE_PLATFORMIUM;
            } else {
                return;
            }
            menu_open = 0;
            geometrium_cancel_input();
            platformium_cancel_input();
            return;
        }
        return;
    }
    /* the menu button toggles the launcher in both modes */
    float bx, by, br;
    game_menu_button_geom(&bx, &by, &br);
    if (action == 0 && br > 0) {
        float dx = x - bx, dy = y - by;
        if (dx * dx + dy * dy <= br * br * 1.69f) {
            menu_open = 1;
            geometrium_cancel_input();
            platformium_cancel_input();
            return;
        }
    }
    if (mode == MODE_GEOMETRIUM) geometrium_game_touch(x, y, action, id);
    else platformium_touch(x, y, action, id);
}

void game_key(const char *name, int down) {
    if (!name) return;
    if (down && (!strcmp(name, "Escape") || !strcmp(name, "m"))) {
        menu_open = !menu_open;
        geometrium_cancel_input();
        platformium_cancel_input();
        return;
    }
    if (menu_open) return;
    if (mode == MODE_GEOMETRIUM) geometrium_key(name, down);
    else platformium_key(name, down);
}

void game_cancel_input(void) {
    geometrium_cancel_input();
    platformium_cancel_input();
}

void game_reset(void) {
    if (mode == MODE_GEOMETRIUM) geometrium_game_reset();
    else if (platformium_ready) platformium_start_level(0);
}

void game_save(void) {
    geometrium_game_save();
    if (platformium_ready) platformium_progress_save();
}
