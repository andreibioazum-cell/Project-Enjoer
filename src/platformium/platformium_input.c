/* Touch and keyboard controls. Each touch owns exactly one control
 * (left, right or jump), mirroring the geometrium multitouch rules. */
#include "platformium_internal.h"

static int left_id = -1, right_id = -1, jump_id = -1;
static int key_left, key_right, key_jump;
static int jump_press;              /* buffered press, consumed by physics */
static float left_x, left_y, right_x, right_y, jump_x, jump_y;
static float left_r, right_r, jump_r, ui_scale;
static int layout_w, layout_h;

void pm_input_reset(void) {
    left_id = right_id = jump_id = -1;
    key_left = key_right = key_jump = 0;
    jump_press = 0;
}

void pm_input_layout(void) {
    if (screen_w == layout_w && screen_h == layout_h) return;
    layout_w = screen_w; layout_h = screen_h;
    pm_input_reset();
    ui_scale = fminf(screen_w / 960.0f, screen_h / 540.0f);
    if (ui_scale <= 0) { left_r = right_r = jump_r = 0; return; }
    left_r = 46 * ui_scale; right_r = 46 * ui_scale; jump_r = 54 * ui_scale;
    left_x = 74 * ui_scale; left_y = screen_h - 82 * ui_scale;
    right_x = 186 * ui_scale; right_y = screen_h - 82 * ui_scale;
    jump_x = screen_w - 92 * ui_scale; jump_y = screen_h - 96 * ui_scale;
}

void pm_input_geom(int control, float *x, float *y, float *r) {
    if (control == 0) { if (x) *x = left_x; if (y) *y = left_y; if (r) *r = left_r; }
    else if (control == 1) { if (x) *x = right_x; if (y) *y = right_y; if (r) *r = right_r; }
    else { if (x) *x = jump_x; if (y) *y = jump_y; if (r) *r = jump_r; }
}

static int hit_circle(float x, float y, float cx, float cy, float r) {
    float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

void pm_input_touch(float x, float y, int action, int id) {
    if (action == 3) { pm_input_reset(); return; }
    if (id < 0 || !isfinite(x + y)) return;
    pm_input_layout();
    if (jump_r <= 0) return;
    if (action == 0) {
        if (id == left_id || id == right_id || id == jump_id) return;
        if (hit_circle(x, y, jump_x, jump_y, jump_r * 1.25f)) {
            jump_id = id; jump_press = 1; key_jump = 1;
        } else if (hit_circle(x, y, left_x, left_y, left_r * 1.3f)) {
            left_id = id; key_left = 1;
        } else if (hit_circle(x, y, right_x, right_y, right_r * 1.3f)) {
            right_id = id; key_right = 1;
        } else if (x < screen_w * .5f) { left_id = id; key_left = 1; }
        else { jump_id = id; jump_press = 1; key_jump = 1; }
    } else if (action == 1 || action == 4) {
        if (id == left_id) { left_id = -1; key_left = 0; }
        if (id == right_id) { right_id = -1; key_right = 0; }
        if (id == jump_id) { jump_id = -1; key_jump = 0; }
    }
    /* action == 2 (move) needs no handling: controls are press/hold only */
}

void pm_input_key(const char *name, int down) {
    if (!name) return;
    if (!strcmp(name, "a") || !strcmp(name, "ArrowLeft")) key_left = down;
    else if (!strcmp(name, "d") || !strcmp(name, "ArrowRight")) key_right = down;
    else if (!strcmp(name, "space") || !strcmp(name, "w") || !strcmp(name, "ArrowUp")) {
        if (down && !key_jump) jump_press = 1;
        key_jump = down;
    }
}

float pm_input_move(void) {
    float m = 0;
    if (key_left) m -= 1;
    if (key_right) m += 1;
    return m;
}

int pm_input_jump_held(void) { return key_jump; }

int pm_input_jump_take_press(void) {
    int press = jump_press;
    jump_press = 0;
    return press;
}

void pm_input_draw(void) {
    float u = ui_scale;
    if (u <= 0) return;
    /* left / right pads */
    ring(left_x, left_y, left_r, 3 * u, 0x66000000u);
    circle(left_x - 12 * u, left_y, 7 * u, key_left ? 0xff000000u : 0x66000000u);
    ring(right_x, right_y, right_r, 3 * u, 0x66000000u);
    circle(right_x + 12 * u, right_y, 7 * u, key_right ? 0xff000000u : 0x66000000u);
    /* jump */
    circle(jump_x, jump_y, jump_r, key_jump ? 0xe6ffffffu : 0xb3ffffffu);
    pm_text_center("Jump", jump_x, jump_y - 7 * u, 0xff1c2430u, .40f * u);
}
