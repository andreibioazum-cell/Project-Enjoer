/* Smooth follow camera with facing lookahead, level clamping and shake. */
#include "platformium_internal.h"

static struct { float x, y, look, shake; } cam;

void pm_camera_reset(float x, float y) {
    cam.x = x; cam.y = y; cam.look = 0; cam.shake = 0;
}

float pm_tile_px(void) {
    return screen_h > 0 ? screen_h / 11.0f : 48.0f;
}

void pm_camera_shake(float amount) {
    cam.shake = fmaxf(cam.shake, amount);
}

static float approach(float value, float target, float step) {
    if (value < target) return fminf(value + step, target);
    return fmaxf(value - step, target);
}

void pm_camera_update(float d) {
    PmLevelDef *level = pm_level_live();
    float px, py;
    pm_player_pos(&px, &py);
    const PmPlayer *player = pm_player_state();
    if (!level || d <= 0) { cam.x = px; cam.y = py; return; }
    /* lookahead eases toward the facing direction, never snapping */
    cam.look = approach(cam.look, player->dir * 2.1f, 6.0f * d);
    float k = 1.0f - expf(-5.0f * d);
    cam.x += (px + cam.look - cam.x) * k;
    float target_y = cam.y;
    if (py < cam.y - 1.6f) target_y = py + 1.6f;
    if (py > cam.y + 1.8f) target_y = py - 1.8f;
    cam.y += (target_y - cam.y) * (1.0f - expf(-4.0f * d));
    /* clamp to the level bounds */
    float tpx = pm_tile_px();
    float half_w = screen_w / (2.0f * tpx), half_h = screen_h / (2.0f * tpx);
    if (level->w <= 2 * half_w) cam.x = level->w * .5f;
    else {
        if (cam.x < half_w) cam.x = half_w;
        if (cam.x > level->w - half_w) cam.x = level->w - half_w;
    }
    if (level->h <= 2 * half_h) cam.y = level->h * .5f;
    else {
        if (cam.y < half_h - 1.0f) cam.y = half_h - 1.0f;
        if (cam.y > level->h - half_h) cam.y = level->h - half_h;
    }
    cam.shake = fmaxf(0.0f, cam.shake - 10.0f * d * (0.4f + cam.shake));
}

void pm_camera_center(float *x, float *y) {
    float ox = 0, oy = 0;
    if (cam.shake > 0) {
        double t = pm_time * 60.0;
        ox = cam.shake * (float)fmod(sin(t * 1.31) + sin(t * 2.17), 1.0);
        oy = cam.shake * (float)fmod(sin(t * 1.73) + sin(t * 2.51), 1.0);
    }
    if (x) *x = cam.x + ox;
    if (y) *y = cam.y + oy;
}

void pm_world_to_screen(float wx, float wy, float *sx, float *sy) {
    float cx, cy, tpx = pm_tile_px();
    pm_camera_center(&cx, &cy);
    if (sx) *sx = (wx - cx) * tpx + screen_w * .5f;
    if (sy) *sy = (wy - cy) * tpx + screen_h * .5f;
}
