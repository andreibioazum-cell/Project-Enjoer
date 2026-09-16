/* STRICT COMPILER MODE — NO VM, NO REFCOUNT, MANUAL MEMORY, SPEED LIKE C, AOT TO MACHINE CODE
 * The C half of the engine: only the AOT compiled game, no interpretation.
 * DimScript -> C99 via dimscript/compiler.py -> clang -O3 -> machine code .so
 *
 * Enjoer is a 2D engine: input goes straight to the script, the script fills
 * one triangle batch per frame, and the renderer presents it.  There is no 3D
 * scene behind the game — what the game draws is the whole picture.
 */

#include "engine.h"
#include "renderer.h"
#include "dimscript_runtime.h"
#include "ds_files.h"
#include "ds_font.h"
#include "generated/cubicbattle.h" /* fallback AOT game */

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int ready;
static int script_ready;
static DsGameManifest manifest;
/* The size the game was laid out for.  screen_w/screen_h are the platform's own
 * record — the window code writes them the moment it learns something changed —
 * so "is this a relayout" cannot be asked of them; this can only move here. */
static int laid_out_w, laid_out_h;

static double elapsed;
static double fps;

static char game_dir[512];

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
    script_ready = 0;
    elapsed = 0.0;
    fps = 0.0;
    ds_manifest_default(&manifest);

    if (!game_dir[0]) {
#ifdef __ANDROID__
        game_set_game_dir(NULL);
#else
        /* The linked game logic is the AOT Cubic Battle, so the fallback assets
         * must be its folder; ENJOER_GAME/--game still override. */
        const char *env = getenv("ENJOER_GAME");
        if (env) game_set_game_dir(env);
        else game_set_game_dir("game");
#endif
    }

    ready = renderer_init(native_window, screen_w, screen_h);
    if (!ready) {
        app_fail("Could not init renderer");
        return;
    }
    ds_runtime_init();
    ds_engine_reset(screen_w, screen_h);
    laid_out_w = screen_w;
    laid_out_h = screen_h;

    /* STRICT COMPILER: the game logic is AOT compiled; the manifest only
     * carries the title, the clear colour and the knobs both sides share. */
    ds_manifest_default(&manifest);
    snprintf(manifest.title, sizeof(manifest.title), "Cubic Battle 4 (AOT)");

    size_t len = 0;
    char *text = ds_files_read(DS_MANIFEST_NAME, &len);
    if (text) {
        ds_manifest_parse(&manifest, text, len);
        free(text);
    }

    dimscript_init();
    dimscript_load();
    script_ready = 1;
    app_log("Enjoer: AOT compiled 2D game on %s — manual memory, speed like C, no refcount", renderer_backend());
}

void game_resize(int width, int height) {
    /* The Android commands that mean "something about the window changed" fire
     * for things that changed nothing: a tap that lets the system bars peek in
     * immersive mode, a locale or a dark-mode toggle.  Only a size the game was
     * not laid out for is a relayout, and only a relayout may call back into the
     * script — otherwise a HUD is rebuilt under the player's finger.  The
     * renderer still hears about every call: it compares the surface itself and
     * knows that a phone turned upside down changes the framebuffer while the
     * window size stays put. */
    screen_w = width;
    screen_h = height;
    if (ready) renderer_resize(width, height);
    if (width == laid_out_w && height == laid_out_h) return;
    laid_out_w = width;
    laid_out_h = height;
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
        DsString *text = NULL;
        /* The same rule a game follows: a text without a font is recorded and
         * never drawn, so the overlay borrows the game's own face — and it is
         * sized by the screen, or a label that reads fine on a laptop is a
         * smudge on a phone. */
        float scale = 0.8f * (float)screen_h / 360.0f;
        if (scale < 0.8f) scale = 0.8f;
        if (scale > 2.4f) scale = 2.4f;
        snprintf(label, sizeof(label), "%d fps", (int)(fps + 0.5f));
        ds_render_color(1.0f, 1.0f, 1.0f);
        ds_render_font(ds_font_count() > 0 ? 0 : -1);
        /* ds_render_text copies the bytes into the frame: the string itself
         * is a per-frame temp and must go right back. */
        text = ds_string_new(label, strlen(label));
        ds_render_text(text, (float)screen_w - 64.0f * scale, 8.0f * scale, scale);
        ds_release(text);
        ds_render_font(-1);
    }
    enjoer_frame_end();
    renderer_render(preview_target, frame);
}

/* Touch actions mirror Android's MotionEvent: 0 down, 1 up, 2 move, 4 cancel.
 * Every pointer goes to the script untouched — the engine has no camera left
 * to steer, so there is nothing to intercept. */
void game_touch(float x, float y, int action, int pointer_id) {
    if (!ready) return;
    if (action == 0) {
        ds_engine_touch(pointer_id, x, y, 1);
        if (script_ready) dimscript_touchpressed(pointer_id, x, y);
    } else if (action == 2) {
        ds_engine_touch(pointer_id, x, y, 1);
        if (script_ready) dimscript_touchmoved(pointer_id, x, y);
    } else if (action == 1 || action == 4) {
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
}

void game_cancel_input(void) {
    /* Touch state lives in the script-facing engine state; a cancel simply
     * stops meaning anything once every pointer is forwarded as-is. */
}

void game_shutdown(void) {
    if (script_ready) {
        dimscript_quit();
        dimscript_shutdown();
        script_ready = 0;
    }
    ds_runtime_shutdown();
    if (ready) renderer_shutdown();
    ready = 0;
    game_cancel_input();
}
