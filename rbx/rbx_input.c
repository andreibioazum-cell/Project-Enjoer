/* rbx/rbx_input.c — чёрный джойстик слева, свайп-обзор справа.
 * Роль пальца закреплена до отпускания: полёт и обзор работают вместе. */
#include "rbx_internal.h"
#include <math.h>

/* Android/browser pointer ID — не индекс массива и может быть большим. */
static int joy_id = -1, look_id = -1;
static float jx, jy, look_lx, look_ly;
static float joy_cx, joy_cy, joy_r;
static int layout_w, layout_h;

void rbx_input_reset(void) {
    joy_id = look_id = -1;
    jx = jy = 0;
}

void rbx_input_layout(void) {
    if (screen_w == layout_w && screen_h == layout_h) return;
    layout_w = screen_w; layout_h = screen_h;
    rbx_input_reset(); /* поворот/resize не должен оставлять зажатый стик */
    float s = fminf((float)screen_w / 400.0f, (float)screen_h / 600.0f);
    if (s <= 0) { joy_r = 0; return; }
    joy_r = 64.0f * s;
    joy_cx = 86.0f * s;
    joy_cy = (float)screen_h - 90.0f * s;
}

void rbx_input_joy(float *ox, float *oy) {
    if (ox) *ox = jx;
    if (oy) *oy = jy;
}

void rbx_input_joy_geom(float *cx, float *cy, float *r) {
    if (cx) *cx = joy_cx;
    if (cy) *cy = joy_cy;
    if (r) *r = joy_r;
}

static int hit_joy(float x, float y) {
    float dx = x - joy_cx, dy = y - joy_cy;
    return dx * dx + dy * dy <= joy_r * joy_r * 1.3f * 1.3f;
}

static void set_joy(float x, float y) {
    float dx = (x - joy_cx) / joy_r, dy = (y - joy_cy) / joy_r;
    float length = sqrtf(dx * dx + dy * dy);
    const float deadzone = 0.10f;
    if (length <= deadzone) { jx = jy = 0; return; }
    float strength = (fminf(length, 1.0f) - deadzone) / (1.0f - deadzone);
    jx = dx / length * strength;
    jy = dy / length * strength;
}

void rbx_input_touch(float x, float y, int action, int pointer_id) {
    if (action == 3) { rbx_input_reset(); return; } /* Android ACTION_CANCEL */
    if (pointer_id < 0 || !isfinite(x + y)) return;
    rbx_input_layout();
    if (joy_r <= 0) return;
    if (action == 0) {
        if (pointer_id == joy_id || pointer_id == look_id) return;
        if (x < (float)screen_w * 0.5f && hit_joy(x, y) && joy_id < 0) {
            joy_id = pointer_id;
            set_joy(x, y);
        } else if (x >= (float)screen_w * 0.5f && look_id < 0) {
            look_id = pointer_id;
            look_lx = x; look_ly = y;
        }
    } else if (action == 2) {
        if (pointer_id == joy_id) {
            set_joy(x, y);
        } else if (pointer_id == look_id) {
            rbx_camera_look(x - look_lx, y - look_ly);
            look_lx = x; look_ly = y;
        }
    } else if (action == 1) {
        if (pointer_id == joy_id) { joy_id = -1; jx = jy = 0; }
        if (pointer_id == look_id) look_id = -1;
    }
}
