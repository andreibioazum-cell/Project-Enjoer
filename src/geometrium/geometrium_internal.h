/* Internal API of the voxel game. Cell coordinates are half-block integers. */
#ifndef GEOMETRIUM_INTERNAL_H
#define GEOMETRIUM_INTERNAL_H
#include "geometrium/geometrium.h"
#define GEOMETRIUM_PLAYER_RADIUS .30f
#define GEOMETRIUM_PLAYER_HEIGHT 1.80f
#define GEOMETRIUM_PLAYER_EYE_HEIGHT 1.62f
#define GEOMETRIUM_FOG_START 32.0f
#define GEOMETRIUM_FOG_END 64.0f
#define GEOMETRIUM_FAR_Z 80.0f
#define GEOMETRIUM_WORLD_SEED 20260905u

enum { BLOCK_AIR,BLOCK_GRASS,BLOCK_DIRT,BLOCK_STONE,BLOCK_SAND,BLOCK_WATER,BLOCK_LOG,BLOCK_LEAVES,BLOCK_COUNT };
/* WORLD_RADIUS is the visible chunk radius; one ring of chunks is prefetched
 * beyond it (CACHE_RADIUS) so the fog never exposes an unmeshed horizon. */
enum { CHUNK_SIZE=16,WORLD_HEIGHT=64,WATER_LEVEL=8,WORLD_RADIUS=4 };
enum { ACTION_BREAK,ACTION_PLACE,ACTION_COUNT,HOTBAR_SLOTS=6 };
typedef struct { int x,y,z,nx,ny,nz,block;float distance; } GeometriumHit;

int geometrium3d_begin(Buffer *,int,float,float,float,float,float,float);
void geometrium3d_sky(uint32_t top,uint32_t bottom);
void geometrium3d_fog(float start,float end);
void geometrium3d_surface(int sx,int sy,int sz,int u,int v,int face,int block,const unsigned char light[4]);
void geometrium3d_segment(float x,float y,float z,float x2,float y2,float z2,uint32_t color);
int geometrium3d_visible(float x,float y,float z,float hx,float hy,float hz);
void geometrium3d_end(void);
int geometrium_materials_load(AAssetManager *assets);
const Image *geometrium_material_icon(int block);

void geometrium_terrain_seed(uint32_t seed);
int geometrium_terrain_height(int x,int z);
int geometrium_terrain_block(int x,int y,int z);
void geometrium_terrain_chunk(int cx,int cz,unsigned char *blocks);
void geometrium_world_build(uint32_t seed);
void geometrium_world_update(float x,float z);
void geometrium_world_draw(void);
int geometrium_world_cell(int sx,int sy,int sz);
int geometrium_cell_solid(int sx,int sy,int sz);
int geometrium_world_set(int sx,int sy,int sz,int block);
void geometrium_world_stats(int *chunks,int *quads);
int geometrium_world_pending(void);
float geometrium_world_distance(void);
float geometrium_world_light(float x,float y,float z);
int geometrium3d_face_visible(int face,float plane);
void geometrium_water_update(float d);

void geometrium_player_spawn(void);
void geometrium_player_update(float d);
void geometrium_player_pos(float *x,float *y,float *z);
int geometrium_player_overlaps(int sx,int sy,int sz);
void geometrium_player_jump(int down);
int geometrium_player_flying(void);
int geometrium_player_grounded(void);
void geometrium_player_toggle_flight(void);
void geometrium_camera_look(float dx,float dy);
void geometrium_camera_angles(float *yaw,float *pitch);
void geometrium_key_state(const char *name,int down);
void geometrium_key_reset(void);

int geometrium_raycast(float x,float y,float z,float dx,float dy,float dz,float reach,GeometriumHit *hit);
void geometrium_actions_reset(void);
void geometrium_action_hold(int action,int down,int source); /* 0 keyboard, 1 touch */
void geometrium_action_cancel(int action,int source);
void geometrium_action_pulse(int action);
void geometrium_actions_update(float d);
int geometrium_action_apply(int action,const GeometriumHit *hit);
int geometrium_target(GeometriumHit *hit);
void geometrium_target_draw(void);
void geometrium_select(int index);
int geometrium_selected(void);
int geometrium_slot_block(int index);

void geometrium_input_layout(void);
void geometrium_input_reset(void);
void geometrium_input_joy(float *x,float *y);
void geometrium_input_joy_geom(float *x,float *y,float *r);
void geometrium_input_jump_geom(float *x,float *y,float *r);
void geometrium_input_flight_geom(float *x,float *y,float *w,float *h);
void geometrium_input_action_geom(int action,float *x,float *y,float *r);
void geometrium_input_slot_geom(int index,float *x,float *y,float *size);
void geometrium_input_touch(float x,float y,int action,int pointer_id);
void geometrium_scene_draw(Buffer *buffer);
void geometrium_hud_draw(void);
void geometrium_hand_reset(void);
void geometrium_hand_update(float d);
void geometrium_hand_swing(void);
void geometrium_hand_equip(void);
void geometrium_hand_draw(void);
void geometrium_perf_reset(void);
void geometrium_perf_frame(double interval);
float geometrium_fps(void);
int geometrium_render_scale(int width,int height);
void geometrium_render_time(double elapsed);
#endif
