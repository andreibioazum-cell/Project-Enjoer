/* Regression test for the C game / C++ renderer boundary. */
#include "engine.h"
#include "vulkan_cube.h"
#include "dimscript_runtime.h"
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

int main(void) {
    screen_w = 320;
    screen_h = 240;
    Buffer frame = {0};
    frame.width = frame.stride = screen_w;
    frame.height = screen_h;
    frame.pixels = (uint32_t *)calloc((size_t)frame.stride * frame.height, sizeof(uint32_t));
    CHECK(frame.pixels);

    game_init(NULL);
    CHECK(!app_failed());
    CHECK(cube_renderer_backend() && cube_renderer_backend()[0] != '\0');
    dt = 1.0 / 60.0;
    game_update();
    game_draw(&frame);
    const unsigned long first = checksum(&frame);
    CHECK(first != 0);
    CHECK(ds_render_text_count() == 3); /* font backend is intentionally absent */

    game_key("ArrowLeft", 1);
    game_draw(&frame);
    CHECK(checksum(&frame) != first);
    game_key("r", 1);
    game_key("r", 0);
    game_touch(160, 120, 0, 7);
    game_touch(200, 130, 2, 7);
    game_touch(200, 130, 1, 7);
    game_cancel_input();
    game_resize(240, 320);
    CHECK(screen_w == 240 && screen_h == 320);
    game_shutdown();
    free(frame.pixels);
    puts("PASS C game state + C++ cube renderer + resize/input boundary");
    return 0;
}
