/* The intentionally small C half of the game.
 *
 * There is no world generator, menu or inventory now: this is a focused cube
 * playground. C owns time, input and camera state;
 * the C++ file owns the Dawn device and the draw calls. */
#include "engine.h"
#include "dawn_cube.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

static int ready;
static float rotation;
static float pitch;
static float spin = 0.65f;
static int dragging;
static int drag_id = -1;
static float last_x;
static float last_y;
static int reset_down;

static float clampf(float value, float lo, float hi) {
    return value < lo ? lo : value > hi ? hi : value;
}

void game_init(void *native_window) {
    rotation = 0.0f;
    pitch = -0.18f;
    spin = 0.65f;
    dragging = 0;
    drag_id = -1;
    ready = cube_renderer_init(native_window, screen_w, screen_h);
    if (!ready) app_fail("Could not initialize the Dawn cube renderer");
    else app_log("Enjoer: 3D cube renderer (%s)", cube_renderer_backend());
}

void game_resize(int width, int height) {
    screen_w = width;
    screen_h = height;
    if (ready) cube_renderer_resize(width, height);
}

void game_update(void) {
    if (!ready) return;
    float seconds = (float)dt;
    if (!isfinite(seconds) || seconds < 0.0f) seconds = 0.0f;
    if (seconds > 0.05f) seconds = 0.05f;
    if (!reset_down) rotation += seconds * spin;
}

void game_draw(Buffer *preview_target) {
    if (ready) cube_renderer_render(preview_target, rotation, pitch);
}

void game_touch(float x, float y, int action, int pointer_id) {
    if (!ready) return;
    if (action == 0) {
        if (drag_id < 0) {
            dragging = 1;
            drag_id = pointer_id;
            last_x = x;
            last_y = y;
        }
    } else if (action == 2 && dragging && pointer_id == drag_id) {
        float dx = x - last_x;
        float dy = y - last_y;
        rotation += dx * 0.012f;
        pitch = clampf(pitch + dy * 0.008f, -1.15f, 1.15f);
        last_x = x;
        last_y = y;
    } else if ((action == 1 || action == 4) && pointer_id == drag_id) {
        dragging = 0;
        drag_id = -1;
    }
}

void game_key(const char *name, int down) {
    if (!ready || !name) return;
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
    if (ready) cube_renderer_shutdown();
    ready = 0;
    game_cancel_input();
}
