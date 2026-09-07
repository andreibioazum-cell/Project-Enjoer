/* Bounded, nearest-first chunk streaming. Never rebuild an entire row on movement. */
#include "rbx_world_internal.h"
#include <limits.h>

static RbxChunk chunks[CACHE_COUNT];
static int center_x,center_z;
static float draw_distance=32;
/* Ход луча к солнцу: на единицу вверх — 0.5 вправо и 0.75 к +Z (половины блока). */
#define SUN_DX .5f
#define SUN_DZ .75f
enum { HMAP_SIDE=CHUNK_SIZE*2, SHADOW_REACH=31, SUN_DEFAULT_TOP=WORLD_HEIGHT+2 };
static int sun_top=SUN_DEFAULT_TOP;
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
    for (int i=1;i<8;i++) if (e->cells[i]!=e->cells[0]) return BLOCK_PARTIAL;
    return e->cells[0];
}
int rbx_world_cell(int sx,int sy,int sz) {
    int x=rbx_floor_div(sx,2),y=rbx_floor_div(sy,2),z=rbx_floor_div(sz,2);
    const RbxEdit *e=rbx_edit_find(x,y,z);
    return e ? e->cells[sx-x*2+(sy-y*2)*2+(sz-z*2)*4] : base_block(x,y,z);
}
/* Карта высот чанка: по колонке на половину блока, правки учитываются сразу.
 * Чистые проходы по массиву блоков — без хэша правок на каждую ячейку. */
static void build_hmap(RbxChunk *c) {
    static const RbxEdit *map[CHUNK_SIZE*WORLD_HEIGHT*CHUNK_SIZE];
    const RbxEdit *first=rbx_edit_first(c->cx,c->cz);
    if (first) {
        memset(map,0,sizeof(map));
        for (const RbxEdit *e=first;e;e=rbx_edit_next(e))
            map[(e->y*CHUNK_SIZE+e->z-c->cz*CHUNK_SIZE)*CHUNK_SIZE+(e->x-c->cx*CHUNK_SIZE)]=e;
    }
    int hmax=0;
    for (int fz=0;fz<CHUNK_SIZE;fz++) for (int fx=0;fx<CHUNK_SIZE;fx++) {
        int sub[4]={0,0,0,0};
        for (int y=WORLD_HEIGHT-1;y>=0;y--) {
            if (sub[0]&&sub[1]&&sub[2]&&sub[3]) break;
            const RbxEdit *e=first ? map[(y*CHUNK_SIZE+fz)*CHUNK_SIZE+fx] : NULL;
            unsigned char base=c->blocks[(y*CHUNK_SIZE+fz)*CHUNK_SIZE+fx];
            int solid=base!=BLOCK_AIR && base!=BLOCK_WATER;
            for (int cc=0;cc<2;cc++) for (int a=0;a<2;a++) {
                int s=cc*2+a;
                if (sub[s]) continue;
                if (e) {
                    /* верхняя половина ячейки — индекс +2, нижняя — +0 */
                    if (e->cells[a+2+4*cc]) sub[s]=y*2+2;
                    else if (e->cells[a+4*cc]) sub[s]=y*2+1;
                } else if (solid) sub[s]=y*2+2;
            }
        }
        for (int s=0;s<4;s++) {
            c->hmap[(fz*2+(s>>1))*HMAP_SIDE+fx*2+(s&1)]=(short)sub[s];
            if (sub[s]>hmax) hmax=sub[s];
        }
    }
    c->hmax=hmax;c->hmap_valid=1;
    if (hmax>sun_top) sun_top=hmax;
}
static int col_top(int sx,int sz) {
    int cx=rbx_floor_div(sx,HMAP_SIDE),cz=rbx_floor_div(sz,HMAP_SIDE);
    RbxChunk *c=slot(cx,cz);
    if (matches(c,cx,cz)&&c->hmap_valid)
        return c->hmap[(sz-cz*HMAP_SIDE)*HMAP_SIDE+(sx-cx*HMAP_SIDE)];
    /* Дальняя граница кэша: только рельеф без деревьев и правок —
       эти колонки лежат в тумане за пределами отрисовки. */
    return rbx_terrain_height(rbx_floor_div(sx,2),rbx_floor_div(sz,2))*2+2;
}
/* Есть ли твёрдая ячейка в колонке между высотами y0..y1 (половины блока).
 * Верхняя ячейка не занята, если луч покидает колонку ровно по её грани. */
static int column_blocked(int sx,int sz,float y0,float y1) {
    int lo=(int)y0,hi=(int)y1;
    if (y1<=(float)hi) hi--;
    for (int cy=lo;cy<=hi;cy++) {
        if (cy>=WORLD_HEIGHT*2) break;
        int b=rbx_world_cell(sx,cy,sz);
        if (b!=BLOCK_AIR && b!=BLOCK_WATER) return 1;
    }
    return 0;
}
/* Освещена ли точка солнцем: 2D-DDA по колонкам вверх к солнцу.
 * Карта высот отсекает пустое небо за одно сравнение; колонки, куда луч
 * входит ниже их вершины, проверяются по реальным ячейкам — поэтому тень
 * под кроной дерева не залезает туда, где луч проходит под ветками. */
int rbx_sunlit(float x,float y,float z) {
    if (!isfinite(x+y+z)) return 1;
    float hx=x*2,hy=y*2,hz=z*2;
    if (fabsf(hx)>4.0e7f || fabsf(hz)>4.0e7f || hy<0 || hy>WORLD_HEIGHT*2) return 1;
    int sx=(int)floorf(hx),sz=(int)floorf(hz);
    float tx=((sx+1)-hx)/SUN_DX,tz=((sz+1)-hz)/SUN_DZ;
    const float step_x=1/SUN_DX,step_z=1/SUN_DZ;
    float top=(float)sun_top;
    if (hy<col_top(sx,sz) && column_blocked(sx,sz,hy,hy+(tx<tz?tx:tz))) return 0;
    for (int i=0;i<SHADOW_REACH;i++) {
        float t;
        /* при равенстве сначала X: касание угла колонки считается входом,
           как и в точном 3D-DDA — границы теней соседних граней согласованы */
        if (tx<=tz) { t=tx;tx+=step_x;sx++; }
        else { t=tz;tz+=step_z;sz++; }
        float ry=hy+t; /* на единицу t луч поднимается на одну половину блока */
        if (ry>=top) return 1;
        if (ry<col_top(sx,sz) && column_blocked(sx,sz,ry,ry+((tx<tz?tx:tz)-t))) return 0;
    }
    return 1;
}
/* Правка колонки: пересчёт одной записи карты высот, без обхода всего чанка. */
static void column_top_update(int sx,int sz) {
    int cx=rbx_floor_div(sx,HMAP_SIDE),cz=rbx_floor_div(sz,HMAP_SIDE);
    RbxChunk *c=slot(cx,cz);
    if (!matches(c,cx,cz)||!c->hmap_valid) return;
    int top=0;
    for (int sy=WORLD_HEIGHT*2-1;sy>=0;sy--) {
        int b=rbx_world_cell(sx,sy,sz);
        if (b!=BLOCK_AIR && b!=BLOCK_WATER) { top=sy+1;break; }
    }
    int index=(sz-cz*HMAP_SIDE)*HMAP_SIDE+(sx-cx*HMAP_SIDE);
    int old=c->hmap[index];
    c->hmap[index]=(short)top;
    if (top>sun_top) sun_top=top;
    if (top>c->hmax) c->hmax=top;
    else if (top<old && old>=c->hmax) {
        int m=0;
        for (int i=0;i<HMAP_SIDE*HMAP_SIDE;i++) if (c->hmap[i]>m) m=c->hmap[i];
        c->hmax=m;
    }
}
int rbx_cell_solid(int sx,int sy,int sz) {
    int b=rbx_world_cell(sx,sy,sz);return b!=BLOCK_AIR && b!=BLOCK_WATER;
}
static void invalidate(int cx,int cz) {
    RbxChunk *c=slot(cx,cz);
    if (matches(c,cx,cz)) c->dirty=1;
}
static void invalidate_at(int sx,int sz) {
    invalidate(rbx_floor_div(sx,HMAP_SIDE),rbx_floor_div(sz,HMAP_SIDE));
}
int rbx_world_set(int sx,int sy,int sz,int block) {
    if (sy<2 || sy>=WORLD_HEIGHT*2 || block<0 || block>=BLOCK_COUNT ||
        sx < -20000000 || sx>20000000 || sz < -20000000 || sz>20000000) return 0;
    int x=rbx_floor_div(sx,2),y=rbx_floor_div(sy,2),z=rbx_floor_div(sz,2);
    if (!rbx_edit_set(sx,sy,sz,block,base_block(x,y,z))) return 0;
    column_top_update(sx,sz);
    int cx=rbx_floor_div(x,CHUNK_SIZE),cz=rbx_floor_div(z,CHUNK_SIZE);
    invalidate(cx,cz);
    int lx=sx-cx*CHUNK_SIZE*2,lz=sz-cz*CHUNK_SIZE*2;
    if (lx==0) invalidate(cx-1,cz);
    if (lx==CHUNK_SIZE*2-1) invalidate(cx+1,cz);
    if (lz==0) invalidate(cx,cz-1);
    if (lz==CHUNK_SIZE*2-1) invalidate(cx,cz+1);
    /* Тень ложится от солнца в сторону -X/-Z и может зайти в соседние чанки. */
    invalidate_at(sx-SHADOW_REACH,sz);invalidate_at(sx,sz-SHADOW_REACH);
    invalidate_at(sx-SHADOW_REACH,sz-SHADOW_REACH);
    return 1;
}
static RbxChunk *ensure_data(int cx,int cz) {
    RbxChunk *c=slot(cx,cz);
    if (!matches(c,cx,cz)) {
        c->cx=cx;c->cz=cz;c->valid=1;c->ready=c->dirty=0;c->count=0;c->hmap_valid=0;
        rbx_terrain_chunk(cx,cz,c->blocks);
        build_hmap(c);
    }
    return c;
}
static void prepare(int cx,int cz) {
    RbxChunk *c=ensure_data(cx,cz);
    for (int dz=-1;dz<=1;dz++) for (int dx=-1;dx<=1;dx++) {
        if (!dx && !dz) continue;
        int x=cx+dx,z=cz+dz;
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
    for (int jobs=0;jobs<2;jobs++) { if (!worker() || app_now()-start>.0025) break; }
    float target=coverage(),d=(float)dt;
    if (!isfinite(d) || d<0) d=0;
    if (draw_distance>target) draw_distance=target; /* don't expose unmeshed edges */
    else draw_distance=fminf(target,draw_distance+fminf(d,.05f)*40);
}
void rbx_world_build(uint32_t seed) {
    rbx_terrain_seed(seed);rbx_edits_reset(seed);rbx_edits_load();
    center_x=center_z=0;draw_distance=32;sun_top=SUN_DEFAULT_TOP;
    for (int i=0;i<CACHE_COUNT;i++) chunks[i].valid=chunks[i].ready=chunks[i].dirty=chunks[i].count=0;
    for (int z=-3;z<=3;z++) for (int x=-3;x<=3;x++) ensure_data(x,z);
    for (int z=-2;z<=2;z++) for (int x=-2;x<=2;x++) prepare(x,z);
}
float rbx_world_distance(void) { return draw_distance; }
void rbx_world_draw(void) {
    for (int ring=0;ring<=WORLD_RADIUS;ring++) for (int dz=-ring;dz<=ring;dz++) for (int dx=-ring;dx<=ring;dx++) {
        if (abs(dx)!=ring && abs(dz)!=ring) continue;
        int cx=center_x+dx,cz=center_z+dz;RbxChunk *c=slot(cx,cz);
        if (!matches(c,cx,cz) || !c->ready || !c->count) continue;
        float ox=cx*CHUNK_SIZE,oz=cz*CHUNK_SIZE,hy=(c->max_y-c->min_y)*.25f;
        if (!rbx3d_visible(ox+8,c->min_y*.5f+hy,oz+8,8,hy,8)) continue;
        for (int i=0;i<c->count;i++) {
            const RbxQuad *f=&c->quads[i];
            int sx=cx*CHUNK_SIZE*2+f->x, sy=f->y, sz=cz*CHUNK_SIZE*2+f->z;
            float hx, hy, hz, cxq, cyq, czq;
            if (f->face<2) {
                hx=f->u*.25f; hz=f->v*.25f; hy=.02f;
                cxq=sx*.5f+hx; cyq=sy*.5f; czq=sz*.5f+hz;
            } else if (f->face<4) {
                hx=f->u*.25f; hy=f->v*.25f; hz=.02f;
                cxq=sx*.5f+hx; cyq=sy*.5f+hy; czq=sz*.5f;
            } else {
                hz=f->u*.25f; hy=f->v*.25f; hx=.02f;
                cxq=sx*.5f; cyq=sy*.5f+hy; czq=sz*.5f+hz;
            }
            if (!rbx3d_visible(cxq,cyq,czq,hx,hy,hz)) continue;
            rbx3d_surface(sx,sy,sz,f->u,f->v,f->face,f->block,f->shadow);
        }
    }
}
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
