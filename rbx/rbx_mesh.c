/* Greedy full-block faces plus only the exposed faces of edited half cells. */
#include "rbx_world_internal.h"

static const int normal[6][3]={{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{1,0,0},{-1,0,0}};
static int at(int x,int y,int z) { return (y*CHUNK_SIZE+z)*CHUNK_SIZE+x; }
int rbx_face_exposed(int b,int n) {
    return b!=BLOCK_AIR && (n==BLOCK_AIR || (n==BLOCK_WATER && b!=BLOCK_WATER));
}
static void emit(RbxChunk *c,const int origin[3],int u,int v,int face,int block) {
    if (c->count==c->capacity) {
        int capacity=c->capacity ? c->capacity*2 : 256;
        RbxQuad *p=realloc(c->quads,(size_t)capacity*sizeof(*p));
        if (!p) { app_fail("Недостаточно памяти для меша");return; }
        c->quads=p;c->capacity=capacity;
    }
    /* A horizontal cut inside grass reveals soil, not a second grass top. */
    if (block==BLOCK_GRASS && face==0 && origin[1]%2) block=BLOCK_DIRT;
    c->quads[c->count++]=(RbxQuad){origin[0],origin[1],origin[2],u,v,face,block};
    int top=origin[1]+(face>=2 ? v : 0);
    if (origin[1]<c->min_y) c->min_y=origin[1];
    if (top>c->max_y) c->max_y=top;
}
static void half_face(RbxChunk *c,int sx,int sy,int sz,int face,int block) {
    int n=rbx_world_cell(c->cx*CHUNK_SIZE*2+sx+normal[face][0],sy+normal[face][1],
                         c->cz*CHUNK_SIZE*2+sz+normal[face][2]);
    if (!rbx_face_exposed(block,n)) return;
    int p[3]={sx,sy,sz};
    for (int axis=0;axis<3;axis++) if (normal[face][axis]>0) p[axis]++;
    emit(c,p,1,1,face,block);
}
static void split_boundary(RbxChunk *c,const int p[3],int a,int ua,int va,int face,int b) {
    for (int v=0;v<2;v++) for (int u=0;u<2;u++) {
        int s[3]={p[0]*2,p[1]*2,p[2]*2};
        s[a]+=normal[face][a]>0; s[ua]+=u;s[va]+=v;
        half_face(c,s[0],s[1],s[2],face,b);
    }
}
void rbx_chunk_mesh(RbxChunk *c) {
    unsigned char resolved[BASE_CELLS];
    const unsigned char *blocks=c->blocks;
    const RbxEdit *first=rbx_edit_first(c->cx,c->cz);
    int ox=c->cx*CHUNK_SIZE,oz=c->cz*CHUNK_SIZE;
    if (first) {
        memcpy(resolved,c->blocks,sizeof(resolved));blocks=resolved;
        for (const RbxEdit *e=first;e;e=rbx_edit_next(e)) {
            int b=e->cells[0];
            for (int i=1;i<8;i++) if (e->cells[i]!=b) { b=BLOCK_PARTIAL;break; }
            resolved[at(e->x-ox,e->y,e->z-oz)]=(unsigned char)b;
        }
    }
    c->count=0;c->min_y=WORLD_HEIGHT*2;c->max_y=0;
    const int dims[3]={CHUNK_SIZE,WORLD_HEIGHT,CHUNK_SIZE};
    unsigned char mask[CHUNK_SIZE*WORLD_HEIGHT];
    for (int face=0;face<6;face++) {
        int a=face<2 ? 1 : face<4 ? 2 : 0;
        int ua=face<4 ? 0 : 2,va=face<2 ? 2 : 1;
        int width=dims[ua],height=dims[va];
        for (int slice=0;slice<dims[a];slice++) {
            memset(mask,0,(size_t)width*height);
            for (int v=0;v<height;v++) for (int u=0;u<width;u++) {
                int p[3];p[a]=slice;p[ua]=u;p[va]=v;
                int b=blocks[at(p[0],p[1],p[2])];
                if (!b || b==BLOCK_PARTIAL) continue;
                int neighbor[3]={p[0]+normal[face][0],p[1]+normal[face][1],p[2]+normal[face][2]};
                int inside=neighbor[a]>=0 && neighbor[a]<dims[a];
                int n=inside ? blocks[at(neighbor[0],neighbor[1],neighbor[2])] :
                    rbx_world_uniform(ox+neighbor[0],neighbor[1],oz+neighbor[2]);
                if (n==BLOCK_PARTIAL) split_boundary(c,p,a,ua,va,face,b);
                else if (rbx_face_exposed(b,n)) mask[v*width+u]=(unsigned char)b;
            }
            for (int v=0;v<height;v++) for (int u=0;u<width;) {
                int b=mask[v*width+u];
                if (!b) { u++;continue; }
                int w=1,h=1;
                while (u+w<width && mask[v*width+u+w]==b) w++;
                while (v+h<height) {
                    int same=1;
                    for (int k=0;k<w;k++) if (mask[(v+h)*width+u+k]!=b) {same=0;break;}
                    if (!same) break;
                    h++;
                }
                int p[3];p[a]=(slice+(normal[face][a]>0))*2;p[ua]=u*2;p[va]=v*2;
                emit(c,p,w*2,h*2,face,b);
                for (int row=0;row<h;row++) memset(mask+(v+row)*width+u,0,(size_t)w);
                u+=w;
            }
        }
    }
    for (const RbxEdit *e=first;e;e=rbx_edit_next(e)) {
        int x=e->x-ox,z=e->z-oz;
        if (resolved[at(x,e->y,z)]!=BLOCK_PARTIAL) continue;
        for (int i=0;i<8;i++) {
            int b=e->cells[i];if (!b) continue;
            for (int f=0;f<6;f++) half_face(c,x*2+(i&1),e->y*2+((i>>1)&1),z*2+(i>>2),f,b);
        }
    }
    c->ready=1;c->dirty=0;
}
