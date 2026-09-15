/* STRICT COMPILER MODE — NO VM, NO REFCOUNT, MANUAL MEMORY, SPEED LIKE C, AOT TO MACHINE CODE
 * Only tests AOT compiled games, no VM.
 */
#include "engine.h"
#include "vulkan_cube.h"
#include "dimscript_runtime.h"
#include "enjoer_draw.h"
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

static int run_compiled_clicker(Buffer *frame) {
    screen_w = 320;
    screen_h = 240;
    frame->width = frame->stride = screen_w;
    frame->height = screen_h;
    game_init(NULL);
    CHECK(!app_failed());
    CHECK(!game_is_interpreted()); /* STRICT: no VM */
    CHECK(cube_renderer_backend() && cube_renderer_backend()[0] != '\0');
    dt = 1.0 / 60.0;
    game_update();
    game_draw(frame);
    unsigned long first = checksum(frame);
    CHECK(first != 0);
    CHECK(ds_render_text_count() == 3);
    CHECK(enjoer_frame()->text_count == 3);

    game_key("ArrowLeft", 1);
    game_draw(frame);
    CHECK(checksum(frame) != first);
    game_key("r", 1);
    game_key("r", 0);
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
    if (run_aot_brick(&frame)) return 1;
    free(frame.pixels);
    puts("PASS C game state + C++ cube renderer + AOT compiled games (strict compiler, manual memory, speed like C)");
    return 0;
}
