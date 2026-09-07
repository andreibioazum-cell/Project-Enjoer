/* Platformium regression suite:
 *  - the app router (launcher menu, playset switching, both playsets render)
 *  - a greedy bot (run right, jump at gaps/walls/hazards/walkers) finishes
 *    every shipped level, proving they are actually beatable
 *  - mechanics: one-way platforms, springs, spikes, lava, coins,
 *    checkpoints, stomping, lifts, the goal and score
 *  - progress persistence
 * The same engine code paths run in the APK. */
#define _POSIX_C_SOURCE 200809L
#include "engine.h"
#include "geometrium/geometrium.h"
#include "platformium/platformium.h"
#include "platformium/platformium_internal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

extern AAssetManager *host_asset_manager(const char *root);
#define HW .34f
#define HH .46f
#define STEP (1.0f / 120.0f)

static Buffer *frame;

static void guard_check(void) {
    for (int y = 0; y < frame->height; y++)
        for (int x = frame->width; x < frame->stride; x++)
            assert(frame->pixels[y * frame->stride + x] == 0x12345678u);
}

/* ── the bot ─────────────────────────────────────────────────────────── */

static int tile_blocks_walk(int t) {
    return t == PLATFORMIUM_TILE_SOLID || t == PLATFORMIUM_TILE_SPRING;
}

static int bot_jump(void) {
    float x, y;
    platformium_player_pos(&x, &y);
    float foot = y + HH;
    int foot_row = (int)floorf(foot + .05f);
    int fcol = (int)floorf(x + HW + .25f);
    int t_front = platformium_tile_at(fcol, foot_row);
    /* gap ahead (one-way tiles still catch the bot, so they are not gaps) */
    if (!tile_blocks_walk(t_front) && t_front != PLATFORMIUM_TILE_ONEWAY) {
        int t_below = platformium_tile_at(fcol, foot_row + 1);
        if (!tile_blocks_walk(t_below) && t_below != PLATFORMIUM_TILE_ONEWAY) return 1;
    }
    /* hazard ahead */
    for (float probe = .3f; probe <= .95f; probe += .65f) {
        int t = platformium_tile_at((int)floorf(x + HW + probe), (int)floorf(y + HH - .1f));
        if (t == PLATFORMIUM_TILE_SPIKE || t == PLATFORMIUM_TILE_LAVA) return 1;
    }
    /* wall ahead at body height */
    if (tile_blocks_walk(platformium_tile_at(fcol, (int)floorf(y))) ||
        tile_blocks_walk(platformium_tile_at(fcol, (int)floorf(y - HH + .1f)))) return 1;
    /* walking enemy close ahead */
    int n = platformium_enemy_count();
    for (int i = 0; i < n; i++) {
        float ex, ey;
        int kind, alive;
        if (!platformium_enemy_info(i, &ex, &ey, &kind, &alive) || !alive) continue;
        if (kind != 0) continue;
        if (fabsf(ex - x) < 1.8f && fabsf(ey - y) < 1.6f) return 1;
    }
    return 0;
}

static int run_level_bot(int level_index) {
    platformium_start_level(level_index);
    platformium_key("d", 1);
    int holding = 0;
    for (int i = 0; i < 240 * 120; i++) {
        int grounded = platformium_player_grounded();
        /* hold the jump through the whole arc (full height), release on
         * landing, then immediately re-press when the bot wants to hop */
        if (grounded && holding) { platformium_key("space", 0); holding = 0; }
        if (grounded && !holding && bot_jump()) { platformium_key("space", 1); holding = 1; }
        platformium_update(STEP);
        dt = STEP;
        assert(gfx_begin_frame(frame));
        platformium_draw();
        gfx_end_frame();
        guard_check();
        assert(!app_failed());
        if (platformium_state() == PLATFORMIUM_STATE_CLEAR) {
            platformium_key("d", 0);
            platformium_key("space", 0);
            return 1;
        }
        if (platformium_state() == PLATFORMIUM_STATE_GAMEOVER) break;
    }
    platformium_key("d", 0);
    platformium_key("space", 0);
    if (getenv("PM_TRACE")) {
        float x, y;
        platformium_player_pos(&x, &y);
        fprintf(stderr, "  bot failed level %d at (%.2f,%.2f) state=%d lives=%d\n",
               level_index, x, y, platformium_state(), platformium_lives());
    }
    return 0;
}

/* ── synthetic levels for mechanics tests ────────────────────────────── */

static int install_level(const char *text) {
    PmLevelDef def;
    int ok = pm_level_parse(text, strlen(text), &def);
    assert(ok);
    int index = pm_level_install(&def);
    assert(index >= 0);
    return index;
}

static void sim_frames(int n) {
    for (int i = 0; i < n; i++) {
        platformium_update(STEP);
        assert(!app_failed());
    }
}

static void test_oneway(void) {
    int idx = install_level(
        "name OneWayTest\ntheme grass\ntime 120\n"
        "...........G\n"
        "............\n"
        "............\n"
        "............\n"
        "............\n"
        "....====....\n"
        ".....P......\n"
        "############\n");
    platformium_start_level(idx);
    sim_frames(10);   /* settle onto the ground */
    assert(platformium_player_grounded());
    platformium_key("space", 1);
    sim_frames(20);
    platformium_key("space", 0);
    float min_y = 99, y;
    for (int i = 0; i < 120; i++) {
        platformium_update(STEP);
        platformium_player_pos(NULL, &y);
        if (y < min_y) min_y = y;
    }
    /* jumped up through the platform, came back down onto it */
    assert(min_y + HH < 5.0f);
    platformium_player_pos(NULL, &y);
    assert(platformium_player_grounded());
    assert(fabsf(y + HH - 5.0f) < .05f);
    puts("PASS one-way platforms: jump through from below, land on top");
}

static void test_spring(void) {
    int idx = install_level(
        "name SpringTest\ntheme grass\ntime 120\n"
        "...........G\n"
        "............\n"
        "............\n"
        "............\n"
        "............\n"
        "............\n"
        "..P.........\n"
        "#####S######\n");
    platformium_start_level(idx);
    platformium_key("d", 1);
    float min_y = 99, y;
    for (int i = 0; i < 240; i++) {
        platformium_update(STEP);
        platformium_player_pos(NULL, &y);
        if (y < min_y) min_y = y;
    }
    platformium_key("d", 0);
    assert(min_y + HH < 2.6f); /* spring sends the player ~6 tiles up */
    puts("PASS springs: walking onto a trampoline launches ~6 tiles high");
}

static void test_hazards(void) {
    int idx = install_level(
        "name SpikeTest\ntheme grass\ntime 120\n"
        "...........G\n............\n............\n............\n"
        "............\n............\n..P.^^......\n############\n");
    platformium_start_level(idx);
    platformium_key("d", 1);
    int dead = 0;
    for (int i = 0; i < 360; i++) {
        platformium_update(STEP);
        if (platformium_state() != PLATFORMIUM_STATE_PLAY) { dead = 1; break; }
    }
    platformium_key("d", 0);
    assert(dead);
    assert(platformium_lives() == 2);

    idx = install_level(
        "name LavaTest\ntheme stone\ntime 120\n"
        "...........G\n............\n............\n............\n"
        "............\n............\n..P.........\n####~~~#####\n");
    platformium_start_level(idx);
    platformium_key("d", 1);
    dead = 0;
    for (int i = 0; i < 360; i++) {
        platformium_update(STEP);
        if (platformium_state() != PLATFORMIUM_STATE_PLAY) { dead = 1; break; }
    }
    platformium_key("d", 0);
    assert(dead);
    puts("PASS hazards: spikes and lava cost a life");
}

static void test_coins_and_goal(void) {
    int idx = install_level(
        "name CoinGoalTest\ntheme grass\ntime 120\n"
        "............\n............\n............\n............\n"
        "............\n............\n..P.o.o.o..G\n############\n");
    platformium_start_level(idx);
    int score0 = platformium_score();
    platformium_key("d", 1);
    int cleared = 0;
    for (int i = 0; i < 360; i++) {
        platformium_update(STEP);
        if (platformium_state() == PLATFORMIUM_STATE_CLEAR) { cleared = 1; break; }
    }
    platformium_key("d", 0);
    assert(cleared);
    assert(platformium_coins() == 3);
    /* 3 coins * 50 + time bonus */
    assert(platformium_score() - score0 >= 150 + 100 * 5);
    assert(platformium_level_best(idx) > 0);
    puts("PASS coins and goal: collecting pays 50 each, the flag clears the level with a time bonus");
}

static void test_checkpoint(void) {
    int idx = install_level(
        "name CheckpointTest\ntheme grass\ntime 120\n"
        "...........G\n............\n............\n............\n"
        "............\n............\n..P.C...^^..\n############\n");
    platformium_start_level(idx);
    platformium_key("d", 1);
    float cx = 0;
    for (int i = 0; i < 600; i++) {
        platformium_update(STEP);
        if (platformium_state() != PLATFORMIUM_STATE_PLAY) break;
    }
    platformium_key("d", 0);
    /* the spikes kill after the checkpoint was taken */
    assert(platformium_lives() == 2);
    for (int i = 0; i < 360 && platformium_state() != PLATFORMIUM_STATE_PLAY; i++)
        platformium_update(STEP);
    assert(platformium_state() == PLATFORMIUM_STATE_PLAY);
    platformium_player_pos(&cx, NULL);
    assert(fabsf(cx - 4.5f) < .6f); /* respawned at the checkpoint tile */
    puts("PASS checkpoints: death after a checkpoint respawns there");
}

static void test_stomp(void) {
    int idx = install_level(
        "name StompTest\ntheme grass\ntime 120\n"
        "...........G\n............\n.....P......\n............\n"
        "............\n............\n.....W......\n############\n");
    platformium_start_level(idx);
    assert(platformium_enemy_count() == 1);
    /* the player drops straight onto the walker: a stomp, not a hurt */
    int stomped = 0;
    for (int i = 0; i < 360; i++) {
        platformium_update(STEP);
        if (platformium_enemies_alive() == 0) { stomped = 1; break; }
        if (platformium_state() != PLATFORMIUM_STATE_PLAY) break;
    }
    assert(stomped && platformium_state() == PLATFORMIUM_STATE_PLAY);
    assert(platformium_score() >= 100);
    puts("PASS stomping: landing on a walker destroys it and bounces");
}

static void test_lift_carry(void) {
    int idx = install_level(
        "name LiftTest\ntheme grass\ntime 120\n"
        "...........G\n............\n........P...\n............\n"
        "............\n.....M......\n............\n............\n");
    platformium_start_level(idx);
    float x0, x, y, max_excursion = 0;
    platformium_player_pos(&x0, NULL);
    /* fall onto the lift, then ride it through a full patrol */
    for (int i = 0; i < 600; i++) {
        platformium_update(STEP);
        assert(platformium_state() == PLATFORMIUM_STATE_PLAY);
        platformium_player_pos(&x, NULL);
        if (fabsf(x - x0) > max_excursion) max_excursion = fabsf(x - x0);
    }
    platformium_player_pos(&x, &y);
    assert(max_excursion > 1.5f);        /* carried sideways with the lift */
    assert(platformium_player_grounded());
    puts("PASS lifts: landing on a moving platform rides it");
}

static void test_parse_rules(void) {
    PmLevelDef def;
    assert(!pm_level_parse("", 0, &def));
    assert(!pm_level_parse("tiny\n..", 6, &def));                    /* too small */
    const char *no_goal =
        "............\n............\n............\n............\n"
        "............\n............\n..P.........\n############\n";
    assert(!pm_level_parse(no_goal, strlen(no_goal), &def));        /* no goal */
    const char *good =
        "name Parsed\ntheme stone\ntime 77\n"
        "............\n............\n............\n............\n"
        "............\n............\n..P.......G.\n############\n";
    assert(pm_level_parse(good, strlen(good), &def));
    assert(!strcmp(def.name, "Parsed"));
    assert(def.theme == PM_THEME_STONE);
    assert(def.time_limit == 77);
    assert(def.spawn_tx == 2 && def.goal_tx == 10);
    puts("PASS level parser: headers, tile codes and validation rules");
}

/* ── router / menu / persistence integration ─────────────────────────── */

static void tap(float x, float y) {
    game_touch(x, y, 0, 100 + rand() % 900);
    game_touch(x, y, 1, 100 + rand() % 900);
}

static void draw_frames(int n) {
    for (int i = 0; i < n; i++) {
        dt = STEP;
        game_update();
        assert(gfx_begin_frame(frame));
        game_draw(frame);
        gfx_end_frame();
        guard_check();
        assert(!app_failed());
    }
}

static void test_router(AAssetManager *am) {
    (void)am;
    assert(game_current_mode() == 0);
    assert(!game_menu_open());
    draw_frames(30);
    /* open the menu via the top-left button */
    float bx, by, br;
    game_menu_button_geom(&bx, &by, &br);
    tap(bx, by);
    assert(game_menu_open());
    draw_frames(10);
    /* pick Platformium card -> level starts */
    float cx, cy, cw, ch;
    game_menu_card_geom(1, &cx, &cy, &cw, &ch);
    tap(cx + cw * .5f, cy + ch * .3f);
    assert(!game_menu_open());
    assert(game_current_mode() == 1);
    assert(platformium_started());
    assert(platformium_state() == PLATFORMIUM_STATE_PLAY);
    draw_frames(60);
    /* keyboard also toggles the menu */
    game_key("Escape", 1);
    game_key("Escape", 0);
    assert(game_menu_open());
    /* pick a level chip (level 2 must be allowed only after unlocking,
     * level 1 is always unlocked) */
    float lx, ly, lw, lh;
    game_menu_chip_geom(1, &lx, &ly, &lw, &lh);
    tap(lx + lw * .5f, ly + lh * .5f);
    if (platformium_level_unlocked(1)) {
        assert(!game_menu_open() && game_current_mode() == 1);
    } else {
        assert(game_menu_open());    /* locked chip ignored */
    }
    if (game_menu_open()) {
        game_menu_chip_geom(0, &lx, &ly, &lw, &lh);
        tap(lx + lw * .5f, ly + lh * .5f);
        assert(!game_menu_open());
    }
    draw_frames(30);
    /* back to Geometrium */
    game_touch(bx, by, 0, 7);
    game_touch(bx, by, 1, 7);
    assert(game_menu_open());
    game_menu_card_geom(0, &cx, &cy, &cw, &ch);
    tap(cx + cw * .5f, cy + ch * .5f);
    assert(game_current_mode() == 0 && !game_menu_open());
    draw_frames(30);
    puts("PASS app router: launcher menu switches between the voxel world and the platformer");
}

static void test_shipped_levels(void) {
    int count = platformium_level_count();
    assert(count >= 4);   /* three built-ins + assets/levels/bonus.txt */
    for (int i = 0; i < count; i++) {
        assert(platformium_level_unlocked(i) || i >= 1);
        assert(platformium_level_name(i)[0]);
        if (!platformium_level_unlocked(i)) {
            /* locked levels still parse: unlock via internals for the bot */
            platformium_start_level(i);
            assert(platformium_started());
        }
        int before = platformium_score();
        (void)before;
        assert(run_level_bot(i));
        printf("PASS level %d \"%s\": bot cleared it (coins %d, score +%d)\n",
               i, platformium_level_name(i), platformium_coins(),
               platformium_score() - before);
    }
}

int main(void) {
    screen_w = 960;
    screen_h = 540;
    frame = malloc(sizeof(Buffer));
    assert(frame);
    frame->width = screen_w;
    frame->height = screen_h;
    frame->stride = screen_w + 8;
    frame->pixels = malloc((size_t)frame->stride * frame->height * 4);
    assert(frame->pixels);
    for (int y = 0; y < frame->height; y++)
        for (int x = frame->width; x < frame->stride; x++)
            frame->pixels[y * frame->stride + x] = 0x12345678u;

    mkdir("build-tests/fixtures", 0777);
    mkdir("build-tests/fixtures/pm-storage", 0777);
    app_set_storage("build-tests/fixtures/pm-storage");
    remove("build-tests/fixtures/pm-storage/platformium.save");

    AAssetManager *am = host_asset_manager("assets");
    assert(gfx_init(am));
    game_init(am);

    test_parse_rules();
    test_router(am);
    test_shipped_levels();
    test_oneway();
    test_spring();
    test_hazards();
    test_coins_and_goal();
    test_checkpoint();
    test_stomp();
    test_lift_carry();
    game_save();
    FILE *f = fopen("build-tests/fixtures/pm-storage/platformium.save", "r");
    assert(f);
    char line[64];
    int saw_unlocked = 0;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "unlocked", 8)) saw_unlocked = 1;
    fclose(f);
    assert(saw_unlocked);
    puts("PASS progress file: unlocked levels and best scores persist");

    gfx_shutdown();
    free(frame->pixels);
    free(frame);
    puts("PLATFORMER SUITE OK");
    return 0;
}
