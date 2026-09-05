/* Bounded, nearest-first chunk streaming. Never rebuild an entire row on movement.
 * Отрисовка идёт слоями: чанк → вертикальный слой → квад, поэтому скрытая
 * толща породы и стенки глубокой шахты не перебираются и не растеризуются. */
#include "rbx_world_internal.h"
#include <limits.h>

static RbxChunk chunks[CACHE_COUNT];
static int center_x,center_z;
static float draw_distance=32;
static int wrap(int x) { int n=x%CACHE_SIDE;return n<0 ? n+CACHE_SIDE : n; }
static RbxChunk *slot(int cx,int cz) { return &chunks[wrap(cz)*CACHE_SIDE+wrap(cx)]; }
static int matches(const RbxChunk *c,int cx,int cz) { return c->valid && c->cx==cx && c->cz==cz; }
static int base_block(int x,int y,int z) {
    if (y<0) return BLOCK_STONE;
    if (y>=WORLD_HEIGHT) return BLOCK_AIR;
    int cx=rbx_floor_div(x,CHUNK_SIZE),cz=rbx_floor_div(z,CHUNK_SIZE);
    RbxChunk *c=slot(cx,cz);
    if (!matches(c,cx,cz)) return rbx_terrain_block(x,y,z);
    return c->blocks[(y*CHUNK_SIZE+z-cz*CHUNK_SIZE)*CHUNK_SIZE+x-cx*CHUNK_SIZE];
}
int rbx_world_uniform(int x,int y,int z) {
    const RbxEdit *e=rbx_edit_find(x,y,z);
    if (!e) return base_block(x,y,z);
    /* Уровни воды внутри одного блока не делают его «частичным» для меша. */
    int block=RBX_CELL_BLOCK(e->cells[0]);
    for (int i=1;i<8;i++) if (RBX_CELL_BLOCK(e->cells[i])!=block) return BLOCK_PARTIAL;
    return block;
}
int rbx_world_cell(int sx,int sy,int sz) {
    int x=rbx_floor_div(sx,2),y=rbx_floor_div(sy,2),z=rbx_floor_div(sz,2);
    const RbxEdit *e=rbx_edit_find(x,y,z);
    return e ? RBX_CELL_BLOCK(e->cells[sx-x*2+(sy-y*2)*2+(sz-z*2)*4]) : base_block(x,y,z);
}
int rbx_cell_solid(int sx,int sy,int sz) {
    int b=rbx_world_cell(sx,sy,sz);return b!=BLOCK_AIR && b!=BLOCK_WATER;
}
static void invalidate(int cx,int cz) {
    RbxChunk *c=slot(cx,cz);
    if (matches(c,cx,cz)) c->dirty=1;
}
int rbx_world_set_value(int sx,int sy,int sz,int value) {
    if (sy<2 || sy>=WORLD_HEIGHT*2 || value<0 || value>255 ||
        RBX_CELL_BLOCK(value)>=BLOCK_COUNT || RBX_CELL_LEVEL(value)>WATER_MAX_FLOW ||
        sx < -20000000 || sx>20000000 || sz < -20000000 || sz>20000000) return 0;
    int x=rbx_floor_div(sx,2),y=rbx_floor_div(sy,2),z=rbx_floor_div(sz,2);
    if (!rbx_edit_set_cell(sx,sy,sz,value,base_block(x,y,z))) return 0;
    int cx=rbx_floor_div(x,CHUNK_SIZE),cz=rbx_floor_div(z,CHUNK_SIZE);
    invalidate(cx,cz);
    int lx=sx-cx*CHUNK_SIZE*2,lz=sz-cz*CHUNK_SIZE*2;
    if (lx==0) invalidate(cx-1,cz);
    if (lx==CHUNK_SIZE*2-1) invalidate(cx+1,cz);
    if (lz==0) invalidate(cx,cz-1);
    if (lz==CHUNK_SIZE*2-1) invalidate(cx,cz+1);
    return 1;
}
int rbx_world_set(int sx,int sy,int sz,int block) {
    if (block<0 || block>=BLOCK_COUNT) return 0;
    if (!rbx_world_set_value(sx,sy,sz,RBX_CELL_VALUE(block,0))) return 0;
    /* Любая правка может пустить воду или, наоборот, оставить её без питания. */
    rbx_water_touch(sx,sy,sz);
    return 1;
}
static RbxChunk *ensure_data(int cx,int cz) {
    RbxChunk *c=slot(cx,cz);
    if (!matches(c,cx,cz)) {
        c->cx=cx;c->cz=cz;c->valid=1;c->ready=c->dirty=0;c->count=0;
        for (int s=0;s<=SLABS;s++) c->slab[s]=0;
        rbx_terrain_chunk(cx,cz,c->blocks);
    }
    return c;
}
static void prepare(int cx,int cz) {
    RbxChunk *c=ensure_data(cx,cz);
    static const int d[4][2]={{1,0},{-1,0},{0,1},{0,-1}};
    for (int i=0;i<4;i++) {
        int x=cx+d[i][0],z=cz+d[i][1];
        if (abs(x-center_x)<=CACHE_RADIUS && abs(z-center_z)<=CACHE_RADIUS) ensure_data(x,z);
    }
    rbx_chunk_mesh(c);
}
static int worker(void) {
    /* Edited chunks take precedence over distant prefetch. */
    RbxChunk *edit=NULL;int best=INT_MAX;
    for (int i=0;i<CACHE_COUNT;i++) {
        RbxChunk *c=&chunks[i];int dx=c->cx-center_x,dz=c->cz-center_z;
        if (!c->valid || !c->dirty || abs(dx)>CACHE_RADIUS || abs(dz)>CACHE_RADIUS) continue;
        int distance=dx*dx+dz*dz;
        if (distance<best) {best=distance;edit=c;}
    }
    if (edit) { prepare(edit->cx,edit->cz);return 1; }
    for (int ring=0;ring<=CACHE_RADIUS;ring++) for (int dz=-ring;dz<=ring;dz++) for (int dx=-ring;dx<=ring;dx++) {
        if (abs(dx)!=ring && abs(dz)!=ring) continue;
        int cx=center_x+dx,cz=center_z+dz;RbxChunk *c=slot(cx,cz);
        if (!matches(c,cx,cz) || !c->ready) { prepare(cx,cz);return 1; }
    }
    return 0;
}
static float coverage(void) {
    int ready_radius=0;
    for (int r=0;r<=WORLD_RADIUS;r++) {
        for (int z=-r;z<=r;z++) for (int x=-r;x<=r;x++) {
            if (abs(x)!=r && abs(z)!=r) continue;
            RbxChunk *c=slot(center_x+x,center_z+z);
            if (!matches(c,center_x+x,center_z+z) || !c->ready) return fmaxf(12,ready_radius*CHUNK_SIZE);
        }
        ready_radius=r;
    }
    return RBX_FOG_END;
}
void rbx_world_update(float x,float z) {
    if (!isfinite(x+z) || fabsf(x)>10000000 || fabsf(z)>10000000) return;
    center_x=(int)floorf(x/CHUNK_SIZE);center_z=(int)floorf(z/CHUNK_SIZE);
    double start=app_now();
    /* Вода пересчитывается до мешинга: затронутые чанки попадают в очередь грязных. */
    rbx_water_update(.0015);
    for (int jobs=0;jobs<2;jobs++) { if (!worker() || app_now()-start>.0025) break; }
    float target=coverage(),d=(float)dt;
    if (!isfinite(d) || d<0) d=0;
    if (draw_distance>target) draw_distance=target; /* don't expose unmeshed edges */
    else draw_distance=fminf(target,draw_distance+fminf(d,.05f)*40);
}
void rbx_world_build(uint32_t seed) {
    rbx_terrain_seed(seed);rbx_edits_reset(seed);rbx_water_reset();rbx_edits_load();
    center_x=center_z=0;draw_distance=32;
    for (int i=0;i<CACHE_COUNT;i++) {
        chunks[i].valid=chunks[i].ready=chunks[i].dirty=chunks[i].count=0;
        for (int s=0;s<=SLABS;s++) chunks[i].slab[s]=0;
    }
    for (int z=-3;z<=3;z++) for (int x=-3;x<=3;x++) ensure_data(x,z);
    for (int z=-2;z<=2;z++) for (int x=-2;x<=2;x++) prepare(x,z);
}
float rbx_world_distance(void) { return draw_distance; }
static void draw_cache(int culled) {
    for (int ring=0;ring<=WORLD_RADIUS;ring++) for (int dz=-ring;dz<=ring;dz++) for (int dx=-ring;dx<=ring;dx++) {
        if (abs(dx)!=ring && abs(dz)!=ring) continue;
        int cx=center_x+dx,cz=center_z+dz;RbxChunk *c=slot(cx,cz);
        if (!matches(c,cx,cz) || !c->ready || !c->count) continue;
        float ox=cx*CHUNK_SIZE,oz=cz*CHUNK_SIZE;
        float top=c->max_y*.5f,bottom=c->min_y*.5f;
        /* Чанк целиком: один тест параллелепипеда вместо тысяч тестов квадов. */
        if (culled && !rbx3d_box_visible(ox+8,(top+bottom)*.5f,oz+8,8,(top-bottom)*.5f,8)) continue;
        int base_x=cx*CHUNK_SIZE*2,base_z=cz*CHUNK_SIZE*2;
        for (int s=0;s<SLABS;s++) {
            int from=c->slab[s],to=c->slab[s+1];
            if (from>=to) continue; /* пустой слой — ни одного квада не перебираем */
            if (culled) {
                int low=s*SLAB_CELLS,high=low+SLAB_CELLS;
                if (c->min_y>low) low=c->min_y;
                if (c->max_y<high) high=c->max_y;
                float y0=low*.5f,y1=high*.5f;
                if (!rbx3d_box_visible(ox+8,(y0+y1)*.5f,oz+8,8,(y1-y0)*.5f,8)) continue;
            }
            for (int i=from;i<to;i++) {
                const RbxQuad *f=&c->quads[i];
                int sx=base_x+f->x,sz=base_z+f->z;
                /* Задняя грань и квады вне пирамиды отсекаются до постройки вершин. */
                if (culled && !rbx3d_quad_visible(sx,f->y,sz,f->u,f->v,f->face)) continue;
                rbx3d_surface(sx,f->y,sz,f->u,f->v,f->face,f->block);
            }
        }
    }
}
void rbx_world_draw(void) { draw_cache(1); }
/* Эталон для проверки отсечения: те же квады без тестов видимости. */
void rbx_world_draw_all(void) { draw_cache(0); }
void rbx_world_stats(int *loaded,int *quads) {
    int n=0,q=0;
    for (int i=0;i<CACHE_COUNT;i++) {
        RbxChunk *c=&chunks[i];
        if (!c->valid || abs(c->cx-center_x)>CACHE_RADIUS || abs(c->cz-center_z)>CACHE_RADIUS) continue;
        n++;if(c->ready)q+=c->count;
    }
    if (loaded) *loaded=n;
    if (quads) *quads=q;
}
int rbx_world_pending(void) {
    int n=0;
    for (int z=-CACHE_RADIUS;z<=CACHE_RADIUS;z++) for (int x=-CACHE_RADIUS;x<=CACHE_RADIUS;x++) {
        RbxChunk *c=slot(center_x+x,center_z+z);
        if (!matches(c,center_x+x,center_z+z) || !c->ready || c->dirty) n++;
    }
    return n;
}
