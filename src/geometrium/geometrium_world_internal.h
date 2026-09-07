#ifndef GEOMETRIUM_WORLD_INTERNAL_H
#define GEOMETRIUM_WORLD_INTERNAL_H
#include "geometrium_internal.h"
enum { BASE_CELLS=CHUNK_SIZE*CHUNK_SIZE*WORLD_HEIGHT, BLOCK_PARTIAL=255,
       CACHE_RADIUS=WORLD_RADIUS+1, CACHE_SIDE=CACHE_RADIUS*2+1, CACHE_COUNT=CACHE_SIDE*CACHE_SIDE };
/* Origin and extents are in half-block units; textures remain in full-block units. */
typedef struct { unsigned char x,y,z,u,v,face,block,light[4]; } GeometriumQuad;
typedef struct {
    int cx,cz,valid,ready,dirty,min_y,max_y;
    unsigned char blocks[BASE_CELLS];
    GeometriumQuad *quads;
    int count,capacity,water_count; /* water quads render in a second pass */
    unsigned char *light; /* half-cell skylight + one-cell halo, only up to local roofs */
    size_t light_capacity;
    int light_y,light_height,light_valid;
} GeometriumChunk;
typedef struct { int x,y,z,next; unsigned char cells[8],flow[8]; } GeometriumEdit;
enum { GEOMETRIUM_LIGHT_MAX=15,GEOMETRIUM_LIGHT_OPAQUE=128,GEOMETRIUM_WATER_REACH=7,GEOMETRIUM_WATER_FALLING=8 };
void geometrium_light_bake(GeometriumChunk *chunk);
int geometrium_light_cell(const GeometriumChunk *chunk,int x,int y,int z);
void geometrium_light_face(const GeometriumChunk *chunk,const int origin[3],int u,int v,int face,unsigned char light[4]);
const unsigned char *geometrium_world_chunk_blocks(int cx,int cz);
int geometrium_world_active(int sx,int sz);
int geometrium_world_water_set(int sx,int sy,int sz,int level); /* -1 = drained, 0 = source */
int geometrium_water_level(int sx,int sy,int sz);
void geometrium_water_reset(void);
void geometrium_water_wake(int sx,int sy,int sz);
void geometrium_water_activate(int cx,int cz);
int geometrium_water_pending(void);
int geometrium_water_last_work(void);
int geometrium_edit_set_flow(int sx,int sy,int sz,int block,int flow,int original);
const GeometriumEdit *geometrium_edit_at(int index);
int geometrium_floor_div(int value,int divisor);
void geometrium_edits_reset(uint32_t seed);
const GeometriumEdit *geometrium_edit_find(int x,int y,int z);
const GeometriumEdit *geometrium_edit_first(int cx,int cz);
const GeometriumEdit *geometrium_edit_next(const GeometriumEdit *edit);
int geometrium_edit_set(int sx,int sy,int sz,int block,int original);
int geometrium_edits_count(void);
int geometrium_edits_save(void);
int geometrium_edits_load(void);
int geometrium_edits_dirty(void);
int geometrium_world_uniform(int x,int y,int z);
void geometrium_chunk_mesh(GeometriumChunk *chunk);
int geometrium_face_exposed(int block,int neighbor);
#endif
