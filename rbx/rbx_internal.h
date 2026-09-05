/* Внутренний API блочного мира, рендера и управления. */
#ifndef RBX_INTERNAL_H
#define RBX_INTERNAL_H
#include "rbx/rbx.h"

#define RBX_PLAYER_RADIUS 0.30f
#define RBX_PLAYER_HEIGHT 1.80f
#define RBX_PLAYER_EYE_HEIGHT 1.62f
#define RBX_FOG_START 28.0f
#define RBX_FOG_END 64.0f
#define RBX_FAR_Z 80.0f
#define RBX_WORLD_SEED 20260905u

enum { BLOCK_AIR, BLOCK_GRASS, BLOCK_DIRT, BLOCK_STONE, BLOCK_SAND,
       BLOCK_WATER, BLOCK_LOG, BLOCK_LEAVES, BLOCK_COUNT };
enum { CHUNK_SIZE = 16, WORLD_HEIGHT = 40, WATER_LEVEL = 8, WORLD_RADIUS = 4 };

int rbx3d_begin(Buffer *b, int sc, float cx, float cy, float cz, float yaw, float pitch, float fov_deg);
void rbx3d_sky(uint32_t top, uint32_t bot);
void rbx3d_box(float x, float y, float z, float hx, float hy, float hz, float yaw, uint32_t color);
void rbx3d_block_face(float x, float y, float z, int face, int block);
int rbx3d_visible(float x, float y, float z, float hx, float hy, float hz);
void rbx3d_end(void);
int rbx3d_project(float x, float y, float z, float *sx, float *sy);

void rbx_terrain_seed(uint32_t seed);
int rbx_terrain_height(int x, int z);
int rbx_terrain_block(int x, int y, int z);
void rbx_terrain_chunk(int cx, int cz, unsigned char *blocks);
void rbx_world_build(uint32_t seed);
void rbx_world_update(float x, float z);
void rbx_world_draw(void);
int rbx_world_block(int x, int y, int z);
int rbx_world_solid(int x, int y, int z);
void rbx_world_stats(int *chunks, int *faces);

void rbx_player_spawn(void);
void rbx_player_update(float d);
void rbx_player_pos(float *x, float *y, float *z, float *yaw, float *walk);
void rbx_player_jump(int down);
int rbx_player_flying(void);
int rbx_player_grounded(void);
void rbx_player_toggle_flight(void);
void rbx_camera_look(float dx, float dy);
void rbx_camera_angles(float *yaw, float *pitch);
void rbx_key_state(const char *name, int down);
void rbx_key_reset(void);

void rbx_input_layout(void);
void rbx_input_reset(void);
void rbx_input_joy(float *jx, float *jy);
void rbx_input_joy_geom(float *cx, float *cy, float *r);
void rbx_input_jump_geom(float *cx, float *cy, float *r);
void rbx_input_flight_geom(float *x, float *y, float *w, float *h);
void rbx_input_touch(float x, float y, int action, int pointer_id);
void rbx_scene_draw(Buffer *buffer);
void rbx_hud_draw(void);
extern double rbx_t_abs;
#endif
