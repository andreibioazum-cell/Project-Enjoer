#ifndef RBX_WORLD_INTERNAL_H
#define RBX_WORLD_INTERNAL_H
#include "rbx_internal.h"
enum { BASE_CELLS=CHUNK_SIZE*CHUNK_SIZE*WORLD_HEIGHT, BLOCK_PARTIAL=255,
       CACHE_RADIUS=WORLD_RADIUS+1, CACHE_SIDE=CACHE_RADIUS*2+1, CACHE_COUNT=CACHE_SIDE*CACHE_SIDE,
       /* Вертикальные слои чанка: отсечение и мешинг не трогают пустые слои. */
       SLAB_CELLS=16, SLABS=WORLD_HEIGHT*2/SLAB_CELLS, SLAB_ROWS=SLAB_CELLS/2,
       /* Ячейка хранит блок в младших четырёх битах и уровень воды в старших.
        * Уровень считается в полуклетках: 14 — это семь блоков растекания. */
       CELL_BLOCK_MASK=15, WATER_MAX_FLOW=14 };
#define RBX_CELL_BLOCK(value) ((int)((value)&CELL_BLOCK_MASK))
#define RBX_CELL_LEVEL(value) ((int)((value)>>4))
#define RBX_CELL_VALUE(block,level) ((unsigned char)((block)|((level)<<4)))
/* Origin and extents are in half-block units; textures remain in full-block units. */
typedef struct { unsigned char x,y,z,u,v,face,block; } RbxQuad;
typedef struct {
    int cx,cz,valid,ready,dirty,min_y,max_y;
    unsigned char blocks[BASE_CELLS];
    RbxQuad *quads;
    int count,capacity;
    int slab[SLABS+1]; /* квады отсортированы по слоям: slab[s]..slab[s+1] */
} RbxChunk;
typedef struct { int x,y,z,next; unsigned char cells[8]; } RbxEdit;
int rbx_floor_div(int value,int divisor);
void rbx_edits_reset(uint32_t seed);
const RbxEdit *rbx_edit_find(int x,int y,int z);
const RbxEdit *rbx_edit_first(int cx,int cz);
const RbxEdit *rbx_edit_next(const RbxEdit *edit);
int rbx_world_set_value(int sx,int sy,int sz,int value);
int rbx_edit_set(int sx,int sy,int sz,int block,int original);
int rbx_edit_set_cell(int sx,int sy,int sz,int value,int original);
int rbx_edit_cell_value(int sx,int sy,int sz);
int rbx_edits_count(void);
int rbx_edits_save(void);
int rbx_edits_load(void);
int rbx_edits_dirty(void);
int rbx_world_uniform(int x,int y,int z);
void rbx_chunk_mesh(RbxChunk *chunk);
void rbx_world_draw_all(void);
int rbx_face_exposed(int block,int neighbor);
/* Растекание воды: уровни, очередь пересчёта и бюджет на кадр. */
void rbx_water_reset(void);
void rbx_water_touch(int sx,int sy,int sz);
int rbx_water_update(double budget);
int rbx_water_pending(void);
int rbx_water_level(int sx,int sy,int sz);
int rbx_water_stats(int *queued,int *changed);
#endif
