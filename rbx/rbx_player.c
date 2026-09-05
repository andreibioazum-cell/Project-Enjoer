/* rbx/rbx_player.c — полёт от первого лица. Джойстик/WASD двигают
 * вдоль взгляда, без гравитации и инерции; стены остаются твёрдыми. */
#include "rbx_internal.h"
#include <math.h>
#include <string.h>

#define FLY_SPEED 13.5f
#define PITCH_LIMIT 1.45f
#define TAU 6.28318530718f

static float px, py, pz, walk;
static float cyaw, cpitch;
static int k_w, k_a, k_s, k_d, k_rise, k_sink;
static int k_left, k_right, k_up, k_down;

void rbx_player_spawn(void) {
    px = 0; py = 0.32f; pz = 1.2f;
    walk = 0;
    cyaw = 0.48f; cpitch = -0.12f;
}

void rbx_player_pos(float *x, float *y, float *z, float *yaw, float *walk_out) {
    if (x) *x = px;
    if (y) *y = py;
    if (z) *z = pz;
    if (yaw) *yaw = cyaw;
    if (walk_out) *walk_out = walk;
}

void rbx_camera_angles(float *yaw, float *pitch) {
    if (yaw) *yaw = cyaw;
    if (pitch) *pitch = cpitch;
}

static void clamp_angles(void) {
    cyaw = remainderf(cyaw, TAU);
    if (cpitch < -PITCH_LIMIT) cpitch = -PITCH_LIMIT;
    if (cpitch > PITCH_LIMIT) cpitch = PITCH_LIMIT;
}

void rbx_camera_look(float dx, float dy) {
    if (screen_w <= 0 || screen_h <= 0 || !isfinite(dx + dy)) return;
    cyaw += dx / (float)screen_w * 3.4f;
    cpitch -= dy / (float)screen_h * 2.6f;
    clamp_angles();
}

void rbx_key_reset(void) {
    k_w = k_a = k_s = k_d = k_rise = k_sink = 0;
    k_left = k_right = k_up = k_down = 0;
}

void rbx_key_state(const char *name, int down) {
    int d = down ? 1 : 0;
    if (!name || !name[0]) return;
    if (!strcmp(name, "w") || !strcmp(name, "W")) k_w = d;
    else if (!strcmp(name, "s") || !strcmp(name, "S")) k_s = d;
    else if (!strcmp(name, "a") || !strcmp(name, "A")) k_a = d;
    else if (!strcmp(name, "d") || !strcmp(name, "D")) k_d = d;
    else if (!strcmp(name, "ArrowLeft")) k_left = d;
    else if (!strcmp(name, "ArrowRight")) k_right = d;
    else if (!strcmp(name, "ArrowUp")) k_up = d;
    else if (!strcmp(name, "ArrowDown")) k_down = d;
    else if (!strcmp(name, "space") || !strcmp(name, " ")) k_rise = d;
    else if (!strcmp(name, "Shift")) k_sink = d;
}

/* Небольшие шаги не дают пролететь сквозь тонкую стену при просадке FPS. */
static void move_axis(int axis, float delta) {
    if (delta == 0) return;
    const float hx = RBX_PLAYER_RADIUS, hy = RBX_PLAYER_HEIGHT * 0.5f, epsilon = .0001f;
    float *pos = axis == 0 ? &px : axis == 1 ? &py : &pz;
    *pos += delta;
    int count = 0;
    const RbxBox *boxes = rbx_world_boxes(&count);
    for (int i = 0; i < count; i++) {
        const RbxBox *b = &boxes[i];
        if (b->mat == MAT_SKIP || b->mat == MAT_WATER || b->mat == MAT_LAVA) continue;
        float cx = px, cy = py + hy, cz = pz;
        /* Контакт с полом не считается проникновением по X/Z из-за float. */
        if (fabsf(cx - b->x) >= hx + b->hx - epsilon || fabsf(cy - b->y) >= hy + b->hy - epsilon ||
            fabsf(cz - b->z) >= hx + b->hz - epsilon) continue;
        if (axis == 0) px = b->x + (delta > 0 ? -b->hx - hx : b->hx + hx);
        else if (axis == 2) pz = b->z + (delta > 0 ? -b->hz - hx : b->hz + hx);
        else py = delta > 0 ? b->y - b->hy - RBX_PLAYER_HEIGHT : b->y + b->hy;
    }
}

static int touching_lava(void) {
    int count = 0;
    const RbxBox *boxes = rbx_world_boxes(&count);
    for (int i = 0; i < count; i++) {
        const RbxBox *b = &boxes[i];
        if (b->mat == MAT_LAVA && fabsf(px - b->x) < b->hx + RBX_PLAYER_RADIUS &&
            fabsf(pz - b->z) < b->hz + RBX_PLAYER_RADIUS &&
            py < b->y + b->hy && py + RBX_PLAYER_HEIGHT > b->y - b->hy) return 1;
    }
    return 0;
}

void rbx_player_update(float d) {
    if (!isfinite(d) || d <= 0) return;
    if (d > 0.05f) d = 0.05f;
    float jx, jy;
    rbx_input_joy(&jx, &jy);
    cyaw += (k_right - k_left) * 1.7f * d;
    cpitch += (k_up - k_down) * 1.1f * d;
    clamp_angles();

    float mx = jx + k_d - k_a, forward = -jy + k_w - k_s;
    float fs = sinf(cyaw), fc = cosf(cyaw), cp = cosf(cpitch), sp = sinf(cpitch);
    float vx = fs * cp * forward + fc * mx;
    float vy = sp * forward + k_rise - k_sink;
    float vz = fc * cp * forward - fs * mx;
    float length = sqrtf(vx * vx + vy * vy + vz * vz);
    if (length > 1) { vx /= length; vy /= length; vz /= length; length = 1; }

    int steps = (int)ceilf(FLY_SPEED * d / 0.35f);
    float step = FLY_SPEED * d / steps;
    for (int i = 0; i < steps; i++) {
        move_axis(1, vy * step);
        move_axis(0, vx * step);
        move_axis(2, vz * step);
        if (touching_lava() || py < -12.0f) {
            rbx_player_spawn();
            rbx_cancel_input();
            snd_play("notify.wav");
            return;
        }
        rbx_world_collect(px, py, pz);
    }
    walk += d * 9.0f * length;
}
