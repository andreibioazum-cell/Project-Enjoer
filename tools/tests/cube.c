/* Regression test for the C game / C++ renderer boundary. */
#define _POSIX_C_SOURCE 200809L
#include "engine.h"
#include "vulkan_cube.h"
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

static unsigned long checksum565(const Buffer *frame) {
    const uint16_t *pixels = (const uint16_t *)frame->pixels;
    unsigned long result = 2166136261u;
    for (int y = 0; y < frame->height; ++y)
        for (int x = 0; x < frame->width; ++x)
            result = (result ^ pixels[y * frame->stride + x]) * 16777619u;
    return result;
}

/* The CPU path phones use when Vulkan is missing: rasterize small, stretch up. */
static int check_software_backend(void) {
    Buffer scaled = {0};
    scaled.width = 200;
    scaled.height = 150;
    scaled.stride = 200;
    scaled.format = ENJOER_BUFFER_FORMAT_RGBA8888;
    scaled.pixels = (uint32_t *)calloc((size_t)scaled.stride * scaled.height, sizeof(uint32_t));
    CHECK(scaled.pixels);
    CHECK(cube_software_render(&scaled, 0.4f, -0.1f));
    CHECK(checksum(&scaled) != 0);
    /* Top-left corner is the sky gradient: r=25, g=45, b=90, opaque. */
    CHECK(scaled.pixels[0] == 0xff5a2d19u);

    /* A padded stride must not shift or shear the image. */
    Buffer padded = {0};
    padded.width = 200;
    padded.height = 150;
    padded.stride = 213;
    padded.format = ENJOER_BUFFER_FORMAT_RGBA8888;
    padded.pixels = (uint32_t *)calloc((size_t)padded.stride * padded.height, sizeof(uint32_t));
    CHECK(padded.pixels);
    CHECK(cube_software_render(&padded, 0.4f, -0.1f));
    for (int y = 0; y < padded.height; ++y)
        for (int x = 0; x < padded.width; ++x)
            CHECK(padded.pixels[y * padded.stride + x] ==
                  scaled.pixels[y * scaled.stride + x]);

    /* A phone-sized window is downscaled and stretched back: no gaps. */
    Buffer big = {0};
    big.width = 900;
    big.height = 600;
    big.stride = 900;
    big.format = ENJOER_BUFFER_FORMAT_RGBA8888;
    big.pixels = (uint32_t *)malloc((size_t)big.stride * big.height * sizeof(uint32_t));
    CHECK(big.pixels);
    for (int i = 0; i < big.stride * big.height; ++i) big.pixels[i] = 0xdeadbeefu;
    CHECK(cube_software_render(&big, 0.4f, -0.1f));
    int leftovers = 0;
    for (int i = 0; i < big.stride * big.height; ++i)
        if (big.pixels[i] == 0xdeadbeefu) ++leftovers;
    CHECK(leftovers == 0);

    /* Windows that only offer a 16-bit surface get RGB565. */
    Buffer rgb565 = {0};
    rgb565.width = 200;
    rgb565.height = 150;
    rgb565.stride = 200;
    rgb565.format = ENJOER_BUFFER_FORMAT_RGB565;
    rgb565.pixels = (uint32_t *)calloc((size_t)rgb565.stride * rgb565.height, sizeof(uint16_t));
    CHECK(rgb565.pixels);
    CHECK(cube_software_render(&rgb565, 0.4f, -0.1f));
    const uint16_t *low = (const uint16_t *)rgb565.pixels;
    CHECK(low[0] == (uint16_t)(((25 >> 3) << 11) | ((45 >> 2) << 5) | (90 >> 3)));
    const unsigned long first565 = checksum565(&rgb565);
    int varied = 0;
    for (int i = 1; i < rgb565.stride * rgb565.height; ++i)
        if (low[i] != low[0]) { varied = 1; break; }
    CHECK(varied);
    CHECK(cube_software_render(&rgb565, 1.2f, -0.1f));
    CHECK(checksum565(&rgb565) != first565);

    free(scaled.pixels);
    free(padded.pixels);
    free(big.pixels);
    free(rgb565.pixels);
    return 0;
}

int main(void) {
    screen_w = 320;
    screen_h = 240;
    Buffer frame = {0};
    frame.width = frame.stride = screen_w;
    frame.height = screen_h;
    frame.format = ENJOER_BUFFER_FORMAT_RGBA8888;
    frame.pixels = (uint32_t *)calloc((size_t)frame.stride * frame.height, sizeof(uint32_t));
    CHECK(frame.pixels);

    /* Devices without a Vulkan driver ask for the CPU rasterizer explicitly. */
    setenv("ENJOER_RENDERER", "software", 1);
    game_init(NULL);
    CHECK(!app_failed());
    CHECK(cube_renderer_backend() && cube_renderer_backend()[0] != '\0');
    CHECK(cube_renderer_software_active() == 1);
    CHECK(strstr(cube_renderer_backend(), "CPU") != NULL);
    dt = 1.0 / 60.0;
    game_update();
    game_draw(&frame);
    const unsigned long first = checksum(&frame);
    CHECK(first != 0);

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

    if (check_software_backend()) return 1;

    game_shutdown();
    free(frame.pixels);
    puts("PASS C game state + C++ cube renderer + resize/input/CPU-fallback boundary");
    return 0;
}
