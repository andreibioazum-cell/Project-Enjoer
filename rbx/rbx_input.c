/* rbx/rbx_input.c — сенсорный ввод: виртуальный джойстик слева,
 * кнопка прыжка справа, осмотр камеры перетаскиванием. */
#include "rbx_internal.h"
#include <math.h>
#include <string.h>

static int p_on[MAX_PTR];
static float p_x[MAX_PTR], p_y[MAX_PTR];
static int joy_id = -1, look_id = -1;
static float jx, jy, look_lx, look_ly;
static float joy_cx, joy_cy, joy_r, jmp_cx, jmp_cy, jmp_r;

void rbx_input_layout(void) {
    float s = (float)screen_w / 400.0f;
    if (s < 0.75f) s = 0.75f;
    joy_r = 64.0f * s;
    joy_cx = 86.0f * s;
    joy_cy = (float)screen_h - 90.0f * s;
    jmp_r = 40.0f * s;
    jmp_cx = (float)screen_w - 74.0f * s;
    jmp_cy = (float)screen_h - 100.0f * s;
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

void rbx_input_jump_geom(float *cx, float *cy, float *r) {
    if (cx) *cx = jmp_cx;
    if (cy) *cy = jmp_cy;
    if (r) *r = jmp_r;
}

void rbx_input_reset(void) {
    joy_id = look_id = -1;
    jx = jy = 0;
    memset(p_on, 0, sizeof(p_on));
}

static int hit_circ(float x, float y, float cx, float cy, float r) {
    float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

void rbx_input_touch(float x, float y, int action, int pointer_id) {
    int id = pointer_id;
    if (id < 0 || id >= MAX_PTR) id = 0;
    if (action == 0) { /* down */
        p_on[id] = 1; p_x[id] = x; p_y[id] = y;
        if (hit_circ(x, y, jmp_cx, jmp_cy, jmp_r * 1.25f)) {
            rbx_player_jump();
            return;
        }
        if (x < (float)screen_w * 0.46f && y > (float)screen_h * 0.55f && joy_id < 0) {
            joy_id = id;
            jx = (x - joy_cx) / joy_r;
            jy = (y - joy_cy) / joy_r;
            float L = sqrtf(jx * jx + jy * jy);
            if (L > 1) { jx /= L; jy /= L; }
        } else if (look_id < 0) {
            look_id = id;
            look_lx = x; look_ly = y;
        }
    } else if (action == 2) { /* move */
        p_x[id] = x; p_y[id] = y;
        if (id == joy_id) {
            jx = (x - joy_cx) / joy_r;
            jy = (y - joy_cy) / joy_r;
            float L = sqrtf(jx * jx + jy * jy);
            if (L > 1) { jx /= L; jy /= L; }
        } else if (id == look_id) {
            float dx = x - look_lx, dy = y - look_ly;
            rbx_camera_look(dx, dy);
            look_lx = x; look_ly = y;
        }
    } else if (action == 1) { /* up */
        p_on[id] = 0;
        if (id == joy_id) { joy_id = -1; jx = 0; jy = 0; }
        if (id == look_id) look_id = -1;
        rbx_player_jump_release();
    }
}
