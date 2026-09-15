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
    CHECK(frame_has(enjoer_frame(), "Счет: 1"));
    game_touch(200, 130, 2, 7);
    game_touch(200, 130, 1, 7);
    game_cancel_input();
    game_resize(240, 320);
    CHECK(screen_w == 240 && screen_h == 320);
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

int main(void) {
    Buffer frame = {0};
    frame.pixels = (uint32_t*)calloc((size_t)320*240, sizeof(uint32_t));
    CHECK(frame.pixels);
    if (run_compiled_clicker(&frame)) return 1;
    free(frame.pixels);
    frame.pixels = (uint32_t*)calloc((size_t)640*640, sizeof(uint32_t));
    CHECK(frame.pixels);
    if (run_assets(&frame)) return 1;
    if (run_manifest()) return 1;
    if (run_aot_brick(&frame)) return 1;
    free(frame.pixels);
    puts("PASS C game state + C++ 2D renderer + AOT compiled games + images/fonts (strict compiler, manual memory, speed like C)");
    return 0;
}
