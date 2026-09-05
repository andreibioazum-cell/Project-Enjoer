#ifndef RBX_WORLD_INTERNAL_H
#define RBX_WORLD_INTERNAL_H
#include "rbx_internal.h"
enum { BASE_CELLS=CHUNK_SIZE*CHUNK_SIZE*WORLD_HEIGHT, BLOCK_PARTIAL=255,
       CACHE_RADIUS=WORLD_RADIUS+1, CACHE_SIDE=CACHE_RADIUS*2+1, CACHE_COUNT=CACHE_SIDE*CACHE_SIDE };
/* Origin and extents are in half-block units; textures remain in full-block units. */
typedef struct { unsigned char x,y,z,u,v,face,block; } RbxQuad;
typedef struct {
    int cx,cz,valid,ready,dirty,min_y,max_y;
    unsigned char blocks[BASE_CELLS];
    RbxQuad *quads;
    int count,capacity;
} RbxChunk;
typedef struct { int x,y,z,next; unsigned char cells[8]; } RbxEdit;
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
