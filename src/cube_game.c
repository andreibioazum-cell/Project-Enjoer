/* STRICT COMPILER MODE — NO VM, NO REFCOUNT, MANUAL MEMORY, SPEED LIKE C, AOT TO MACHINE CODE
 * The C half of the engine: only AOT compiled games, no interpretation.
 * DimScript -> C99 via dimscript/compiler.py -> clang -O3 -> machine code .so
 */

#include "engine.h"
#include "vulkan_cube.h"
#include "dimscript_runtime.h"
#include "ds_files.h"
#include "generated/clicker.h" /* fallback AOT game */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ready;
static int script_ready;
static DsGameManifest manifest;

static float rotation;
static float pitch;
static float spin = 0.65f;
static int dragging;
static int drag_id = -1;
static float last_x;
static float last_y;
static int reset_down;
static double elapsed;
static double fps;

static char game_dir[512];

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

void game_set_game_dir(const char *dir) {
    if (!dir) {
        game_dir[0] = '\0';
        ds_files_set_root(NULL);
        return;
    }
    snprintf(game_dir, sizeof(game_dir), "%s", dir);
    ds_files_set_root(game_dir);
}

const char *game_title(void) {
    return manifest.title[0] ? manifest.title : "Enjoer";
}
int game_is_interpreted(void) { return 0; } /* STRICT: no VM */
int game_script_failed(void) { return 0; }
const char *game_script_error(void) { return ""; }
const DsGameManifest *game_manifest(void) { return &manifest; }

void game_init(void *native_window) {
    rotation = 0.0f;
    pitch = -0.18f;
    spin = 0.65f;
    dragging = 0;
    drag_id = -1;
    script_ready = 0;
    elapsed = 0.0;
    fps = 0.0;
    ds_manifest_default(&manifest);

    if (!game_dir[0]) {
#ifdef __ANDROID__
        game_set_game_dir(NULL);
#else
        const char *env = getenv("ENJOER_GAME");
        if (env) game_set_game_dir(env);
        else game_set_game_dir(".");
#endif
    }

    ready = cube_renderer_init(native_window, screen_w, screen_h);
    if (!ready) {
        app_fail("Could not init Vulkan renderer");
        return;
    }
    ds_runtime_init();
    ds_engine_reset(screen_w, screen_h);

    /* STRICT COMPILER: no manifest VM loading, always AOT */
    ds_manifest_default(&manifest);
    manifest.show_cube = 1;
    snprintf(manifest.title, sizeof(manifest.title), "Enjoer Clicker (AOT)");

    /* Try to read manifest for title only, but game logic is AOT compiled */
    size_t len = 0;
    char *text = ds_files_read(DS_MANIFEST_NAME, &len);
    if (text) {
        ds_manifest_parse(&manifest, text, len);
        free(text);
    }

    dimscript_init();
    dimscript_load();
    script_ready = 1;
    app_log("Enjoer: AOT compiled game on %s — manual memory, speed like C, no refcount", cube_renderer_backend());
}

void game_resize(int width, int height) {
    screen_w = width;
    screen_h = height;
    if (ready) cube_renderer_resize(width, height);
    ds_engine_reset(screen_w, screen_h);
    if (script_ready) dimscript_resized((float)width, (float)height);
}

void game_update(void) {
    if (!ready) return;
    float sec = (float)dt;
    if (!isfinite(sec) || sec < 0.0f) sec = 0.0f;
    if (sec > 0.05f) sec = 0.05f;
    elapsed += sec;
    fps = sec > 0.0f ? fps + (1.0f / sec - fps) * 0.1f : fps;
    if (!reset_down) rotation += sec * spin;
    ds_engine_new_frame(elapsed, sec, fps);
    if (ds_engine_quit_requested()) {
        ds_engine_reset(screen_w, screen_h);
        app_quit();
        return;
    }
    if (script_ready) dimscript_update(sec);
}

void game_draw(Buffer *preview_target) {
    if (!ready) return;
    EnjoerFrame *frame = enjoer_frame();
    enjoer_frame_begin(screen_w, screen_h);
    for (int i = 0; i < 3; ++i) frame->clear_color[i] = manifest.clear_color[i];

    if (script_ready) dimscript_draw();

    if (manifest.show_fps) {
        char label[32];
        snprintf(label, sizeof(label), "%d fps", (int)(fps + 0.5f));
        ds_render_color(1.0f, 1.0f, 1.0f);
        ds_render_text(ds_string_new(label, strlen(label)), (float)(screen_w - 64), 8.0f, 0.8f);
    }
    enjoer_frame_end();
    cube_renderer_render(preview_target, rotation, pitch, frame, manifest.show_cube);
}

void game_touch(float x, float y, int action, int pointer_id) {
    if (!ready) return;
    if (action == 0) {
        if (drag_id < 0) {
            dragging = 1;
            drag_id = pointer_id;
            last_x = x; last_y = y;
            ds_engine_touch(pointer_id, x, y, 1);
            if (script_ready) dimscript_touchpressed(pointer_id, x, y);
        }
    } else if (action == 2 && dragging && pointer_id == drag_id) {
        float dx = x - last_x;
        float dy = y - last_y;
        rotation += dx * 0.012f;
        pitch = clampf(pitch + dy * 0.008f, -1.15f, 1.15f);
        last_x = x; last_y = y;
        ds_engine_touch(pointer_id, x, y, 1);
        if (script_ready) dimscript_touchmoved(pointer_id, x, y);
    } else if ((action == 1 || action == 4) && pointer_id == drag_id) {
        dragging = 0;
        drag_id = -1;
        ds_engine_touch(pointer_id, x, y, 0);
        if (script_ready) dimscript_touchreleased(pointer_id, x, y);
    }
}

void game_key(const char *name, int down) {
    if (!ready || !name) return;
    ds_engine_key(name, down);
    if (script_ready) {
        if (down) dimscript_keypressed(name);
        else dimscript_keyreleased(name);
    }
    if (!strcmp(name, "r") || !strcmp(name, "R")) {
        reset_down = down;
        if (down) { rotation = 0.0f; pitch = -0.18f; }
        return;
    }
    if (!down) return;
    if (!strcmp(name, "ArrowLeft") || !strcmp(name, "a") || !strcmp(name, "A")) rotation -= 0.14f;
    else if (!strcmp(name, "ArrowRight") || !strcmp(name, "d") || !strcmp(name, "D")) rotation += 0.14f;
    else if (!strcmp(name, "ArrowUp") || !strcmp(name, "w") || !strcmp(name, "W")) pitch = clampf(pitch - 0.10f, -1.15f, 1.15f);
    else if (!strcmp(name, "ArrowDown") || !strcmp(name, "s") || !strcmp(name, "S")) pitch = clampf(pitch + 0.10f, -1.15f, 1.15f);
    else if (!strcmp(name, "space")) spin = spin < 0.0f ? 0.65f : -spin;
}

void game_cancel_input(void) {
    dragging = 0;
    drag_id = -1;
    reset_down = 0;
}

void game_shutdown(void) {
    if (script_ready) {
        dimscript_quit();
        dimscript_shutdown();
        script_ready = 0;
    }
    ds_runtime_shutdown();
    if (ready) cube_renderer_shutdown();
    ready = 0;
    game_cancel_input();
}
