/* rbx/rbx_player.c — игрок: физика (AABB по осям), вода/лава/батут,
 * камера третьего лица, клавиатурные клавиши, прыжок. */
#include "rbx_internal.h"
#include <math.h>
#include <string.h>

static float px, py, pz, pvy, pyaw, walk;
static int on_ground, jumped;
static float cyaw, cpitch;
static int k_w, k_a, k_s, k_d, k_sp, k_left, k_right, k_up, k_down;

void rbx_player_spawn(void) {
    px = 0; py = 0.32f; pz = 1.2f;
    pvy = 0; pyaw = 0.55f; walk = 0;
    on_ground = 1;
    cyaw = 0.48f; cpitch = -0.20f;
}

void rbx_player_pos(float *x, float *y, float *z, float *yaw, float *walk_out) {
    if (x) *x = px;
    if (y) *y = py;
    if (z) *z = pz;
    if (yaw) *yaw = pyaw;
    if (walk_out) *walk_out = walk;
}

void rbx_camera_angles(float *yaw, float *pitch) {
    if (yaw) *yaw = cyaw;
    if (pitch) *pitch = cpitch;
}

void rbx_camera_look(float dx, float dy) {
    cyaw += dx / (float)screen_w * 3.4f;
    cpitch -= dy / (float)screen_h * 2.2f;
    if (cpitch < -1.15f) cpitch = -1.15f;
    if (cpitch > 0.55f) cpitch = 0.55f;
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
    else if (!strcmp(name, "space") || !strcmp(name, " ")) k_sp = d;
}

void rbx_player_jump(void) {
    if (on_ground && !jumped) {
        pvy = 16.5f;
        on_ground = 0;
        jumped = 1;
        snd_play("send.wav");
    }
}

static int aabb(float ax, float ay, float az, float ahx, float ahy, float ahz, const RbxBox *b) {
    return fabsf(ax - b->x) < ahx + b->hx &&
           fabsf(ay - b->y) < ahy + b->hy &&
           fabsf(az - b->z) < ahz + b->hz;
}

static void resolve_axis(int axis, float *px_, float *py_, float *pz_, float *pvy_, int *ground, int *lava, int *water, int *bounce) {
    const float hx = 0.82f, hy = 2.45f, hz = 0.82f;
    float cx = *px_, cy = *py_ + hy, cz = *pz_;
    int count = 0;
    const RbxBox *boxes = rbx_world_boxes(&count);
    for (int i = 0; i < count; i++) {
        const RbxBox *b = &boxes[i];
        if (b->mat == MAT_SKIP) continue;
        if (b->mat == MAT_WATER) {
            if (aabb(cx, cy, cz, hx, hy, hz, b)) *water = 1;
            continue;
        }
        if (b->mat == MAT_LAVA) {
            if (fabsf(*px_ - b->x) < b->hx && fabsf(*pz_ - b->z) < b->hz &&
                *py_ < b->y + b->hy + 1.8f && *py_ > b->y - b->hy - 1.0f)
                *lava = 1;
            continue;
        }
        if (!aabb(cx, cy, cz, hx, hy, hz, b)) continue;
        if (b->mat == MAT_BOUNCE && axis == 1) *bounce = 1;
        if (axis == 0) {
            float pen = hx + b->hx - fabsf(cx - b->x);
            *px_ += (cx > b->x) ? pen : -pen;
            cx = *px_;
        } else if (axis == 2) {
            float pen = hz + b->hz - fabsf(cz - b->z);
            *pz_ += (cz > b->z) ? pen : -pen;
            cz = *pz_;
        } else {
            float pen = hy + b->hy - fabsf(cy - b->y);
            if (cy >= b->y) {
                *py_ += pen;
                if (*pvy_ < 0) *pvy_ = 0;
                *ground = 1;
            } else {
                *py_ -= pen;
                if (*pvy_ > 0) *pvy_ = 0;
            }
            cy = *py_ + hy;
        }
    }
}

void rbx_player_update(float d) {
    float jx, jy;
    rbx_input_joy(&jx, &jy);

    if (k_left) cyaw -= 1.7f * d;
    if (k_right) cyaw += 1.7f * d;
    if (k_up) cpitch += 1.1f * d;
    if (k_down) cpitch -= 1.1f * d;
    if (cpitch < -1.15f) cpitch = -1.15f;
    if (cpitch > 0.55f) cpitch = 0.55f;

    float mx = jx, mz = -jy;
    if (k_w) mz += 1;
    if (k_s) mz -= 1;
    if (k_a) mx -= 1;
    if (k_d) mx += 1;
    float ml = sqrtf(mx * mx + mz * mz);
    if (ml > 1.0f) { mx /= ml; mz /= ml; ml = 1; }

    float fs = sinf(cyaw), fc = cosf(cyaw);
    float wishx = fs * mz + fc * mx;
    float wishz = fc * mz + (-fs) * mx;

    int water = 0, lava = 0, bounce = 0;
    float speed = water ? 6.0f : 13.5f;
    /* water flag from previous frame roughly — resolve after move */

    if (k_sp) rbx_player_jump();
    else jumped = 0;

    pvy += -48.0f * d;
    if (pvy < -42.0f) pvy = -42.0f;
    py += pvy * d;
    on_ground = 0;
    resolve_axis(1, &px, &py, &pz, &pvy, &on_ground, &lava, &water, &bounce);
    if (water) {
        pvy += 28.0f * d; /* выталкивание */
        if (pvy > 4.0f) pvy = 4.0f;
        if (pvy < -8.0f) pvy = -8.0f;
        speed = 6.5f;
    }
    if (bounce && pvy <= 0.5f) {
        pvy = 24.0f;
        on_ground = 0;
        bounce = 0;
    }

    px += wishx * speed * d;
    resolve_axis(0, &px, &py, &pz, &pvy, &on_ground, &lava, &water, &bounce);
    pz += wishz * speed * d;
    resolve_axis(2, &px, &py, &pz, &pvy, &on_ground, &lava, &water, &bounce);

    if (ml > 0.15f) {
        pyaw = atan2f(wishx, wishz);
        walk += d * 9.0f * ml;
    } else {
        walk += d * 0.8f;
    }

    if (lava || py < -6.0f) {
        rbx_player_spawn();
        snd_play("notify.wav");
    }

    rbx_world_collect(px, py, pz);
}

/* тач-кнопка прыжка отпускает флаг */
void rbx_player_jump_release(void) { jumped = 0; }
