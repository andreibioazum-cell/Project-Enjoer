/* Регрессии публичного API рендера; без Android, GPU и include .c. */
#include "rbx/rbx_render_internal.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int screen_w = 320, screen_h = 240;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)
#define PI 3.14159265358979323846f
#define GUARD 0x12345678u

static Buffer make_buffer(int w, int h, int pad) {
    Buffer b = {NULL, w, h, w + pad};
    b.pixels = malloc((size_t)b.stride * h * sizeof(*b.pixels));
    CHECK(b.pixels);
    for (int i = 0; i < b.stride * h; i++) b.pixels[i] = GUARD;
    return b;
}

static void check_padding(const Buffer *b) {
    for (int y = 0; y < b->height; y++)
        for (int x = b->width; x < b->stride; x++) CHECK(b->pixels[y * b->stride + x] == GUARD);
}

static int channel(uint32_t p, int i) { return (p >> (i * 8)) & 255; }

static void test_color_fog(void) {
    Buffer b = make_buffer(320, 240, 7);
    const float distances[] = {10, 27.9f, 28.12f, 28.2f, 40, 60, 64, 70, 76};
    const uint32_t colors[] = {0xFFFFD600u, 0xFFFF00FFu, 0xFFFFFFFFu, 0xFF0000FFu};
    const int fog[] = {110, 197, 247};
    for (int sc = 1; sc <= 3; sc++) {
        for (unsigned c = 0; c < sizeof(colors) / sizeof(*colors); c++) {
            for (unsigned i = 0; i < sizeof(distances) / sizeof(*distances); i++) {
                float z = distances[i];
                CHECK(rbx3d_begin(&b, sc, 0, 0, 0, 0, 0, 72));
                rbx3d_sky(0xFF6EC5F7u, 0xFF6EC5F7u);
                rbx3d_box(0, 0, z, 4, 4, .12f, 0, colors[c]);
                rbx3d_end();
                uint32_t pixel = b.pixels[120 * b.stride + 160];
                float t = fmaxf(0, fminf(1, (z - .12f - RBX_FOG_START) / (RBX_FOG_END - RBX_FOG_START)));
                for (int ch = 0; ch < 3; ch++) {
                    int lit = (int)(((colors[c] >> (16 - ch * 8)) & 255) * .52f);
                    int expected = (int)(lit + (fog[ch] - lit) * t);
                    CHECK(abs(channel(pixel, ch) - expected) <= 1);
                }
                CHECK((pixel >> 24) == 255);
            }
        }
    }
    check_padding(&b);
    free(b.pixels);
    puts("PASS fog: bright colors stay bounded at all distances/scales");
}

static void pattern(Buffer *b, int scale) {
    CHECK(rbx3d_begin(b, scale, 0, 0, 0, .15f, -.12f, 72));
    rbx3d_sky(0xFFBBDDEFu, 0xFF284658u);
    const uint32_t colors[] = {0xFFFFFFFFu, 0xFF000000u, 0xFFFFD600u, 0xFF0000FFu};
    for (int i = 0; i < 4; i++)
        rbx3d_box(-2.1f + i * 1.4f, 0, 6 + i * .3f, .55f, 1.4f, .3f, i * .31f, colors[i]);
    rbx3d_end();
}

static void test_upscale(void) {
    const int sizes[][2] = {{320, 240}, {319, 241}, {403, 811}};
    for (unsigned k = 0; k < sizeof(sizes) / sizeof(*sizes); k++) {
        for (int scale = 2; scale <= 3; scale++) {
            Buffer out = make_buffer(sizes[k][0], sizes[k][1], 9);
            Buffer src = make_buffer(out.width / scale, out.height / scale, 0);
            pattern(&src, 1);
            pattern(&out, scale);
            for (int y = 0; y < out.height; y++) {
                double fy = fmax(0, fmin(src.height - 1, (y + .5) * src.height / out.height - .5));
                int y0 = (int)fy, y1 = y0 + 1 < src.height ? y0 + 1 : y0;
                double ty = fy - y0;
                for (int x = 0; x < out.width; x++) {
                    double fx = fmax(0, fmin(src.width - 1, (x + .5) * src.width / out.width - .5));
                    int x0 = (int)fx, x1 = x0 + 1 < src.width ? x0 + 1 : x0;
                    double tx = fx - x0;
                    uint32_t actual = out.pixels[y * out.stride + x];
                    for (int c = 0; c < 3; c++) {
                        double a = channel(src.pixels[y0 * src.stride + x0], c);
                        double b = channel(src.pixels[y0 * src.stride + x1], c);
                        double d = channel(src.pixels[y1 * src.stride + x0], c);
                        double e = channel(src.pixels[y1 * src.stride + x1], c);
                        double top = a * (1 - tx) + b * tx, bot = d * (1 - tx) + e * tx;
                        int expected = (int)(top * (1 - ty) + bot * ty + .5);
                        CHECK(abs(channel(actual, c) - expected) <= 2);
                    }
                    CHECK((actual >> 24) == 255);
                }
            }
            check_padding(&out);
            free(src.pixels); free(out.pixels);
        }
    }
    puts("PASS bilinear upscale: matches signed reference, including odd sizes and stride");
}

typedef struct { float x, y, z, hx, hy, hz, yaw; uint32_t color; } Box;
static const Box red = {-.2f, 0, 8, 4, 3, .15f, .8f, 0xFFFF0000u};
static const Box blue = {.5f, 0, 9, 3, 2.6f, .12f, -.2f, 0xFF0000FFu};

static float ray_box(float dx, float dy, const Box *b) {
    float c = cosf(b->yaw), s = sinf(b->yaw);
    float o[] = {-b->x * c + b->z * s, -b->y, -b->x * s - b->z * c};
    float d[] = {dx * c - s, dy, dx * s + c};
    float h[] = {b->hx, b->hy, b->hz};
    float lo = .18f, hi = 110;
    for (int i = 0; i < 3; i++) {
        if (fabsf(d[i]) < 1e-8f) {
            if (fabsf(o[i]) > h[i]) return INFINITY;
        } else {
            float a = (-h[i] - o[i]) / d[i], e = (h[i] - o[i]) / d[i];
            lo = fmaxf(lo, fminf(a, e)); hi = fminf(hi, fmaxf(a, e));
            if (lo > hi) return INFINITY;
        }
    }
    return lo;
}

static int expected_object(float x, float y, int w, int h) {
    float foc = h * .5f / tanf(72 * PI / 360);
    float dx = (x + .5f - w * .5f) / foc, dy = (h * .5f - y - .5f) / foc;
    float r = ray_box(dx, dy, &red), b = ray_box(dx, dy, &blue);
    return isfinite(r) && r < b ? 1 : isfinite(b) ? 2 : 0;
}

static void draw_box(const Box *b) {
    rbx3d_box(b->x, b->y, b->z, b->hx, b->hy, b->hz, b->yaw, b->color);
}

static void test_perspective_depth(void) {
    Buffer out = make_buffer(256, 192, 3);
    int checked = 0;
    for (int reverse = 0; reverse < 2; reverse++) {
        CHECK(rbx3d_begin(&out, 1, 0, 0, 0, 0, 0, 72));
        rbx3d_sky(0xFF004000u, 0xFF004000u);
        draw_box(reverse ? &blue : &red); draw_box(reverse ? &red : &blue);
        rbx3d_end();
        for (int y = 4; y < out.height - 4; y++) {
            for (int x = 4; x < out.width - 4; x++) {
                int expected = expected_object(x, y, out.width, out.height);
                /* Пропускаем только субпиксельные границы силуэтов/пересечений. */
                if (expected != expected_object(x - .8f, y, out.width, out.height) ||
                    expected != expected_object(x + .8f, y, out.width, out.height) ||
                    expected != expected_object(x, y - .8f, out.width, out.height) ||
                    expected != expected_object(x, y + .8f, out.width, out.height)) continue;
                uint32_t pixel = out.pixels[y * out.stride + x];
                int actual = channel(pixel, 0) ? 1 : channel(pixel, 2) ? 2 : 0;
                CHECK(actual == expected);
                checked++;
            }
        }
    }
    CHECK(checked > 60000);
    check_padding(&out); free(out.pixels);
    puts("PASS reciprocal depth: slanted overlapping surfaces match ray intersections in both draw orders");
}

static void test_clipping_and_camera(void) {
    Buffer out = make_buffer(160, 120, 5);
    CHECK(rbx3d_begin(&out, 1, 0, 0, 0, 0, 0, 72));
    rbx3d_sky(0xFF6EC5F7u, 0xFF6EC5F7u);
    rbx3d_box(0, 0, -5, 1, 1, 1, 0, 0xFFFFFFFFu);
    rbx3d_box(0, 0, 130, 1, 1, 1, 0, 0xFFFFFFFFu);
    rbx3d_box(1000, 0, 5, 1, 1, 1, 0, 0xFFFFFFFFu);
    rbx3d_box(0, -1000, 5, 1, 1, 1, 0, 0xFFFFFFFFu);
    rbx3d_end();
    for (int y = 0; y < out.height; y++)
        for (int x = 0; x < out.width; x++) CHECK(out.pixels[y * out.stride + x] == 0xFFF7C56Eu);
    for (int i = 0; i < 120; i++) {
        CHECK(rbx3d_begin(&out, 1, 0, 0, 0, 0, (i - 60) * .024f, 72));
        rbx3d_sky(0xFF6EC5F7u, 0xFF6EC5F7u);
        rbx3d_box(0, 0, (i - 20) * .05f, .42f, .42f, .12f, i * .12f, 0xFFFFD600u);
        rbx3d_box(0, -2, 0, 32, .5f, 32, 0, 0xFF4CAF50u);
        rbx3d_end();
        check_padding(&out);
    }
    float yaw = .7f, pitch = .8f, sx, sy;
    CHECK(rbx3d_begin(&out, 1, 10, 6, -3, yaw, pitch, 72));
    CHECK(rbx3d_project(10 + sinf(yaw) * cosf(pitch) * 20, 6 + sinf(pitch) * 20,
                        -3 + cosf(yaw) * cosf(pitch) * 20, &sx, &sy));
    CHECK(fabsf(sx - out.width * .5f) < .001f && fabsf(sy - out.height * .5f) < .001f);
    Buffer invalid = out; invalid.stride = out.width - 1;
    CHECK(!rbx3d_begin(&invalid, 1, 0, 0, 0, 0, 0, 72));
    rbx3d_end(); /* не пишет старый кадр после неудачного begin */
    CHECK(!rbx3d_project(0, 0, 1, &sx, &sy));
    free(out.pixels);
    puts("PASS near/frustum clipping, first-person pitch direction, invalid frame guard");
}

static void test_voxel_materials(void) {
    for (int b = BLOCK_GRASS; b < BLOCK_COUNT; b++) for (int face = 0; face < 6; face++) {
        const RbxMaterial *m = rbx_material(b, face);
        CHECK(m);
        for (int i = 0; i < 341; i++) CHECK(m->mip[i] < 16);
        for (int i = 0; i < 16; i++) CHECK((m->palette[i] >> 24) == 255);
    }
    Buffer b = make_buffer(160, 120, 5);
    CHECK(rbx3d_begin(&b, 1, .5f, .5f, -2, 0, 0, 72));
    rbx3d_sky(0xFF6EC5F7u, 0xFF6EC5F7u);
    rbx3d_block_face(0, 0, 0, 3, BLOCK_STONE);
    rbx3d_end();
    uint32_t colors[16]; int count = 0;
    for (int y = 48; y < 72; y++) for (int x = 68; x < 92; x++) {
        uint32_t c = b.pixels[y * b.stride + x];
        int found = 0;
        for (int i = 0; i < count; i++) if (colors[i] == c) found = 1;
        if (!found && count < 16) colors[count++] = c;
    }
    CHECK(count >= 3);
    check_padding(&b); free(b.pixels);
    puts("PASS voxel UV textures, bounded palettes and mip levels");
}

int main(void) {
    test_color_fog();
    test_upscale();
    test_perspective_depth();
    test_clipping_and_camera();
    test_voxel_materials();
    return 0;
}
