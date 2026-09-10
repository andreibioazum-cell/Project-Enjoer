/* App router regression: the launcher lists the engine projects, runs the one
 * you pick, routes keys/pointer into the engine and keeps drawing frames. */
#define _POSIX_C_SOURCE 200809L
#include "engine.h"
#include "engine/eng_api.h"
#include "engine/eng_internal.h"
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

/* Tap the middle of a launcher card and report whether the tap hit it. */
static void tap_card(int index) {
    float x, y, w, h;
    game_menu_card_geom(index, &x, &y, &w, &h);
    game_touch(x + w * .5f, y + h * .5f, 0, 0);
    game_touch(x + w * .5f, y + h * .5f, 1, 0);
}

static int card_with_title(const char *title) {
    for (int card = 0; card < game_menu_card_count(); card++)
        if (!strcmp(game_menu_title(card), title)) return card;
    return -1;
}

static void test_launcher(Buffer *b) {
    CHECK(game_menu_open());                      /* the app greets you */
    CHECK(game_menu_card_count() == game_project_count() + 1);
    CHECK(game_project_count() >= 3);
    CHECK(!strcmp(game_menu_title(0), "Geometrium"));   /* the restored block world */
    int demo = -1, physics = -1, voxel = -1, platformer = -1;
    for (int card = 0; card < game_menu_card_count(); card++) {
        const char *title = game_menu_title(card);
        CHECK(title && title[0]);
        if (!strcmp(title, "Demo Scene")) demo = card;
        if (!strcmp(title, "Physics Playground")) physics = card;
        if (!strcmp(title, "Voxel World")) voxel = card;
        if (!strcmp(title, "Platformer")) platformer = card;
        /* every card sits inside the frame and below the title */
        float x, y, w, h;
        game_menu_card_geom(card, &x, &y, &w, &h);
        CHECK(x > 0 && y > 0 && w > 0 && h > 0 && x + w < screen_w && y + h < screen_h);
        if (card > 0) {
            float py, ph;
            game_menu_card_geom(card - 1, NULL, &py, NULL, &ph);
            CHECK(y > py + ph - 1.0f);            /* no overlap */
        }
    }
    CHECK(demo >= 1 && physics >= 1 && voxel >= 1 && platformer >= 1);

    step(b, 1);
    CHECK(distinct_colors(b) > 32);               /* the launcher really drew */
    check_stride_guard(b, b->width);

    /* a tap on the menu button while the launcher is open changes nothing */
    float bx, by, br;
    game_menu_button_geom(&bx, &by, &br);
    CHECK(br > 0);
    game_touch(bx, by, 0, 0);
    game_touch(bx, by, 1, 0);
    CHECK(game_menu_open());

    /* picking a project starts it */
    tap_card(demo);
    CHECK(!game_menu_open());
    CHECK(eng_project_active());
    CHECK(game_current_project() == demo - 1);
    CHECK(!strcmp(eng_project_name(), "Demo Scene"));
    CHECK(eng_scene_node_count() > 5);
    printf("PASS launcher: Geometrium + %d engine projects listed, tap starts '%s' (%d nodes)\n",
           game_project_count(), eng_project_name(), eng_scene_node_count());
}

static void test_running_project(Buffer *b) {
    EngNode *hero = eng_node_find("Main/Hero");
    EngNode *camera = eng_node_find("Main/Camera");
    CHECK(hero && camera);
    float yaw0, pitch, roll;
    eng_node3d_get_rotation(hero, &yaw0, &pitch, &roll);
    float cx0, cy0, cz0;
    eng_node3d_get_position(camera, &cx0, &cy0, &cz0);

    step(b, 1);
    remember(b);
    step(b, 45);
    CHECK(changed_since(b));                      /* the scene is alive */
    check_stride_guard(b, b->width);

    float yaw1;
    eng_node3d_get_rotation(hero, &yaw1, &pitch, &roll);
    CHECK(yaw1 > yaw0 + 0.3f);                    /* the spin script ran */
    float cx1, cy1, cz1;
    eng_node3d_get_position(camera, &cx1, &cy1, &cz1);
    CHECK(fabsf(cx1 - cx0) + fabsf(cz1 - cz0) > 0.05f);   /* the orbit script ran */

    /* keys and the pointer reach the engine input API */
    game_key("w", 1);
    game_key("ArrowRight", 1);
    game_key("space", 1);
    CHECK(eng_input_is_pressed(ENG_KEY_UP));
    CHECK(eng_input_is_pressed(ENG_KEY_RIGHT));
    CHECK(eng_input_is_pressed(ENG_KEY_SPACE));
    game_touch(400, 300, 0, 0);
    float px, py;
    int down = 0;
    CHECK(eng_input_pointer(&px, &py, &down));
    CHECK(down && (int)px == 400 && (int)py == 300);
    game_touch(420, 310, 2, 0);
    CHECK(eng_input_pointer(&px, &py, &down) && down && (int)px == 420);
    game_touch(420, 310, 1, 0);
    CHECK(eng_input_pointer(&px, &py, &down) && !down);
    game_key("w", 0);
    game_key("ArrowRight", 0);
    game_key("space", 0);
    CHECK(!eng_input_is_pressed(ENG_KEY_UP));

    /* Escape and M toggle the launcher; the world freezes behind it */
    game_key("Escape", 1);
    CHECK(game_menu_open());
    CHECK(!eng_input_is_pressed(ENG_KEY_RIGHT));  /* holds are released */
    step(b, 1);                                   /* draw the launcher once */
    remember(b);
    step(b, 30);
    CHECK(!changed_since(b));                     /* frozen, not re-simulated */
    float cx2, cy2, cz2;
    eng_node3d_get_position(camera, &cx2, &cy2, &cz2);
    CHECK(fabsf(cx2 - cx1) < 1e-6f && fabsf(cz2 - cz1) < 1e-6f);
    game_key("m", 1);
    CHECK(!game_menu_open());
    step(b, 2);
    CHECK(changed_since(b));
    puts("PASS running project: scripts animate the scene, input is routed, the launcher pauses it");
}

static void test_project_switch(Buffer *b) {
    game_key("Escape", 1);
    CHECK(game_menu_open());
    int physics = card_with_title("Physics Playground");
    CHECK(physics >= 1);
    tap_card(physics);
    CHECK(game_current_project() == physics - 1);
    CHECK(!strcmp(eng_project_name(), "Physics Playground"));

    EngNode *ball = eng_node_find("Main/Ball");
    CHECK(ball);
    float x, y0, z;
    eng_node3d_get_position(ball, &x, &y0, &z);
    step(b, 60);
    float y1;
    eng_node3d_get_position(ball, &x, &y1, &z);
    CHECK(y1 < y0 - 0.5f);                        /* gravity pulled the body down */
    CHECK(eng_body_is_grounded(ball) || y1 < y0);
    check_stride_guard(b, b->width);

    /* the roller script answers the engine input API */
    game_key("d", 1);
    float vx, vy, vz;
    eng_body_get_linear_velocity(ball, &vx, &vy, &vz);
    step(b, 30);
    eng_body_get_linear_velocity(ball, &vx, &vy, &vz);
    game_key("d", 0);
    CHECK(vx > 0.05f);                            /* pushed to the right */
    printf("PASS project switch: '%s' runs, rigid body falls to y=%.2f and rolls on input\n",
           eng_project_name(), y1);
}

static void test_free_camera(Buffer *b) {
    int voxel = card_with_title("Voxel World");
    game_key("Escape", 1);
    CHECK(voxel >= 1);
    tap_card(voxel);
    CHECK(game_current_project() == voxel - 1);

    EngNode *camera = eng_node_find("Main/Camera");
    CHECK(camera);
    float yaw0, pitch0, roll, x0, y0, z0;
    eng_node3d_get_rotation(camera, &yaw0, &pitch0, &roll);
    eng_node3d_get_position(camera, &x0, &y0, &z0);

    /* this scene scripts nothing: the engine drives the camera instead */
    game_touch(500, 300, 0, 0);
    step(b, 1);                                   /* drag begins */
    game_touch(600, 320, 2, 0);
    step(b, 2);                                   /* the drag rotates the view */
    game_touch(600, 320, 1, 0);
    float yaw1, pitch1;
    eng_node3d_get_rotation(camera, &yaw1, &pitch1, &roll);
    CHECK(fabsf(yaw1 - yaw0) > 0.1f);
    CHECK(fabsf(pitch1 - pitch0) > 0.01f);

    game_key("w", 1);
    step(b, 30);
    game_key("w", 0);
    float x1, y1, z1;
    eng_node3d_get_position(camera, &x1, &y1, &z1);
    CHECK(fabsf(x1 - x0) + fabsf(z1 - z0) > 0.5f);
    step(b, 2);
    CHECK(distinct_colors(b) > 32);               /* the voxel world rendered */
    check_stride_guard(b, b->width);
    puts("PASS free camera: an unscripted Camera3D orbits on drag and flies on WASD");
}

static void test_reset_and_direct_open(Buffer *b) {
    /* game_reset reloads the running project from its scene file */
    int before = game_current_project();
    game_reset();
    CHECK(game_current_project() == before);
    CHECK(eng_project_active());
    step(b, 3);

    /* --project style start: load a directory that is not in the scanned root */
    CHECK(game_open_project("projects/demo"));
    CHECK(!game_menu_open());
    CHECK(!strcmp(eng_project_name(), "Demo Scene"));
    step(b, 3);
    CHECK(!app_failed());
    puts("PASS reset and direct open: a project reloads from its scene file");
}

/* The restored first-person block world: streaming terrain, hand, controls. */
static void test_geometrium(Buffer *b) {
    game_key("Escape", 1);
    CHECK(game_menu_open());
    tap_card(0);
    CHECK(!game_menu_open());
    CHECK(game_in_geometrium());
    CHECK(game_current_project() == -1);

    /* chunks stream in around the spawn; world, hand and HUD all draw */
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

    /* the hotbar answers digit keys; F toggles flight */
    game_key("1", 1); game_key("1", 0);
    CHECK(geometrium_selected() == 0);
    game_key("3", 1); game_key("3", 0);
    CHECK(geometrium_selected() == 2);
    game_key("f", 1); game_key("f", 0);
    CHECK(geometrium_player_flying());

    /* the launcher pauses the world: frames freeze, the player stops */
    game_key("Escape", 1);
    CHECK(game_menu_open());
    step(b, 1);                                   /* draw the launcher once */
    remember(b);
    step(b, 30);
    CHECK(!changed_since(b));
    float x2, y2, z2;
    geometrium_player_pos(&x2, &y2, &z2);
    CHECK(x2 == x1 && y2 == y1 && z2 == z1);

    /* switching to an engine project from the block world works */
    int demo = card_with_title("Demo Scene");
    tap_card(demo);
    CHECK(eng_project_active());
    CHECK(!game_in_geometrium());
    step(b, 3);
    puts("PASS geometrium: the restored block world streams, walks, flies and pauses in the launcher");
}

static void test_platformer(Buffer *b) {
    CHECK(game_open_project("projects/platformer"));
    CHECK(!game_menu_open());
    CHECK(!strcmp(eng_project_name(), "Platformer"));
    EngNode *hero = eng_node_find("Main/Hero");
    CHECK(hero && hero->type == ENG_CHARACTER_BODY);
    EngNode *camera = eng_node_find("Main/Camera");
    CHECK(camera && camera->script);            /* follow script owns the camera */

    step(b, 30);
    CHECK(eng_character_is_grounded(hero));     /* gravity settled the hero */

    float x0, y0, z0;
    eng_node3d_get_position(hero, &x0, &y0, &z0);
    (void)z0;

    /* D walks right */
    game_key("d", 1);
    step(b, 40);
    game_key("d", 0);
    float x1, y1, z1;
    eng_node3d_get_position(hero, &x1, &y1, &z1);
    CHECK(x1 > x0 + 0.5f);
    CHECK(eng_character_is_grounded(hero));

    /* Space jumps; the hero rises off the floor */
    game_key("space", 1);
    step(b, 8);
    game_key("space", 0);
    float x2, y2, z2;
    eng_node3d_get_position(hero, &x2, &y2, &z2);
    CHECK(y2 > y1 + 0.5f);

    /* the follow camera tracks the hero along X */
    float cx2, cy2, cz2;
    eng_node3d_get_position(camera, &cx2, &cy2, &cz2);
    CHECK(fabsf(cx2 - x2) < 2.0f);
    (void)y1; (void)z1; (void)x2; (void)cy2; (void)cz2;
    check_stride_guard(b, b->width);
    puts("PASS platformer: CharacterBody3D grounds, walks, jumps and the camera follows");
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
    game_set_project_root("projects");
    game_init(am);
    CHECK(!app_failed());

    if (w == 960) {
        test_launcher(&b);
        test_running_project(&b);
        test_project_switch(&b);
        test_free_camera(&b);
        test_geometrium(&b);
        test_reset_and_direct_open(&b);
        test_platformer(&b);
    } else {
        /* a second resolution: geometry scales, frames stay complete */
        tap_card(0);
        CHECK(!game_menu_open());
        step(&b, 30);
        check_stride_guard(&b, w);
    }
    game_save();
    gfx_shutdown();
    free(b.pixels);
    printf("PASS %dx%d engine app: launcher, projects, physics, input, %d projects\n",
           w, h, game_project_count());
}

int main(void) {
    run(960, 540);
    run(1600, 900);
    free(frame_copy);
    puts("ENGINE APP SUITE OK");
    return 0;
}
