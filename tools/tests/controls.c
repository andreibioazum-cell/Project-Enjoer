/* Физика/мультитач и контракты сцены/HUD. Рендер и звук — шпионы API. */
#include "rbx/rbx_internal.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int screen_w = 400, screen_h = 800;
double dt = 0;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __func__, __LINE__, #x); exit(1); } } while (0)
#define CLOSE(a,b) CHECK(fabsf((a) - (b)) < .0003f)
static float scene_x, scene_y, scene_z, scene_yaw, scene_pitch;
static int box_calls, scene_scale, black_knob, self_label, jump_label, sounds;

int snd_load(const char *name) { (void)name; return 1; }
int snd_play(const char *name) { (void)name; sounds++; return 1; }
void ds_log(const char *format, ...) { (void)format; }
int rbx3d_begin(Buffer *b, int sc, float x, float y, float z, float yaw, float pitch, float fov) {
    (void)b; (void)fov;
    scene_x = x; scene_y = y; scene_z = z; scene_yaw = yaw; scene_pitch = pitch;
    scene_scale = sc; box_calls = 0;
    return 1;
}
void rbx3d_sky(uint32_t a, uint32_t b) { (void)a; (void)b; }
void rbx3d_end(void) {}
int rbx3d_project(float x, float y, float z, float *sx, float *sy) {
    (void)x; (void)y; (void)z; (void)sx; (void)sy; return 0;
}
void rbx3d_box(float x, float y, float z, float hx, float hy, float hz, float yaw, uint32_t color) {
    (void)x; (void)y; (void)z; (void)hx; (void)hy; (void)hz; (void)yaw; (void)color;
    box_calls++;
}
void rect(float x, float y, float w, float h, uint32_t c) {
    (void)x; (void)y; (void)w; (void)h; (void)c;
}
void roundrect(float x, float y, float w, float h, float r, uint32_t c) {
    (void)x; (void)y; (void)w; (void)h; (void)r; (void)c;
}
void ring(float x, float y, float r, float th, uint32_t c) {
    (void)x; (void)y; (void)r; (void)th; (void)c;
}
void circle(float x, float y, float r, uint32_t c) {
    float jr;
    (void)x; (void)y;
    rbx_input_joy_geom(NULL, NULL, &jr);
    if (fabsf(r - jr * .38f) < .001f && c == 0xFF000000u) black_knob++;
}
int text_width(const char *s) { return (int)strlen(s) * 12; }
void text_scaled(const char *s, float x, float y, uint32_t c, float scale) {
    (void)x; (void)y; (void)c; (void)scale;
    if (!strcmp(s, "Ты")) self_label++;
    if (!strcmp(s, "прыжок")) jump_label++;
}

typedef struct { float x, y, z; } Pos;
static Pos position(void) {
    Pos p;
    rbx_player_pos(&p.x, &p.y, &p.z, NULL, NULL);
    return p;
}
static float distance(Pos a, Pos b) {
    float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return sqrtf(x * x + y * y + z * z);
}
static void fresh_player(void) {
    rbx_cancel_input(); rbx_player_spawn(); rbx_input_layout();
}
static void aim(float yaw, float pitch) {
    float cy, cp;
    rbx_camera_angles(&cy, &cp);
    rbx_camera_look((yaw - cy) * screen_w / 3.4f, (cp - pitch) * screen_h / 2.6f);
}

static void test_multitouch_flight(void) {
    /* До world_build нет препятствий: проверяем сам вектор движения. */
    fresh_player();
    Pos start = position();
    for (int i = 0; i < 100; i++) rbx_player_update(.016f);
    CLOSE(distance(start, position()), 0); /* отпущенный джойстик = зависание */
    float cx, cy, r, jx, jy, yaw, pitch, yaw_before, pitch_before;
    rbx_input_joy_geom(&cx, &cy, &r);
    rbx_camera_angles(&yaw_before, &pitch_before);
    rbx_input_touch(cx, cy, 0, 37);
    rbx_input_touch(cx + r * .05f, cy, 2, 37);
    rbx_input_joy(&jx, &jy); CLOSE(jx, 0); CLOSE(jy, 0);
    rbx_input_touch(cx, cy - r, 2, 37);
    rbx_input_touch(300, 380, 0, 1001);
    rbx_input_touch(340, 180, 2, 1001);
    rbx_input_joy(&jx, &jy); CLOSE(jx, 0); CLOSE(jy, -1);
    rbx_camera_angles(&yaw, &pitch);
    CHECK(yaw > yaw_before && pitch > pitch_before && pitch > 0);
    rbx_player_update(.05f);
    Pos moved = position();
    CLOSE(moved.x - start.x, sinf(yaw) * cosf(pitch) * 13.5f * .05f);
    CLOSE(moved.y - start.y, sinf(pitch) * 13.5f * .05f);
    CLOSE(moved.z - start.z, cosf(yaw) * cosf(pitch) * 13.5f * .05f);
    CLOSE(distance(start, moved), 13.5f * .05f);

    /* Третий палец не крадёт роли и не отпускает чужой джойстик. */
    rbx_input_touch(350, 600, 0, 2000);
    rbx_input_touch(10, 10, 2, 2000);
    rbx_input_touch(10, 10, 1, 2000);
    float new_yaw, new_pitch;
    rbx_camera_angles(&new_yaw, &new_pitch); CLOSE(new_yaw, yaw); CLOSE(new_pitch, pitch);
    rbx_input_touch(340, 180, 1, 1001);
    rbx_input_joy(NULL, &jy); CLOSE(jy, -1);
    rbx_player_update(.05f); CHECK(distance(moved, position()) > .67f);
    rbx_input_touch(cx, cy - r, 1, 37);
    start = position(); rbx_player_update(.05f); CLOSE(distance(start, position()), 0);

    /* Свайп, начавшийся слева вне стика, не крутит камеру. */
    rbx_input_touch(20, 100, 0, 2); rbx_input_touch(130, 200, 2, 2);
    rbx_camera_angles(&new_yaw, &new_pitch); CLOSE(new_yaw, yaw); CLOSE(new_pitch, pitch);
    rbx_input_touch(130, 200, 1, 2);
    puts("PASS two-finger flight/look, arbitrary pointer IDs, deadzone, release and hover");
}

static void test_gesture_lifetime(void) {
    fresh_player();
    float cx, cy, r, jy, yaw, pitch, y2, p2;
    rbx_input_joy_geom(&cx, &cy, &r);
    rbx_input_touch(cx, cy, 0, 50);
    rbx_input_touch(320, 380, 0, 70);
    rbx_camera_angles(&yaw, &pitch);
    rbx_input_touch(360, cy - r, 2, 50); /* джойстик пересёк середину */
    rbx_camera_angles(&y2, &p2); CLOSE(y2, yaw); CLOSE(p2, pitch);
    rbx_input_touch(100, 350, 2, 70); /* обзор пересёк середину */
    rbx_camera_angles(&y2, &p2); CHECK(fabsf(y2 - yaw) > .1f);
    rbx_input_touch(0, 0, 3, -1);
    rbx_input_joy(NULL, &jy); CLOSE(jy, 0);
    rbx_input_touch(50, 300, 2, 70);
    rbx_camera_angles(&yaw, &pitch); CLOSE(y2, yaw); CLOSE(p2, pitch);

    rbx_input_touch(cx, cy - r, 0, 99);
    rbx_key("w", 1); rbx_key("ArrowRight", 1);
    rbx_cancel_input();
    Pos start = position(); rbx_player_update(.05f);
    CLOSE(distance(start, position()), 0);
    rbx_camera_angles(&y2, &p2); CLOSE(y2, yaw); CLOSE(p2, pitch);

    rbx_input_touch(cx, cy - r, 0, 99);
    screen_w = 800; screen_h = 400; rbx_input_layout();
    rbx_input_joy(NULL, &jy); CLOSE(jy, 0);
    screen_w = 400; screen_h = 800; rbx_input_layout();
    puts("PASS gesture roles stay captured; cancel, focus loss and resize clear held input");
}

static void test_keyboard_and_camera(void) {
    fresh_player(); aim(.5f, .7f);
    Pos start = position(); rbx_key("w", 1); rbx_key("d", 1); rbx_player_update(.05f);
    CLOSE(distance(start, position()), 13.5f * .05f);
    fresh_player(); aim(0, 0);
    start = position(); rbx_key("space", 1); rbx_player_update(.05f);
    CLOSE(position().y - start.y, 13.5f * .05f);
    rbx_key("space", 0); rbx_key("Shift", 1); rbx_player_update(.05f);
    CLOSE(distance(start, position()), 0);
    rbx_cancel_input();
    rbx_camera_look(1e7f, -1e7f);
    float yaw, pitch;
    rbx_camera_angles(&yaw, &pitch);
    CHECK(fabsf(yaw) <= 3.142f && pitch > 1.4f && pitch < 1.5f);
    rbx_camera_look(-1e7f, 1e7f);
    rbx_camera_angles(&yaw, &pitch);
    CHECK(fabsf(yaw) <= 3.142f && pitch < -1.4f && pitch > -1.5f);
    puts("PASS normalized flight speed, rise/sink keys, bounded camera pitch/yaw");
}

static void test_world_and_collect(void) {
    rbx_world_build(); fresh_player(); aim(0, 0);
    rbx_key("Shift", 1);
    for (int i = 0; i < 20; i++) rbx_player_update(.05f);
    CLOSE(position().y, .28f); /* верх спавна, без проваливания */
    rbx_key("Shift", 0); rbx_key("s", 1);
    for (int i = 0; i < 40; i++) rbx_player_update(.05f);
    Pos p = position();
    CLOSE(p.x, 0); CHECK(p.z >= -6.081f && p.z < -5.9f);
    rbx_cancel_input();
    Pos start = p; rbx_player_update(.05f); CLOSE(distance(start, position()), 0);

    rbx_world_collect_reset();
    const RbxCoin *coins = rbx_world_coins(NULL);
    int sound_before = sounds;
    rbx_world_collect(coins[0].x, coins[0].y - RBX_PLAYER_EYE_HEIGHT, coins[0].z);
    CHECK(rbx_world_caught() == 1 && coins[0].taken && sounds == sound_before + 1);
    rbx_world_collect(coins[0].x, coins[0].y - RBX_PLAYER_EYE_HEIGHT, coins[0].z);
    CHECK(rbx_world_caught() == 1 && sounds == sound_before + 1);
    rbx_world_collect_reset(); CHECK(rbx_world_caught() == 0 && !coins[0].taken);
    rbx_world_collect(coins[0].x + 3, coins[0].y - 2.5f, coins[0].z);
    CHECK(rbx_world_caught() == 0);
    puts("PASS solid-world collisions and eye-height coin collection without duplicate rewards");
}

static void test_scene_hud_and_reset(void) {
    init(NULL);
    int nbox, ncoin;
    rbx_world_boxes(&nbox); rbx_world_coins(&ncoin);
    Pos p = position(); float yaw, pitch;
    rbx_camera_angles(&yaw, &pitch);
    rbx_scene_draw(NULL);
    CLOSE(scene_x, p.x); CLOSE(scene_y, p.y + RBX_PLAYER_EYE_HEIGHT); CLOSE(scene_z, p.z);
    CLOSE(scene_yaw, yaw); CLOSE(scene_pitch, pitch);
    CHECK(scene_scale == 1 && box_calls == nbox + ncoin + MAX_BOT * 6);
    rbx_hud_draw(); CHECK(black_knob == 1 && self_label == 0 && jump_label == 0);
    screen_w = 1080; screen_h = 2400; rbx_input_layout();
    rbx_scene_draw(NULL); CHECK(scene_scale == 2);
    const int sizes[][2] = {{400, 800}, {1080, 2400}, {1280, 720}, {320, 480}};
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(*sizes); i++) {
        screen_w = sizes[i][0]; screen_h = sizes[i][1]; rbx_input_layout();
        float x, y, r; rbx_input_joy_geom(&x, &y, &r);
        CHECK(x - r > 0 && x + r < screen_w * .5f && y - r > 0 && y + r < screen_h);
    }
    rbx_key("w", 1); rbx_t_abs = 100; reset();
    CHECK(rbx_t_abs == 0 && rbx_join_t == 0);
    p = position(); dt = -.1; update(); CLOSE(distance(p, position()), 0);
    CHECK(rbx_t_abs == 0);
    dt = .05; update(); CLOSE(distance(p, position()), 0);
    puts("PASS first-person eye camera, hidden self avatar/label, black joystick, HUD layout and reset");
}

int main(void) {
    test_multitouch_flight();
    test_gesture_lifetime();
    test_keyboard_and_camera();
    test_world_and_collect();
    test_scene_hud_and_reset();
    return 0;
}
