/* Internal API of the voxel game. Cell coordinates are half-block integers. */
#ifndef RBX_INTERNAL_H
#define RBX_INTERNAL_H
#include "rbx/rbx.h"
#define RBX_PLAYER_RADIUS .30f
#define RBX_PLAYER_HEIGHT 1.80f
#define RBX_PLAYER_EYE_HEIGHT 1.62f
#define RBX_FOG_START 48.0f
#define RBX_FOG_END 96.0f
#define RBX_FAR_Z 112.0f
#define RBX_WORLD_SEED 20260905u

enum { BLOCK_AIR,BLOCK_GRASS,BLOCK_DIRT,BLOCK_STONE,BLOCK_SAND,BLOCK_WATER,BLOCK_LOG,BLOCK_LEAVES,BLOCK_COUNT };
enum { CHUNK_SIZE=16,WORLD_HEIGHT=64,WATER_LEVEL=8,WORLD_RADIUS=6 };
enum { ACTION_BREAK,ACTION_PLACE,ACTION_COUNT,HOTBAR_SLOTS=6 };
typedef struct { int x,y,z,nx,ny,nz,block;float distance; } RbxHit;

int rbx3d_begin(Buffer *,int,float,float,float,float,float,float);
void rbx3d_sky(uint32_t top,uint32_t bottom);
void rbx3d_fog(float start,float end);
void rbx3d_surface(int sx,int sy,int sz,int u,int v,int face,int block);
void rbx3d_segment(float x,float y,float z,float x2,float y2,float z2,uint32_t color);
int rbx3d_visible(float x,float y,float z,float hx,float hy,float hz);
void rbx3d_end(void);
int rbx_materials_load(AAssetManager *assets);
const Image *rbx_material_icon(int block);

void rbx_terrain_seed(uint32_t seed);
int rbx_terrain_height(int x,int z);
int rbx_terrain_block(int x,int y,int z);
void rbx_terrain_chunk(int cx,int cz,unsigned char *blocks);
void rbx_world_build(uint32_t seed);
void rbx_world_update(float x,float z);
void rbx_world_draw(void);
int rbx_world_cell(int sx,int sy,int sz);
int rbx_cell_solid(int sx,int sy,int sz);
int rbx_world_set(int sx,int sy,int sz,int block);
void rbx_world_stats(int *chunks,int *quads);
int rbx_world_pending(void);
float rbx_world_distance(void);

void rbx_player_spawn(void);
void rbx_player_update(float d);
void rbx_player_pos(float *x,float *y,float *z);
int rbx_player_overlaps(int sx,int sy,int sz);
void rbx_player_jump(int down);
int rbx_player_flying(void);
int rbx_player_grounded(void);
void rbx_player_toggle_flight(void);
void rbx_camera_look(float dx,float dy);
void rbx_camera_angles(float *yaw,float *pitch);
void rbx_key_state(const char *name,int down);
void rbx_key_reset(void);

int rbx_raycast(float x,float y,float z,float dx,float dy,float dz,float reach,RbxHit *hit);
void rbx_actions_reset(void);
void rbx_action_hold(int action,int down,int source); /* 0 keyboard, 1 touch */
void rbx_action_cancel(int action,int source);
void rbx_action_pulse(int action);
void rbx_actions_update(float d);
int rbx_action_apply(int action,const RbxHit *hit);
int rbx_target(RbxHit *hit);
void rbx_target_draw(void);
void rbx_select(int index);
int rbx_selected(void);
int rbx_slot_block(int index);

void rbx_input_layout(void);
void rbx_input_reset(void);
void rbx_input_joy(float *x,float *y);
void rbx_input_joy_geom(float *x,float *y,float *r);
void rbx_input_jump_geom(float *x,float *y,float *r);
void rbx_input_flight_geom(float *x,float *y,float *w,float *h);
void rbx_input_action_geom(int action,float *x,float *y,float *r);
void rbx_input_slot_geom(int index,float *x,float *y,float *size);
void rbx_input_touch(float x,float y,int action,int pointer_id);
void rbx_scene_draw(Buffer *buffer);
void rbx_hud_draw(void);
void rbx_perf_reset(void);
void rbx_perf_frame(double interval);
float rbx_fps(void);
int rbx_render_scale(int width,int height);
void rbx_render_time(double elapsed);
#endif
