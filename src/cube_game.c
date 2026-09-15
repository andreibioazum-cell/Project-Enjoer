/* The C half of the engine: time, input, the game manifest and the choice
 * between the two ways a DimScript game can run.
 *
 *   1. interpreted (the normal case) — a folder with game.manifest and .ds
 *      files is loaded by the VM in src/ds_vm.c, so a game can be changed
 *      without rebuilding anything;
 *   2. ahead of time compiled — src/generated/clicker.c, produced by the
 *      DimScript compiler from examples/clicker.ds, used when no game folder is
 *      present so a fresh checkout still shows something.
 *
 * The C++ file owns the device and the draw calls; this file owns what a game
 * can see: screen size, elapsed time, pointers and keys. */
#include "engine.h"
#include "vulkan_cube.h"
#include "dimscript_runtime.h"
#include "ds_files.h"
#include "ds_vm.h"
#include "generated/clicker.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SCRIPT_FILES 32

static int ready;
static int interpreted;
static int script_ready;
static int script_failed;
static DsVM *vm;
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

static float clampf(float value, float lo, float hi) {
    return value < lo ? lo : value > hi ? hi : value;
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

int game_is_interpreted(void) { return interpreted; }

int game_script_failed(void) { return script_failed; }

const char *game_script_error(void) {
    if (interpreted && vm) return ds_vm_error(vm);
    return "";
}

const DsGameManifest *game_manifest(void) { return &manifest; }

/* --- game loading --------------------------------------------------------- */

static int manifest_exists(void) {
    size_t length = 0;
    char *text = ds_files_read(DS_MANIFEST_NAME, &length);
    if (!text) return 0;
    ds_manifest_default(&manifest);
    const int ok = ds_manifest_parse(&manifest, text, length);
    free(text);
    if (!ok) app_fail("game.manifest: %s", manifest.error);
    else app_log("Enjoer: game '%s' (%s, %d fps, %d script(s))", manifest.title,
                 manifest.orientation, manifest.target_fps, manifest.script_count);
    return ok;
}

static void module_name(const char *file, char *out, size_t capacity) {
    const char *base = file;
    for (const char *cursor = file; *cursor; ++cursor)
        if (*cursor == '/' || *cursor == '\\') base = cursor + 1;
    size_t length = strlen(base);
    if (length > 3 && !strcmp(base + length - 3, ".ds")) length -= 3;
    if (length >= capacity) length = capacity - 1;
    memcpy(out, base, length);
    out[length] = '\0';
}

static void copy_name(char *out, size_t capacity, const char *text) {
    size_t length = strlen(text);
    if (length > capacity - 1) length = capacity - 1;
    memcpy(out, text, length);
    out[length] = '\0';
}

static int add_script(DsVM *machine, const char *file) {
    size_t length = 0;
    char *text = ds_files_read(file, &length);
    if (!text) {
        app_fail("нет файла %s в папке игры", file);
        return 0;
    }
    char name[DS_FILES_NAME];
    module_name(file, name, sizeof(name));
    const int ok = ds_vm_add_source(machine, name, text, length);
    free(text);
    if (!ok) app_fail("%s: %s", file, ds_vm_error(machine));
    return ok;
}

static int load_interpreted_game(void) {
    char files[MAX_SCRIPT_FILES][DS_FILES_NAME];
    int count = 0;

    for (int index = 0; index < manifest.script_count && count < MAX_SCRIPT_FILES; ++index) {
        copy_name(files[count], DS_FILES_NAME, manifest.scripts[index]);
        ++count;
    }
    /* Anything the manifest did not mention still belongs to the game: the
     * engine loads it after the listed files so a forgotten entry is not a
     * silent no-op on the desktop. */
    char found[MAX_SCRIPT_FILES][DS_FILES_NAME];
    const int found_count = ds_files_list_ds(found, MAX_SCRIPT_FILES);
    for (int index = 0; index < found_count && count < MAX_SCRIPT_FILES; ++index) {
        int known = 0;
        for (int existing = 0; existing < count; ++existing)
            if (!strcmp(files[existing], found[index])) known = 1;
        if (!known) {
            copy_name(files[count], DS_FILES_NAME, found[index]);
            ++count;
        }
    }
    if (!count) {
        app_fail("в игре нет ни одного .ds файла");
        return 0;
    }

    vm = ds_vm_create();
    if (!vm) return 0;
    for (int index = 0; index < count; ++index)
        if (!add_script(vm, files[index])) return 0;
    if (!ds_vm_link(vm)) {
        app_fail("DimScript: %s", ds_vm_error(vm));
        ds_vm_destroy(vm);
        vm = NULL;
        return 0;
    }
    interpreted = 1;
    return 1;
}

void game_init(void *native_window) {
    rotation = 0.0f;
    pitch = -0.18f;
    spin = 0.65f;
    dragging = 0;
    drag_id = -1;
    script_ready = 0;
    script_failed = 0;
    interpreted = 0;
    elapsed = 0.0;
    fps = 0.0;
    ds_manifest_default(&manifest);

    if (!game_dir[0]) {
#ifdef __ANDROID__
        /* No folder on a device: ds_files reads the APK's assets/game. */
        game_set_game_dir(NULL);
#else
        const char *from_environment = getenv("ENJOER_GAME");
        if (from_environment) game_set_game_dir(from_environment);
        else game_set_game_dir(".");
#endif
    }

    ready = cube_renderer_init(native_window, screen_w, screen_h);
    if (!ready) {
        app_fail("Could not initialize the Vulkan renderer");
        return;
    }
    ds_runtime_init();
    ds_engine_reset(screen_w, screen_h);

    if (manifest_exists()) {
        if (load_interpreted_game()) {
            if (!ds_vm_start(vm)) {
                app_fail("DimScript: %s", ds_vm_error(vm));
                return;
            }
            app_log("Enjoer: interpreted %s (%s, %d .ds file(s))", manifest.title,
                    cube_renderer_backend(), ds_vm_script_count(vm));
            return;
        }
        return;
    }
    if (manifest.error[0]) {
        app_fail("game.manifest: %s", manifest.error);
        return;
    }

    /* No game folder: run the ahead-of-time compiled example instead.  The cube
     * stays visible, because without a game the preview *is* the cube. */
    ds_manifest_default(&manifest);
    manifest.show_cube = 1;
    snprintf(manifest.title, sizeof(manifest.title), "Enjoer Clicker");
    dimscript_init();
    dimscript_load();
    script_ready = 1;
    app_log("Enjoer: compiled-in DimScript clicker on %s", cube_renderer_backend());
}

void game_resize(int width, int height) {
    screen_w = width;
    screen_h = height;
    if (ready) cube_renderer_resize(width, height);
    ds_engine_reset(screen_w, screen_h);
    if (interpreted && vm && !script_failed) ds_vm_call_two_numbers(vm, "resized", (double)width, (double)height);
    else if (script_ready) dimscript_resized((float)width, (float)height);
}

static void report_script_error(void) {
    if (!script_failed) {
        script_failed = 1;
        app_log_error("DimScript: %s", ds_vm_error(vm));
    }
}

void game_update(void) {
    if (!ready) return;
    float seconds = (float)dt;
    if (!isfinite(seconds) || seconds < 0.0) seconds = 0.0f;
    if (seconds > 0.05f) seconds = 0.05f;
    elapsed += seconds;
    fps = seconds > 0.0 ? fps + (1.0 / seconds - fps) * 0.1 : fps;
    if (!reset_down) rotation += seconds * spin;
    ds_engine_new_frame(elapsed, seconds, fps);
    /* engine.quit() is the script asking to close the window: the VM raises its
     * own flag and generated C raises the runtime one, so both are checked. */
    if (ds_engine_quit_requested() || (interpreted && vm && ds_vm_quit_requested(vm))) {
        ds_engine_reset(screen_w, screen_h);
        app_quit();
        return;
    }
    if (interpreted && vm) {
        if (script_failed) return;
        ds_vm_set_budget(vm, 200000);
        if (!ds_vm_call_number(vm, "update", seconds)) report_script_error();
    } else if (script_ready) {
        dimscript_update(seconds);
    }
}

void game_draw(Buffer *preview_target) {
    if (!ready) return;
    EnjoerFrame *frame = enjoer_frame();
    enjoer_frame_begin(screen_w, screen_h);
    /* The manifest paints the background up front; render.clear(...) in the
     * script overwrites it, which is how a game gets a per-screen colour. */
    for (int index = 0; index < 3; ++index) frame->clear_color[index] = manifest.clear_color[index];

    if (interpreted && vm) {
        if (!script_failed) {
            ds_vm_set_budget(vm, 200000);
            if (!ds_vm_call_void(vm, "draw")) report_script_error();
        }
    } else if (script_ready) {
        dimscript_draw();
    }
    if (manifest.show_fps) {
        /* The frame counter is part of the batch, not a widget: with no font
         * backend it shows up as recorded text, which the preview draws and a
         * future text pass will place exactly the same way. */
        char label[32];
        snprintf(label, sizeof(label), "%d fps", (int)(fps + 0.5));
        ds_render_color(1.0f, 1.0f, 1.0f);
        ds_render_text(label, (float)(screen_w - 64), 8.0f, 0.8f);
    }
    enjoer_frame_end();

    cube_renderer_render(preview_target, rotation, pitch, frame, manifest.show_cube);

    if (interpreted && vm && !script_failed) ds_vm_collect(vm);
}

void game_touch(float x, float y, int action, int pointer_id) {
    if (!ready) return;
    if (action == 0) {
        if (drag_id < 0) {
            dragging = 1;
            drag_id = pointer_id;
            last_x = x;
            last_y = y;
            ds_engine_touch(pointer_id, x, y, 1);
            if (interpreted && vm && !script_failed) ds_vm_call_touch(vm, "touchpressed", pointer_id, x, y);
            else if (script_ready) dimscript_touchpressed(pointer_id, x, y);
        }
    } else if (action == 2 && dragging && pointer_id == drag_id) {
        const float dx = x - last_x;
        const float dy = y - last_y;
        rotation += dx * 0.012f;
        pitch = clampf(pitch + dy * 0.008f, -1.15f, 1.15f);
        last_x = x;
        last_y = y;
        ds_engine_touch(pointer_id, x, y, 1);
        if (interpreted && vm && !script_failed) ds_vm_call_touch(vm, "touchmoved", pointer_id, x, y);
        else if (script_ready) dimscript_touchmoved(pointer_id, x, y);
    } else if ((action == 1 || action == 4) && pointer_id == drag_id) {
        dragging = 0;
        drag_id = -1;
        ds_engine_touch(pointer_id, x, y, 0);
        if (interpreted && vm && !script_failed) ds_vm_call_touch(vm, "touchreleased", pointer_id, x, y);
        else if (script_ready) dimscript_touchreleased(pointer_id, x, y);
    }
}

void game_key(const char *name, int down) {
    if (!ready || !name) return;
    ds_engine_key(name, down);
    if (interpreted && vm && !script_failed) {
        if (down) {
            ds_vm_call_string(vm, "keypressed", name);
        } else {
            ds_vm_call_string(vm, "keyreleased", name);
        }
    } else if (script_ready) {
        if (down) dimscript_keypressed(name);
        else dimscript_keyreleased(name);
    }
    if (!strcmp(name, "r") || !strcmp(name, "R")) {
        reset_down = down;
        if (down) {
            rotation = 0.0f;
            pitch = -0.18f;
        }
        return;
    }
    if (!down) return;
    if (!strcmp(name, "ArrowLeft") || !strcmp(name, "a") || !strcmp(name, "A"))
        rotation -= 0.14f;
    else if (!strcmp(name, "ArrowRight") || !strcmp(name, "d") || !strcmp(name, "D"))
        rotation += 0.14f;
    else if (!strcmp(name, "ArrowUp") || !strcmp(name, "w") || !strcmp(name, "W"))
        pitch = clampf(pitch - 0.10f, -1.15f, 1.15f);
    else if (!strcmp(name, "ArrowDown") || !strcmp(name, "s") || !strcmp(name, "S"))
        pitch = clampf(pitch + 0.10f, -1.15f, 1.15f);
    else if (!strcmp(name, "space"))
        spin = spin < 0.0f ? 0.65f : -spin;
}

void game_cancel_input(void) {
    dragging = 0;
    drag_id = -1;
    reset_down = 0;
}

void game_shutdown(void) {
    if (interpreted && vm) {
        ds_vm_stop(vm); /* runs the script's own quit callback first */
        ds_vm_destroy(vm);
        vm = NULL;
        interpreted = 0;
    }
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
