#ifndef RBX_WORLD_INTERNAL_H
#define RBX_WORLD_INTERNAL_H
#include "rbx_internal.h"
enum { BASE_CELLS=CHUNK_SIZE*CHUNK_SIZE*WORLD_HEIGHT, BLOCK_PARTIAL=255,
       CACHE_RADIUS=WORLD_RADIUS+1, CACHE_SIDE=CACHE_RADIUS*2+1, CACHE_COUNT=CACHE_SIDE*CACHE_SIDE };
/* Origin and extents are in half-block units; textures remain in full-block units. */
typedef struct { unsigned char x,y,z,u,v,face,block,light[4]; } RbxQuad;
typedef struct {
    int cx,cz,valid,ready,dirty,min_y,max_y;
    unsigned char blocks[BASE_CELLS];
    RbxQuad *quads;
    int count,capacity;
    unsigned char *light; /* half-cell skylight + one-cell halo, only up to local roofs */
    size_t light_capacity;
    int light_y,light_height,light_valid;
} RbxChunk;
typedef struct { int x,y,z,next; unsigned char cells[8],flow[8]; } RbxEdit;
enum { RBX_LIGHT_MAX=15,RBX_LIGHT_OPAQUE=128,RBX_WATER_REACH=7,RBX_WATER_FALLING=8 };
void rbx_light_bake(RbxChunk *chunk);
int rbx_light_cell(const RbxChunk *chunk,int x,int y,int z);
void rbx_light_face(const RbxChunk *chunk,const int origin[3],int u,int v,int face,unsigned char light[4]);
const unsigned char *rbx_world_chunk_blocks(int cx,int cz);
int rbx_world_active(int sx,int sz);
int rbx_world_water_set(int sx,int sy,int sz,int level); /* -1 = drained, 0 = source */
int rbx_water_level(int sx,int sy,int sz);
void rbx_water_reset(void);
void rbx_water_wake(int sx,int sy,int sz);
void rbx_water_activate(int cx,int cz);
int rbx_water_pending(void);
int rbx_water_last_work(void);
int rbx_edit_set_flow(int sx,int sy,int sz,int block,int flow,int original);
const RbxEdit *rbx_edit_at(int index);
int rbx_floor_div(int value,int divisor);
void rbx_edits_reset(uint32_t seed);
const RbxEdit *rbx_edit_find(int x,int y,int z);
const RbxEdit *rbx_edit_first(int cx,int cz);
const RbxEdit *rbx_edit_next(const RbxEdit *edit);
int rbx_edit_set(int sx,int sy,int sz,int block,int original);
int rbx_edits_count(void);
int rbx_edits_save(void);
int rbx_edits_load(void);
int rbx_edits_dirty(void);
int rbx_world_uniform(int x,int y,int z);
void rbx_chunk_mesh(RbxChunk *chunk);
int rbx_face_exposed(int block,int neighbor);
#endif
