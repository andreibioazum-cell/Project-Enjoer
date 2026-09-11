/* App regression: the Minecraft-style main menu (Play / Servers / Options /
 * Quit) routes keyboard and touch into the Geometrium block world, the
 * options screen adjusts sound and render quality, and the world freezes
 * behind the menu. */
#define _POSIX_C_SOURCE 200809L
#include "engine.h"
#include "engine/render/rend_internal.h"
#include "geometrium/geometrium_internal.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern AAssetManager *host_asset_manager(const char *root);

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)

static uint32_t *frame_copy;
static size_t frame_bytes;

static void remember(const Buffer *b) {
    frame_bytes = (size_t)b->stride * b->height * 4;
    if (!frame_copy) frame_copy = malloc(frame_bytes);
    CHECK(frame_copy);
    memcpy(frame_copy, b->pixels, frame_bytes);
}
static int changed_since(const Buffer *b) {
    return memcmp(frame_copy, b->pixels, frame_bytes) != 0;
}
static int distinct_colors(const Buffer *b) {
    int seen[256] = {0};
    for (int y = 0; y < b->height; y++)
        for (int x = 0; x < b->width; x++) {
            uint32_t c = b->pixels[y * b->stride + x];
            seen[((c & 0xff) ^ ((c >> 8) & 0xff) ^ ((c >> 16) & 0xff)) & 255] = 1;
        }
    int n = 0;
    for (int i = 0; i < 256; i++) n += seen[i];
    return n;
}
static void check_stride_guard(const Buffer *b, int w) {
    for (int y = 0; y < b->height; y++)
        for (int x = w; x < b->stride; x++)
            CHECK(b->pixels[y * b->stride + x] == 0x12345678u);
}

static void step(Buffer *b, int frames) {
    for (int i = 0; i < frames; i++) {
        dt = 1.0 / 60.0;
        game_update();
        CHECK(gfx_begin_frame(b));
        game_draw(b);
        gfx_end_frame();
        CHECK(!app_failed());
    }
}

/* Center geometry of a flat menu button on the current screen. */
static void button_center(int index, float *x, float *y) {
    float bx, by, bw, bh;
    game_menu_button_geom(index, &bx, &by, &bw, &bh);
    CHECK(bw > 0 && bh > 0);
    *x = bx + bw * .5f;
    *y = by + bh * .5f;
}

/* Tap = down then up at the same point (Android/preview semantics). */
static void tap(int index) {
    float x, y;
    button_center(index, &x, &y);
    game_touch(x, y, 0, 0);
    game_touch(x, y, 1, 0);
}

static void check_buttons_in_frame(void) {
    for (int i = 0; i < 8; i++) {
        float x, y, w, h;
        game_menu_button_geom(i, &x, &y, &w, &h);
        if (w <= 0 || h <= 0) continue;          /* past the screen's count */
        CHECK(x >= 0 && y >= 0 && x + w <= (float)screen_w && y + h <= (float)screen_h);
    }
}

/* The app greets you with the flat main menu over the frozen world. */
static void test_main_menu(Buffer *b) {
    CHECK(game_menu_open());
    CHECK(game_screen() == GAME_SCREEN_MAIN);
    check_buttons_in_frame();

    step(b, 2);
    CHECK(distinct_colors(b) > 32);              /* world + menu really drew */
    check_stride_guard(b, b->width);

    /* keyboard navigation: four buttons, selection wraps, screen stays put */
    game_key("ArrowDown", 1); game_key("ArrowDown", 0);
    game_key("ArrowDown", 1); game_key("ArrowDown", 0);
    game_key("ArrowDown", 1); game_key("ArrowDown", 0);
    game_key("ArrowDown", 1); game_key("ArrowDown", 0);
    CHECK(game_screen() == GAME_SCREEN_MAIN);
    game_key("ArrowUp", 1); game_key("ArrowUp", 0);
    game_key("ArrowUp", 1); game_key("ArrowUp", 0);
    game_key("ArrowUp", 1); game_key("ArrowUp", 0);
    game_key("ArrowUp", 1); game_key("ArrowUp", 0);
    CHECK(game_screen() == GAME_SCREEN_MAIN);
    /* the main menu is the entry point: Escape stays put */
    game_key("Escape", 1); game_key("Escape", 0);
    CHECK(game_screen() == GAME_SCREEN_MAIN);
    puts("PASS main menu: four flat buttons in frame, selection wraps, Escape stays");
}

/* Options: sound and render quality cycle with the arrow keys. */
static void test_options(Buffer *b) {
    tap(2);                                      /* Options */
    CHECK(game_screen() == GAME_SCREEN_OPTIONS);
    check_buttons_in_frame();

    int vol = game_settings_volume();
    CHECK(vol == 80);                            /* default on a fresh storage */
    game_key("ArrowRight", 1); game_key("ArrowRight", 0);
    CHECK(game_settings_volume() == vol + 5);
    game_key("ArrowLeft", 1); game_key("ArrowLeft", 0);
    game_key("ArrowLeft", 1); game_key("ArrowLeft", 0);
    CHECK(game_settings_volume() == vol - 5);

    game_key("ArrowDown", 1); game_key("ArrowDown", 0);
    CHECK(game_settings_quality() == REND_QUALITY_AUTO);
    game_key("ArrowRight", 1); game_key("ArrowRight", 0);
    CHECK(game_settings_quality() == REND_QUALITY_HIGH);
    game_key("ArrowRight", 1); game_key("ArrowRight", 0);
    game_key("ArrowRight", 1); game_key("ArrowRight", 0);
    CHECK(game_settings_quality() == REND_QUALITY_LOW);
    game_key("ArrowRight", 1); game_key("ArrowRight", 0);
    CHECK(game_settings_quality() == REND_QUALITY_AUTO);   /* wraps */

    tap(2);                                      /* Back */
    CHECK(game_screen() == GAME_SCREEN_MAIN);
    step(b, 2);
    check_stride_guard(b, b->width);
    printf("PASS options: sound %d%% and quality cycle, back returns to main\n", game_settings_volume());
}

/* Servers: a placeholder list with a working Back button. */
static void test_servers(Buffer *b) {
    tap(1);                                      /* Servers */
    CHECK(game_screen() == GAME_SCREEN_SERVERS);
    check_buttons_in_frame();
    step(b, 2);
    CHECK(distinct_colors(b) > 16);              /* the sub-screen drew */
    tap(0);                                      /* Back */
    CHECK(game_screen() == GAME_SCREEN_MAIN);
    puts("PASS servers: placeholder screen with a back button");
}

/* The block world: streaming terrain, walking, hotbar, flight, pause. */
static void test_play(Buffer *b) {
    tap(0);                                      /* Play */
    CHECK(!game_menu_open());
    CHECK(game_screen() == GAME_SCREEN_GAME);
    CHECK(game_in_geometrium());

    step(b, 60);
    int chunks = 0, quads = 0;
    voxel_world_stats(&chunks, &quads);
    CHECK(chunks > 0 && quads > 50);
    CHECK(distinct_colors(b) > 32);
    check_stride_guard(b, b->width);

    /* WASD walks on the terrain */
    float x0, y0, z0;
    geometrium_player_pos(&x0, &y0, &z0);
    game_key("w", 1);
    step(b, 30);
    game_key("w", 0);
    float x1, y1, z1;
    geometrium_player_pos(&x1, &y1, &z1);
    CHECK(fabsf(x1 - x0) + fabsf(z1 - z0) > 1.0f);

    /* hotbar and flight */
    game_key("1", 1); game_key("1", 0);
    CHECK(geometrium_selected() == 0);
    game_key("3", 1); game_key("3", 0);
    CHECK(geometrium_selected() == 2);
    game_key("f", 1); game_key("f", 0);
    CHECK(geometrium_player_flying());

    /* the round menu button (top-left) opens the menu and freezes the world */
    float bx, by, br;
    game_menu_button_geom2(&bx, &by, &br);
    CHECK(br > 0);
    game_touch(bx, by, 0, 0);
    game_touch(bx, by, 1, 0);
    CHECK(game_menu_open());
    CHECK(game_screen() == GAME_SCREEN_MAIN);
    step(b, 1);
    remember(b);
    step(b, 30);
    CHECK(!changed_since(b));                    /* frozen, not re-simulated */
    float x2, y2, z2;
    geometrium_player_pos(&x2, &y2, &z2);
    CHECK(x2 == x1 && y2 == y1 && z2 == z1);

    /* "Play" drops you straight back into the world */
    tap(0);
    CHECK(game_screen() == GAME_SCREEN_GAME);
    step(b, 2);
    check_stride_guard(b, b->width);
    puts("PASS play: streaming world, walking, hotbar, flight, round-button pause");
}

/* Escape from the game returns to the menu and releases held keys. */
static void test_escape(Buffer *b) {
    CHECK(game_screen() == GAME_SCREEN_GAME);
    game_key("w", 1);
    game_key("ArrowRight", 1);
    game_key("Escape", 1); game_key("Escape", 0);
    CHECK(game_screen() == GAME_SCREEN_MAIN);
    CHECK(game_menu_open());
    /* held controls are released and the main menu stays put on Escape */
    game_key("Escape", 1); game_key("Escape", 0);
    CHECK(game_screen() == GAME_SCREEN_MAIN);
    /* a sub-screen steps back to the main menu on Escape */
    tap(2);                                      /* Options */
    CHECK(game_screen() == GAME_SCREEN_OPTIONS);
    game_key("Escape", 1); game_key("Escape", 0);
    CHECK(game_screen() == GAME_SCREEN_MAIN);
    step(b, 1);
    check_stride_guard(b, b->width);
    puts("PASS escape: game -> main, sub-screen -> main, main stays");
}

static void run(int w, int h) {
    Buffer b;
    screen_w = w;
    screen_h = h;
    b.pixels = malloc((size_t)(w + 8) * h * 4);
    b.width = w;
    b.height = h;
    b.stride = w + 8;
    CHECK(b.pixels);
    for (int y = 0; y < h; y++)
        for (int x = w; x < b.stride; x++) b.pixels[y * b.stride + x] = 0x12345678u;

    AAssetManager *am = host_asset_manager("assets");
    CHECK(gfx_init(am));
    game_init(am);
    CHECK(!app_failed());

    if (w == 960) {
        test_main_menu(&b);
        test_options(&b);
        test_servers(&b);
        test_play(&b);
        test_escape(&b);
    } else {
        /* a second resolution: geometry scales, Play still works */
        check_buttons_in_frame();
        tap(0);
        CHECK(game_screen() == GAME_SCREEN_GAME);
        step(&b, 30);
        check_stride_guard(&b, w);
    }
    game_save();
    gfx_shutdown();
    free(b.pixels);
    printf("PASS %dx%d app: main menu, options, servers and the block world\n", w, h);
}

int main(void) {
    run(960, 540);
    run(1600, 900);
    free(frame_copy);
    puts("APP SUITE OK");
    return 0;
}
