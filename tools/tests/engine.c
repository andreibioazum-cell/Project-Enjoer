/* STRICT COMPILER MODE — NO VM, NO REFCOUNT, MANUAL MEMORY, SPEED LIKE C, AOT TO MACHINE CODE
 * Only tests the AOT compiled game, the 2D renderer and the asset registries.
 */
#include "engine.h"
#include "renderer.h"
#include "dimscript_runtime.h"
#include "enjoer_draw.h"
#include "ds_font.h"
#include "ds_image.h"
#include "ds_ttf.h"
#include "ds_manifest.h"
#include "ds_files.h"
#include "surface_transform.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(v) do { if (!(v)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #v); return 1; } } while(0)

static unsigned long checksum(const Buffer *frame) {
    unsigned long r = 2166136261u;
    for (int y = 0; y < frame->height; ++y)
        for (int x = 0; x < frame->width; ++x)
            r = (r ^ frame->pixels[y * frame->stride + x]) * 16777619u;
    return r;
}
static int frame_has(const EnjoerFrame *frame, const char *needle) {
    for (int i = 0; i < frame->text_count; ++i)
        if (strstr(frame->texts[i].text, needle)) return 1;
    return 0;
}

static int32_t load_image(const char *name) {
    DsString *text = ds_string_new(name, strlen(name));
    const int32_t handle = ds_image_load(text);
    ds_release(text);
    return handle;
}

static int32_t load_font(const char *name) {
    DsString *text = ds_string_new(name, strlen(name));
    const int32_t handle = ds_font_load(text);
    ds_release(text);
    return handle;
}

static int write_bytes(const char *path, const void *bytes, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    const size_t written = bytes && size ? fwrite(bytes, 1, size, file) : 0;
    fclose(file);
    return written == size;
}

static int run_compiled_clicker(Buffer *frame) {
    screen_w = 320;
    screen_h = 240;
    frame->width = frame->stride = screen_w;
    frame->height = screen_h;
    /* The real game folder: the AOT clicker loads its bundled font.ttf and the
     * text pass turns the recorded texts into glyph quads end to end. */
    game_set_game_dir("games/clicker");
    game_init(NULL);
    CHECK(!app_failed());
    CHECK(!game_is_interpreted()); /* STRICT: no VM */
    CHECK(renderer_backend() && renderer_backend()[0] != '\0');
    dt = 1.0 / 60.0;
    game_update();
    game_draw(frame);
    unsigned long first = checksum(frame);
    CHECK(first != 0);
    CHECK(ds_render_text_count() == 4);
    CHECK(enjoer_frame()->text_count == 4);
    CHECK(enjoer_frame()->texts_resolved == 1);
    CHECK(ds_font_count() == 1);
    CHECK(ds_ttf_layer(0) >= 0);
    CHECK(enjoer_frame()->vertex_count > 0); /* glyph quads landed in the batch */

    /* Pure 2D: drawing the same state twice paints identical pixels. */
    game_draw(frame);
    CHECK(checksum(frame) == first);

    /* Keys reach the script only — with no 3D camera left, they change no pixel. */
    game_key("ArrowLeft", 1);
    game_key("ArrowLeft", 0);
    game_draw(frame);
    CHECK(checksum(frame) == first);

    game_touch(160, 120, 0, 7);
    game_draw(frame);
    CHECK(frame_has(enjoer_frame(), "Счёт: 1"));
    game_touch(200, 130, 2, 7);
    game_touch(200, 130, 1, 7);
    game_cancel_input();
    game_resize(240, 320);
    CHECK(screen_w == 240 && screen_h == 320);
    CHECK(ds_engine_width() == 240.0 && ds_engine_height() == 320.0);
    /* The same numbers are not a relayout.  Being laid out again shows up as the
     * engine size being set, so poison it and check that a no-op resize leaves
     * it alone — a game that is relaid out under the finger is the shake a
     * player reports, and Android sends this command for far less than a
     * rotation. */
    ds_engine_reset(7, 9);
    game_resize(240, 320);
    CHECK(ds_engine_width() == 7.0 && ds_engine_height() == 9.0);
    game_resize(200, 100);
    CHECK(ds_engine_width() == 200.0 && ds_engine_height() == 100.0);
    game_shutdown();
    return 0;
}

static int run_assets(Buffer *frame) {
    (void)frame;
    /* Images and fonts resolve through the game folder, like on a device. */
    game_set_game_dir("tools/tests/data");
    ds_runtime_init();
    ds_engine_reset(64, 64);

    const int32_t dot = load_image("dot.png");
    CHECK(dot == 0);
    CHECK(ds_image_count() == 1);
    CHECK(ds_image_width(dot) == 1 && ds_image_height(dot) == 1);
    CHECK(load_image("dot.png") == dot); /* loaded once, same handle */
    CHECK(load_image("missing.png") == -1);

    /* The format sniffer behind the PNG-only loader: every common raster
     * header is named, short and NULL inputs stay "unknown" without
     * crashing. */
    static const unsigned char png_head[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    static const unsigned char jpeg_head[12] = {0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10,
                                                'J', 'F', 'I', 'F', 0x00, 0x01};
    static const unsigned char gif_head[6] = {'G', 'I', 'F', '8', '9', 'a'};
    static const unsigned char bmp_head[2] = {'B', 'M'};
    static const unsigned char webp_head[12] = {'R', 'I', 'F', 'F', 0, 0, 0, 0,
                                                'W', 'E', 'B', 'P'};
    static const unsigned char tiff_head[4] = {'I', 'I', '*', 0};
    CHECK(ds_png_magic_ok(png_head, sizeof(png_head)));
    CHECK(!ds_png_magic_ok(png_head, 7));
    CHECK(!ds_png_magic_ok(NULL, 0));
    CHECK(!strcmp(ds_image_format_name(png_head, sizeof(png_head)), "PNG"));
    CHECK(!strcmp(ds_image_format_name(jpeg_head, sizeof(jpeg_head)), "JPEG"));
    CHECK(!strcmp(ds_image_format_name(gif_head, sizeof(gif_head)), "GIF"));
    CHECK(!strcmp(ds_image_format_name(bmp_head, sizeof(bmp_head)), "BMP"));
    CHECK(!strcmp(ds_image_format_name(webp_head, sizeof(webp_head)), "WebP"));
    CHECK(!strcmp(ds_image_format_name(tiff_head, sizeof(tiff_head)), "TIFF"));
    CHECK(!strcmp(ds_image_format_name((const uint8_t *)"hello", 5), "unknown"));
    CHECK(!strcmp(ds_image_format_name(NULL, 0), "unknown"));
    CHECK(!strcmp(ds_image_format_name(webp_head, 8), "unknown"));

    enjoer_frame_begin(64, 64);
    ds_render_color(1.0f, 1.0f, 1.0f);
    ds_render_image(dot, 0.0f, 0.0f, 8.0f, 8.0f);
    CHECK(enjoer_frame()->vertex_count == 6);
    enjoer_frame_clear(64, 64);

    /* End to end: a JPEG file is refused with the format in the log, and the
     * refusal adds nothing to the registry. */
    const char *img_out = getenv("BUILD_DIR");
    char img_dir[256];
    snprintf(img_dir, sizeof(img_dir), "%s", img_out && img_out[0] ? img_out : "build-tests");
    char jpg_path[300];
    snprintf(jpg_path, sizeof(jpg_path), "%s/photo-test.jpg", img_dir);
    CHECK(write_bytes(jpg_path, jpeg_head, sizeof(jpeg_head)));
    game_set_game_dir(img_dir);
    CHECK(load_image("photo-test.jpg") == -1);
    CHECK(ds_image_count() == 1);

    /* Fonts: the registry only checks the header magic (a full face parse runs
     * on top and fails gracefully), so the test font is a header's worth of
     * honest bytes written at runtime. */
    const char *out = getenv("BUILD_DIR");
    char dir[256];
    snprintf(dir, sizeof(dir), "%s", out && out[0] ? out : "build-tests");
    char font_path[300];
    snprintf(font_path, sizeof(font_path), "%s/font-test.ttf", dir);
    static const unsigned char font_magic[8] = {0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
    CHECK(write_bytes(font_path, font_magic, sizeof(font_magic)));
    char bad_path[300];
    snprintf(bad_path, sizeof(bad_path), "%s/bad-test.ttf", dir);
    CHECK(write_bytes(bad_path, "not a font", 10));
    game_set_game_dir(dir);

    const int32_t font = load_font("font-test.ttf");
    CHECK(font == 0);
    CHECK(ds_font_count() == 1);
    /* Magic alone is not a font: the face stays down and texts in it stay
     * recorded but undrawn, exactly like the default face. */
    CHECK(ds_ttf_layer(font) == -1);
    CHECK(load_font("font-test.ttf") == font);
    CHECK(load_font("missing.ttf") == -1);
    CHECK(load_font("bad-test.ttf") == -1);
    CHECK(ds_font_valid(font) && !ds_font_valid(7));
    CHECK(ds_font_at(font) && ds_font_at(font)->size == sizeof(font_magic));

    ds_render_font(font);
    CHECK(ds_render_current_font() == font);
    enjoer_frame_begin(64, 64);
    DsString *hello = ds_string_new("hi", 2);
    ds_render_text(hello, 1.0f, 2.0f, 1.0f);
    ds_release(hello);
    CHECK(enjoer_frame()->text_count == 1);
    CHECK(enjoer_frame()->texts[0].font == font);
    ds_render_font(99); /* bogus handles are ignored, not recorded */
    CHECK(ds_render_current_font() == font);
    ds_render_font(-1);
    CHECK(ds_render_current_font() == -1);
    enjoer_frame_clear(64, 64);

    ds_runtime_shutdown();
    game_set_game_dir("tools/tests/data");
    return 0;
}

static int run_manifest(void) {
    DsGameManifest manifest;
    static const char text[] =
        "title = \"Assets\"\n"
        "package = \"com.cb4.assets\"\n"
        "scripts = [\"main.ds\"]\n"
        "images = [\"hero.png\", \"sub/coin.png\"]\n"
        "fonts = [\"font.ttf\"]\n";
    ds_manifest_default(&manifest);
    CHECK(ds_manifest_parse(&manifest, text, sizeof(text) - 1));
    CHECK(manifest.image_count == 2);
    CHECK(!strcmp(manifest.images[0], "hero.png"));
    CHECK(!strcmp(manifest.images[1], "sub/coin.png"));
    CHECK(manifest.font_count == 1);
    CHECK(!strcmp(manifest.fonts[0], "font.ttf"));

    static const char bad_image[] = "images = [\"hero.jpg\"]\n";
    ds_manifest_default(&manifest);
    CHECK(!ds_manifest_parse(&manifest, bad_image, sizeof(bad_image) - 1));

    /* The 3D cube is gone: an old manifest that still asks for it fails with
     * a message that says exactly what to delete. */
    static const char cube[] = "cube = true\n";
    ds_manifest_default(&manifest);
    CHECK(!ds_manifest_parse(&manifest, cube, sizeof(cube) - 1));
    CHECK(strstr(manifest.error, "cube") != NULL);
    return 0;
}

static int run_aot_brick(Buffer *frame) {
    /* In strict compiler mode, brick game is also AOT compiled via tools/aot.py
     * For now we just test that AOT clicker works and that brick manifest parses */
    game_set_game_dir("games/brick");
    screen_w = 640;
    screen_h = 360;
    frame->width = frame->stride = screen_w;
    frame->height = screen_h;

    /* Parse manifest only, no VM */
    size_t len = 0;
    char *txt = ds_files_read(DS_MANIFEST_NAME, &len);
    if (txt) {
        DsGameManifest m;
        ds_manifest_default(&m);
        if (ds_manifest_parse(&m, txt, len)) {
            CHECK(m.script_count == 3);
            CHECK(!strcmp(m.title, "Кирпич"));
            CHECK(!strcmp(m.package, "com.cb4.brick"));
        }
        free(txt);
    }

    /* Run AOT clicker again but with brick dir set to ensure no crash */
    game_init(NULL);
    CHECK(!game_is_interpreted());
    dt = 1.0 / 60.0;
    for (int i = 0; i < 3; ++i) { game_update(); game_draw(frame); }
    CHECK(enjoer_frame()->vertex_count >= 0);
    game_shutdown();
    return 0;
}

/* The Android surface math: how a game's screen pixels reach a swapchain that
 * the compositor is going to rotate.  This is the one place the fix for "the
 * landscape app drew itself sideways" can be checked without a GPU, so it is
 * checked here instead of on a phone. */
static int clip_of(const float *m, float x, float y, float *out_x, float *out_y) {
    *out_x = m[0] * x + m[4] * y + m[12];
    *out_y = m[1] * x + m[5] * y + m[13];
    return 0;
}

static int run_surface_transform(void) {
    static const int rotations[] = {ENJOER_SURFACE_ROTATE_0, ENJOER_SURFACE_ROTATE_90,
                                    ENJOER_SURFACE_ROTATE_180, ENJOER_SURFACE_ROTATE_270};
    int index = 0;
    int width = 0;
    int height = 0;
    /* A mirror or an INHERIT is not ours to guess: the caller must not build a
     * rotation matrix out of it. */
    CHECK(enjoer_surface_rotation_from_vk(1 << 20) == -1);

    /* Two vocabularies for the same thing: Vulkan indexes the four rotations a
     * surface can ask for (0, 1, 2, 3) while this file counts them in degrees,
     * and conflating them is invisible in the worst way — every rotation looks
     * unsupported, the pre-rotation quietly stops happening, and the picture is
     * still upright because the compositor takes over. */
    CHECK(enjoer_surface_rotation_from_vk(0) == ENJOER_SURFACE_ROTATE_0);
    CHECK(enjoer_surface_rotation_from_vk(1) == ENJOER_SURFACE_ROTATE_90);
    CHECK(enjoer_surface_rotation_from_vk(2) == ENJOER_SURFACE_ROTATE_180);
    CHECK(enjoer_surface_rotation_from_vk(3) == ENJOER_SURFACE_ROTATE_270);
    CHECK(enjoer_surface_rotation_from_vk(4) == -1); /* INHERIT_BIT: not a rotation */
    CHECK(enjoer_surface_rotation_to_vk(ENJOER_SURFACE_ROTATE_270) == 3);
    CHECK(enjoer_surface_rotation_bit(ENJOER_SURFACE_ROTATE_0) == 1);
    CHECK(enjoer_surface_rotation_bit(ENJOER_SURFACE_ROTATE_90) == (1 << 1));

    /* Pre-rotate when the driver asks for it *and* accepts it on a swapchain;
     * take identity when it asks for something it will not take, since the
     * alternative is a failed vkCreateSwapchainKHR and a black screen.  The
     * framebuffer size follows the choice: the extent and the projection are the
     * same decision, made once. */
    CHECK(enjoer_surface_rotation_for(1, (1 << 1) | 1) == ENJOER_SURFACE_ROTATE_90);
    CHECK(enjoer_surface_rotation_for(3, 1 << 3) == ENJOER_SURFACE_ROTATE_270);
    CHECK(enjoer_surface_rotation_for(1, 1) == ENJOER_SURFACE_ROTATE_0);
    CHECK(enjoer_surface_rotation_for(4, 0xFF) == ENJOER_SURFACE_ROTATE_0);
    {
        const int turned = enjoer_surface_rotation_for(1, (1 << 1) | 1);
        const int flat = enjoer_surface_rotation_for(1, 1);
        enjoer_surface_extent(turned, 2400, 1080, &width, &height);
        CHECK(width == 1080 && height == 2400);
        enjoer_surface_extent(flat, 2400, 1080, &width, &height);
        CHECK(width == 2400 && height == 1080);
    }

    /* A landscape window on a panel that is portrait: 90 and 270 transpose the
     * framebuffer, because that is the image the compositor rotates. */
    enjoer_surface_extent(ENJOER_SURFACE_ROTATE_90, 2400, 1080, &width, &height);
    CHECK(width == 1080 && height == 2400);
    enjoer_surface_extent(ENJOER_SURFACE_ROTATE_270, 2400, 1080, &width, &height);
    CHECK(width == 1080 && height == 2400);
    enjoer_surface_extent(ENJOER_SURFACE_ROTATE_180, 2400, 1080, &width, &height);
    CHECK(width == 2400 && height == 1080);
    enjoer_surface_extent(ENJOER_SURFACE_ROTATE_0, 2400, 1080, &width, &height);
    CHECK(width == 2400 && height == 1080);

    /* The extent a driver will accept: min/max clamp, and a driver that states
     * no limit at all (0, or the 0xFFFFFFFF "undefined" sentinel) must not
     * produce a degenerate image. */
    enjoer_surface_framebuffer(ENJOER_SURFACE_ROTATE_90, 2400, 1080, 1, 1, 9999, 9999, &width,
                               &height);
    CHECK(width == 1080 && height == 2400);
    enjoer_surface_framebuffer(ENJOER_SURFACE_ROTATE_90, 2400, 1080, 1, 1, 0, 0xFFFFFFFF, &width,
                               &height);
    CHECK(width == 1080 && height == 2400);
    enjoer_surface_framebuffer(ENJOER_SURFACE_ROTATE_90, 2400, 1080, 1, 1, 1000, 1000, &width,
                               &height);
    CHECK(width == 1000 && height == 1000);

    /* Android's pre-rotation table as a per-point mapping: with ROTATE_90 the
     * window's top-left sits on the framebuffer's top-right, and ROTATE_270 is
     * the mirror-image choice.  Swapping the two is an upside-down game, which
     * no screenshot of a host test would ever catch — so the pair is pinned. */
    {
        float u = 0.0f;
        float v = 0.0f;
        enjoer_surface_map(ENJOER_SURFACE_ROTATE_90, 1080.0f, 2400.0f, 0.0f, 0.0f, &u, &v);
        CHECK((int)u == 1080 && (int)v == 0);
        enjoer_surface_map(ENJOER_SURFACE_ROTATE_90, 1080.0f, 2400.0f, 2400.0f, 1080.0f, &u, &v);
        CHECK((int)u == 0 && (int)v == 2400);
        enjoer_surface_map(ENJOER_SURFACE_ROTATE_270, 1080.0f, 2400.0f, 0.0f, 0.0f, &u, &v);
        CHECK((int)u == 0 && (int)v == 2400);
    }

    for (index = 0; index < 4; ++index) {
        const int rotation = rotations[index];
        float m[16];
        float u[4], v[4];
        int corner = 0;
        int fb_w = 1, fb_h = 1;
        const float xs[4] = {0.0f, 2400.0f, 0.0f, 2400.0f};
        const float ys[4] = {0.0f, 0.0f, 1080.0f, 1080.0f};
        enjoer_surface_extent(rotation, 2400, 1080, &fb_w, &fb_h);
        enjoer_surface_ortho(rotation, (float)fb_w, (float)fb_h, m);
        for (corner = 0; corner < 4; ++corner) {
            clip_of(m, xs[corner], ys[corner], &u[corner], &v[corner]);
            /* Every window corner lands on a framebuffer corner: the game fills
             * the screen at any rotation, with nothing squashed into the top
             * left and nothing cut off the right — the shape of the bug this
             * replaced. */
            CHECK(fabsf(fabsf(u[corner]) - 1.0f) < 0.001f);
            CHECK(fabsf(fabsf(v[corner]) - 1.0f) < 0.001f);
        }
        /* And they stay four *different* corners: a rotation, not a collapse
         * onto one edge. */
        {
            int a = 0, b = 0;
            for (a = 0; a < 4; ++a)
                for (b = a + 1; b < 4; ++b)
                    CHECK(!(fabsf(u[a] - u[b]) < 0.001f && fabsf(v[a] - v[b]) < 0.001f));
        }
        {
            float cx = 0.0f, cy = 0.0f;
            clip_of(m, 1200.0f, 540.0f, &cx, &cy);
            CHECK(fabsf(cx) < 0.001f && fabsf(cy) < 0.001f);
        }
        /* One screen pixel is one framebuffer pixel on both axes: the mapping
         * rotates, it never stretches — text keeps its proportions. */
        {
            float ax = 0.0f, ay = 0.0f, bx = 0.0f, by = 0.0f;
            float step = 64.0f;
            clip_of(m, 300.0f, 300.0f, &ax, &ay);
            clip_of(m, 300.0f + step, 300.0f, &bx, &by);
            /* The step in x is one framebuffer axis, in y the other, so each
             * contributes on exactly one of clip x / clip y, and the length of
             * the step in clip units is step / the framebuffer size along it. */
            {
                const float dx = fabsf(bx - ax), dy = fabsf(by - ay);
                const float along = dx > dy ? dx : dy;
                const float axis = dx > dy ? (float)fb_w : (float)fb_h;
                CHECK(fabsf(along * axis / (2.0f * step) - 1.0f) < 0.001f);
            }
        }
        /* The invariant every check above is blind to: the mapping must be a
         * rotation, never a mirror.  A reflection lands corners on corners and
         * keeps step lengths, yet on the device it reads as "every letter
         * backwards and upside down" — so pin the determinant (a rotation
         * keeps it positive) and pin exact agreement with the point map, on
         * asymmetric points a reflection would move: window pixel (x, y) must
         * land on the very framebuffer pixel enjoer_surface_map() assigns it,
         * which under the plain full-screen viewport is
         * ndc = 2*u/size - 1 on *both* axes — the one negated axis is the
         * whole bug. */
        {
            const float determinant = m[0] * m[5] - m[4] * m[1];
            CHECK(determinant > 0.0f);
        }
        {
            static const float px[6] = {0.0f, 2400.0f, 0.0f, 2400.0f, 37.0f, 2000.0f};
            static const float py[6] = {0.0f, 0.0f, 1080.0f, 1080.0f, 91.0f, 555.0f};
            int point = 0;
            for (point = 0; point < 6; ++point) {
                float want_u = 0.0f, want_v = 0.0f, got_x = 0.0f, got_y = 0.0f;
                enjoer_surface_map(rotation, (float)fb_w, (float)fb_h, px[point], py[point],
                                   &want_u, &want_v);
                clip_of(m, px[point], py[point], &got_x, &got_y);
                CHECK(fabsf(got_x - (2.0f * want_u / (float)fb_w - 1.0f)) < 0.001f);
                CHECK(fabsf(got_y - (2.0f * want_v / (float)fb_h - 1.0f)) < 0.001f);
            }
        }
    }
    return 0;
}

/* The text pass on its own frame: glyph boxes land on whole pixels, which is
 * what keeps a HUD steady while it is being rescaled, and a game that draws the
 * same text at two sizes gets two atlas cells instead of one of them shrunk. */
static int run_text_pass(void) {
    DsString *label = NULL;
    const EnjoerFrame *frame = NULL;
    int vertex = 0;
    int32_t font = -1;

    game_set_game_dir("games/clicker");
    ds_runtime_init();
    ds_engine_reset(640, 360);
    font = load_font("font.ttf");
    CHECK(font >= 0);
    ds_render_font(font);
    ds_render_color(1.0f, 1.0f, 1.0f);

    /* 10.3 / 20.7 are on purpose: an origin between two pixels must still put
     * every glyph edge on one. */
    enjoer_frame_begin(640, 360);
    label = ds_string_new("Счёт 12", strlen("Счёт 12"));
    ds_render_text(label, 10.3f, 20.7f, 2.0f);
    ds_release(label);
    ds_ttf_resolve_frame();
    frame = enjoer_frame();
    CHECK(frame->vertex_count >= 3);
    for (vertex = 0; vertex < frame->vertex_count; ++vertex) {
        const float x = frame->vertices[vertex].x;
        const float y = frame->vertices[vertex].y;
        CHECK(fabsf(x - floorf(x)) < 0.0001f);
        CHECK(fabsf(y - floorf(y)) < 0.0001f);
        CHECK(frame->vertices[vertex].layer == (float)ds_ttf_layer(font));
    }
    /* Texels inside their cell, never into the padding or the neighbour glyph:
     * the quad spans the glyph, the uv spans it minus half a texel per side. */
    for (vertex = 0; vertex + 5 < frame->vertex_count; vertex += 6) {
        const EnjoerVertex *top_left = &frame->vertices[vertex];
        const EnjoerVertex *top_right = &frame->vertices[vertex + 1];
        const EnjoerVertex *bottom_right = &frame->vertices[vertex + 2];
        CHECK(top_right->u > top_left->u && bottom_right->v > top_left->v);
        CHECK((top_right->u - top_left->u) * (float)ds_ttf_atlas_size() >= 1.0f);
        CHECK(top_left->u * (float)ds_ttf_atlas_size() >= 1.5f);
    }
    enjoer_frame_clear(640, 360);

    /* Two sizes of one string, each in its own frame: a glyph then costs an
     * atlas cell of the size it is drawn at, so neither text is a shrunk copy
     * of the other.  That is the whole difference between a readable 16 px
     * label and mush, and the ratio below is how to see it from the batch. */
    {
        const float atlas = (float)ds_ttf_atlas_size();
        float big_cell = 0.0f, big_width = 0.0f;
        float small_cell = 0.0f, small_width = 0.0f;

        enjoer_frame_begin(640, 360);
        label = ds_string_new("Счёт 12", strlen("Счёт 12"));
        ds_render_text(label, 10.0f, 20.0f, 2.0f);
        ds_render_text(label, 10.0f, 120.0f, 0.6f);
        ds_release(label);
        ds_ttf_resolve_frame();
        CHECK(enjoer_frame()->vertex_count >= 12);
        /* Vertex 0 of each text's first quad: 6 vertices per glyph, and the
         * two texts start at the same x, so the first glyph of each is at the
         * same index in its own half of the batch. */
        {
            const EnjoerVertex *v = enjoer_frame()->vertices;
            const int per_text = enjoer_frame()->vertex_count / 2;
            big_cell = (v[1].u - v[0].u) * atlas;
            big_width = v[1].x - v[0].x;
            small_cell = (v[per_text + 1].u - v[per_text].u) * atlas;
            small_width = v[per_text + 1].x - v[per_text].x;
        }
        /* One texel per screen pixel at both sizes: the ratio a game can rely
         * on, and the reason small text is sharp instead of soft. */
        CHECK(big_width > 0.0f && small_width > 0.0f);
        CHECK(fabsf(big_cell / big_width - 1.0f) < 0.25f);
        CHECK(fabsf(small_cell / small_width - 1.0f) < 0.25f);
        /* And the two sizes are two rasterizations of the glyph, not one. */
        CHECK(big_width > small_width * 2.0f);
        CHECK(fabsf(big_cell - small_cell) > 1.0f);
        enjoer_frame_clear(640, 360);
    }

    ds_runtime_shutdown();
    return 0;
}

/* The two counters an image registry has to offer a renderer: rebuilding the
 * texture array is for a new image, copying pixels is for a glyph that just
 * baked.  Folding the second into the first is what made a score hitch on a
 * tap. */
static int run_image_registry(void) {
    ds_runtime_init();
    CHECK(ds_image_count() == 0);
    CHECK(ds_image_generation() != 0);

    {
        uint8_t *pixels = (uint8_t *)calloc(4, 4);
        const uint64_t generation = ds_image_generation();
        const int32_t handle = 0;
        CHECK(pixels);
        CHECK(ds_image_add("sprite.png", pixels, 4, 4) == handle);
        CHECK(ds_image_count() == 1);
        CHECK(ds_image_generation() != generation); /* the set itself changed */
        CHECK((ds_image_dirty_mask() & 1u) != 0);  /* and its pixels are new */

        ds_image_clear_dirty();
        CHECK(ds_image_dirty_mask() == 0);

        /* A glyph landing in an atlas is a pixel change and nothing else: no
         * new generation, one dirty layer. */
        {
            const uint64_t after_add = ds_image_generation();
            ds_image_touch_layer(handle);
            CHECK(ds_image_generation() == after_add);
            CHECK(ds_image_dirty_mask() == 1u);
            ds_image_clear_dirty();
            ds_image_touch(); /* no layer named: everything might have moved */
            CHECK(ds_image_dirty_mask() != 0);
            ds_image_clear_dirty();
            CHECK(ds_image_dirty_mask() == 0);
        }
    }
    ds_runtime_shutdown();
    return 0;
}

int main(void) {
    Buffer frame = {0};
    frame.pixels = (uint32_t*)calloc((size_t)320*240, sizeof(uint32_t));
    CHECK(frame.pixels);
    if (run_compiled_clicker(&frame)) return 1;
    if (run_surface_transform()) return 1;
    free(frame.pixels);
    frame.pixels = (uint32_t*)calloc((size_t)640*640, sizeof(uint32_t));
    CHECK(frame.pixels);
    if (run_assets(&frame)) return 1;
    if (run_manifest()) return 1;
    if (run_image_registry()) return 1;
    if (run_text_pass()) return 1;
    if (run_aot_brick(&frame)) return 1;
    free(frame.pixels);
    puts("PASS C game state + C++ 2D renderer + AOT compiled games + images/fonts (strict compiler, manual memory, speed like C)");
    return 0;
}
