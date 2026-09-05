/* rbx/rbx_internal.h — внутренние связи модуля 3D-плейса.
 * Публичный ввод/сброс — rbx_key() и rbx_cancel_input() из rbx.h. */
#ifndef RBX_INTERNAL_H
#define RBX_INTERNAL_H

#include "rbx/rbx.h"

/* ── rbx_render.c: софтверный 3D ─────────────────────────────────────── */
int rbx3d_begin(Buffer *b, int sc, float cx, float cy, float cz, float yaw, float pitch, float fov_deg);
void rbx3d_sky(uint32_t top, uint32_t bot);
void rbx3d_box(float x, float y, float z, float hx, float hy, float hz, float yaw, uint32_t color);
void rbx3d_end(void);
int rbx3d_project(float x, float y, float z, float *sx, float *sy);

/* ── rbx_world.c: мир, монеты, боты ──────────────────────────────────── */
enum { MAT_SOLID = 0, MAT_LAVA = 1, MAT_WATER = 2, MAT_BOUNCE = 3, MAT_SKIP = 4 };
enum { MAX_BOX = 240, MAX_COIN = 12, MAX_BOT = 3 };
#define RBX_PLAYER_RADIUS 0.82f
#define RBX_PLAYER_HEIGHT 4.9f
#define RBX_PLAYER_EYE_HEIGHT 4.6f

typedef struct {
    float x, y, z, hx, hy, hz;
    uint32_t color;
    int mat;
} RbxBox;

typedef struct {
    float x, y, z;
    int taken;
} RbxCoin;

typedef struct {
    float x, y, z, yaw, phase;
    float wp[4][2];
    int nwp, i, dir;
    uint32_t head, torso, pants;
} RbxBot;

void rbx_world_build(void);
const RbxBox *rbx_world_boxes(int *count);
const RbxCoin *rbx_world_coins(int *count);
const RbxBot *rbx_world_bots(void); /* всегда MAX_BOT штук */
void rbx_bots_update(float d);
void rbx_world_collect(float px, float py, float pz);
void rbx_world_collect_reset(void);
int rbx_world_caught(void);
int rbx_world_coin_count(void);

/* ── rbx_player.c: игрок, физика, камера, клавиатура ─────────────────── */
void rbx_player_spawn(void);
void rbx_player_update(float d);
void rbx_player_pos(float *x, float *y, float *z, float *yaw, float *walk);
void rbx_camera_look(float dx, float dy);
void rbx_camera_angles(float *yaw, float *pitch);
void rbx_key_state(const char *name, int down);
void rbx_key_reset(void);

/* ── rbx_input.c: тач, джойстик полёта, свайп-обзор ───────────────────────── */
void rbx_input_layout(void);
void rbx_input_reset(void);
void rbx_input_joy(float *jx, float *jy);
void rbx_input_joy_geom(float *cx, float *cy, float *r);
void rbx_input_touch(float x, float y, int action, int pointer_id);

/* ── rbx_scene.c / rbx_hud.c: отрисовка ──────────────────────────────── */
void rbx_scene_draw(Buffer *buffer);
void rbx_hud_draw(void);

/* ── rbx_game.c: время сессии ────────────────────────────────────────── */
extern double rbx_t_abs;
extern double rbx_join_t;

#endif
