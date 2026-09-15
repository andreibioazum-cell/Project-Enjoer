/* Regression test for the C game / C++ renderer boundary.
 *
 * It runs the engine twice in one process: first with no game folder, which
 * must fall back to the ahead-of-time compiled clicker in src/generated, then
 * with games/brick, which must load three .ds files through the VM.  Both runs
 * go through the real draw path, so the batch the Vulkan pipeline eats on a
 * device is checked here as well. */
#include "engine.h"
#include "vulkan_cube.h"
#include "dimscript_runtime.h"
#include "enjoer_draw.h"
#include "ds_manifest.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(value) do { \
    if (!(value)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #value); return 1; } \
} while (0)

static unsigned long checksum(const Buffer *frame) {
    unsigned long result = 2166136261u;
    for (int y = 0; y < frame->height; ++y)
        for (int x = 0; x < frame->width; ++x)
            result = (result ^ frame->pixels[y * frame->stride + x]) * 16777619u;
    return result;
}

/* A frame of a real game has to be non-empty in both halves of the batch: text
 * for the (still missing) font backend and triangles for everything else. */
static int frame_has(const EnjoerFrame *frame, const char *needle) {
    for (int index = 0; index < frame->text_count; ++index)
        if (strstr(frame->texts[index].text, needle)) return 1;
    return 0;
}

static int run_compiled_clicker(Buffer *frame) {
    screen_w = 320;
    screen_h = 240;
    frame->width = frame->stride = screen_w;
    frame->height = screen_h;
    game_init(NULL);
    CHECK(!app_failed());
    CHECK(!game_is_interpreted());
    CHECK(cube_renderer_backend() && cube_renderer_backend()[0] != '\0');
    dt = 1.0 / 60.0;
    game_update();
    game_draw(frame);
    const unsigned long first = checksum(frame);
    CHECK(first != 0);
    CHECK(ds_render_text_count() == 3); /* font backend is intentionally absent */
    CHECK(enjoer_frame()->text_count == 3);

    game_key("ArrowLeft", 1);
    game_draw(frame);
    CHECK(checksum(frame) != first);
    game_key("r", 1);
    game_key("r", 0);
    game_touch(160, 120, 0, 7);
    game_draw(frame);
    CHECK(frame_has(enjoer_frame(), "Счет: 1")); /* the tap reached the script */
    game_touch(200, 130, 2, 7);
    game_touch(200, 130, 1, 7);
    game_cancel_input();
    game_resize(240, 320);
    CHECK(screen_w == 240 && screen_h == 320);
    game_shutdown();
    return 0;
}

static int run_interpreted_game(Buffer *frame) {
    game_set_game_dir("games/brick");
    screen_w = 640;
    screen_h = 360;
    frame->width = frame->stride = screen_w;
    frame->height = screen_h;
    game_init(NULL);
    CHECK(game_is_interpreted());
    const DsGameManifest *manifest = game_manifest();
    CHECK(manifest && manifest->script_count == 3);
    CHECK(!strcmp(game_title(), "Кирпич"));
    CHECK(!strcmp(manifest->package, "com.cb4.brick"));

    dt = 1.0 / 60.0;
    for (int index = 0; index < 3; ++index) {
        game_update();
        game_draw(frame);
    }
    CHECK(!game_script_failed());
    CHECK(frame_has(enjoer_frame(), "Кирпич"));
    CHECK(enjoer_frame()->vertex_count > 100); /* 66 bricks are more than a quad */
    const unsigned long playing = checksum(frame);
    game_touch(320, 300, 0, 3); /* start the ball */
    game_touch(320, 300, 1, 3);
    for (int index = 0; index < 20; ++index) {
        game_update();
        game_draw(frame);
    }
    CHECK(!game_script_failed());
    CHECK(checksum(frame) != playing);
    game_key("space", 1);
    game_key("space", 0);
    game_draw(frame);
    CHECK(enjoer_frame()->text_count > 0);
    game_resize(360, 640);
    game_update();
    game_draw(frame);
    CHECK(!game_script_failed());
    game_shutdown();
    return 0;
}

int main(void) {
    Buffer frame = {0};
    frame.pixels = (uint32_t *)calloc((size_t)320 * 240, sizeof(uint32_t));
    CHECK(frame.pixels);
    if (run_compiled_clicker(&frame)) return 1;
    /* The first run sized the buffer for 320x240; the game needs 640x360. */
    free(frame.pixels);
    frame.pixels = (uint32_t *)calloc((size_t)640 * 640, sizeof(uint32_t));
    CHECK(frame.pixels);
    if (run_interpreted_game(&frame)) return 1;
    free(frame.pixels);
    puts("PASS C game state + C++ cube renderer + compiled and interpreted games");
    return 0;
}
