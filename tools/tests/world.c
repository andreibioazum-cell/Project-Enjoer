/* Deterministic generation, streaming and exact greedy / half-cell coverage. */
#include "rbx/rbx_world_internal.h"
#include <stdio.h>
#define CHECK(x) do {if(!(x)){fprintf(stderr,"%s:%d: %s\n",__func__,__LINE__,#x);exit(1);}}while(0)
int rbx3d_visible(float x,float y,float z,float hx,float hy,float hz) {(void)x;(void)y;(void)z;(void)hx;(void)hy;(void)hz;return 0;}
void rbx3d_surface(int x,int y,int z,int u,int v,int face,int block) {(void)x;(void)y;(void)z;(void)u;(void)v;(void)face;(void)block;}
static void test_chunks(void) {
    unsigned char a[BASE_CELLS],b[BASE_CELLS];int types[BLOCK_COUNT]={0},low=100,high=0;
    const int coords[][2]={{0,0},{1,-1},{-1,1},{-3,-2},{2,3},{3,-4}};
    rbx_terrain_seed(RBX_WORLD_SEED);
    for(unsigned i=0;i<sizeof(coords)/sizeof(*coords);i++) {
        int cx=coords[i][0],cz=coords[i][1];
        rbx_terrain_chunk(cx,cz,a);rbx_terrain_chunk(cx,cz,b);CHECK(!memcmp(a,b,sizeof(a)));
        for(int z=0;z<CHUNK_SIZE;z++)for(int x=0;x<CHUNK_SIZE;x++) {
            int wx=cx*CHUNK_SIZE+x,wz=cz*CHUNK_SIZE+z,h=rbx_terrain_height(wx,wz);
            if(h<low)low=h;
            if(h>high)high=h;
            for(int y=0;y<WORLD_HEIGHT;y++) {
                int block=a[(y*CHUNK_SIZE+z)*CHUNK_SIZE+x];CHECK(block<BLOCK_COUNT);types[block]++;
                if(x==0||x==15||z==0||z==15||(x%5==0&&z%5==0))CHECK(block==rbx_terrain_block(wx,y,wz));
            }
        }
    }
    CHECK(high-low>=5);for(int i=0;i<BLOCK_COUNT;i++)CHECK(types[i]>0);
    rbx_terrain_chunk(3,-4,a);rbx_terrain_seed(RBX_WORLD_SEED+1);rbx_terrain_chunk(3,-4,b);CHECK(memcmp(a,b,sizeof(a))!=0);
    puts("PASS deterministic seeds, negative coordinates, tree seams and every PNG terrain material");
}
static uint32_t fingerprint(void) {
    uint32_t hash=2166136261u;
    for(int z=-45;z<60;z+=3)for(int x=-45;x<60;x+=3)for(int y=4;y<30;y+=3)
        hash=(hash^(uint32_t)rbx_world_cell(x*2,y*2,z*2))*16777619u;
    return hash;
}
static void warm(float x,float z) {
    rbx_world_update(x,z);
    for(int i=0;rbx_world_pending() && i<500;i++)rbx_world_update(x,z);
    CHECK(!rbx_world_pending());
    for(int i=0;i<180;i++)rbx_world_update(x,z);
    CHECK(rbx_world_distance()==RBX_FOG_END);
}
static void test_streaming(void) {
    rbx_world_build(RBX_WORLD_SEED);uint32_t original=fingerprint();
    int n,q;rbx_world_stats(&n,&q);CHECK(n<CACHE_COUNT && rbx_world_pending()>0);
    warm(8.5f,8.5f);rbx_world_stats(&n,&q);CHECK(n==CACHE_COUNT && q>0 && q<CACHE_COUNT*2500);
    CHECK(rbx_cell_solid(16,24,16) && !rbx_cell_solid(16,26,16));
    CHECK(rbx_world_cell(2,-1,2)==BLOCK_STONE && rbx_world_cell(2,WORLD_HEIGHT*2,2)==BLOCK_AIR);
    const int positions[][2]={{80,0},{-80,-96},{1024,-1024},{-10000,20000},{8,8}};
    for(unsigned i=0;i<sizeof(positions)/sizeof(*positions);i++) {
        int x=positions[i][0],z=positions[i][1];warm(x+.5f,z+.5f);rbx_world_stats(&n,&q);
        CHECK(n==CACHE_COUNT && q>0 && q<CACHE_COUNT*2500);
        for(int dx=-17;dx<=17;dx+=3)for(int dz=-17;dz<=17;dz+=3)for(int y=5;y<30;y+=4)
            CHECK(rbx_world_cell((x+dx)*2,y*2,(z+dz)*2)==rbx_terrain_block(x+dx,y,z+dz));
    }
    CHECK(original==fingerprint());rbx_world_build(RBX_WORLD_SEED);CHECK(original==fingerprint());
    puts("PASS bounded 15x15 cache, incremental nearest-first streaming, 96-block range and return-trip determinism");
}
static const int normals[6][3]={{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{1,0,0},{-1,0,0}};
static size_t index_of(int x,int y,int z,int f) {
    return ((size_t)f*WORLD_HEIGHT*2+y)*(CHUNK_SIZE*2)*(CHUNK_SIZE*2)+z*(CHUNK_SIZE*2)+x;
}
static void check_mesh(int cx,int cz) {
    RbxChunk c={.cx=cx,.cz=cz};rbx_terrain_chunk(cx,cz,c.blocks);rbx_chunk_mesh(&c);
    unsigned char *cover=calloc((size_t)BASE_CELLS*8*6,1);CHECK(cover);
    for(int i=0;i<c.count;i++) {
        const RbxQuad *q=&c.quads[i];int f=q->face,a=f<2 ? 1 : f<4 ? 2 : 0,ua=f<4 ? 0 : 2,va=f<2 ? 2 : 1;
        CHECK(q->u>0 && q->v>0 && q->block>0 && q->block<BLOCK_COUNT);
        for(int v=0;v<q->v;v++)for(int u=0;u<q->u;u++) {
            int p[3]={q->x,q->y,q->z};p[a]-=normals[f][a]>0;p[ua]+=u;p[va]+=v;
            CHECK(p[0]>=0 && p[0]<32 && p[1]>=0 && p[1]<WORLD_HEIGHT*2 && p[2]>=0 && p[2]<32);
            size_t at=index_of(p[0],p[1],p[2],f);CHECK(cover[at]==0);cover[at]=q->block;
        }
    }
    int exposed=0;
    for(int y=0;y<WORLD_HEIGHT*2;y++)for(int z=0;z<32;z++)for(int x=0;x<32;x++) {
        int wx=cx*32+x,wz=cz*32+z,b=rbx_world_cell(wx,y,wz);
        for(int f=0;f<6;f++) {
            int material=0;
            if(b && rbx_face_exposed(b,rbx_world_cell(wx+normals[f][0],y+normals[f][1],wz+normals[f][2]))) {
                material=b;exposed++;
                if(b==BLOCK_GRASS && f==0 && (y+1)%2)material=BLOCK_DIRT;
            }
            CHECK(cover[index_of(x,y,z,f)]==material);
        }
    }
    CHECK(c.count*4<exposed);free(cover);free(c.quads);
}
static void test_mesh(void) {
    rbx_world_build(RBX_WORLD_SEED);
    CHECK(rbx_world_set(17,25,16,BLOCK_AIR));
    int original=rbx_terrain_block(15,rbx_terrain_height(15,0),0);CHECK(original>0);
    CHECK(rbx_world_set(31,rbx_terrain_height(15,0)*2+1,0,BLOCK_AIR));
    /* Full water beside a partial water/stone/air parent must not emit internal water faces. */
    for(int z=0;z<2;z++)for(int y=80;y<82;y++)for(int x=30;x<32;x++)CHECK(rbx_world_set(x,y,z,BLOCK_WATER));
    CHECK(rbx_world_set(32,80,0,BLOCK_WATER));CHECK(rbx_world_set(32,81,0,BLOCK_STONE));
    /* Grow and rehash both edit indices, including negative boundaries and tall construction. */
    for(int i=0;i<340;i++) {
        int x=i%20-10,y=40+i/200,z=(i/20)%10-5;
        CHECK(rbx_world_set(x*2+(i&1),y*2,z*2,(i%3)+BLOCK_GRASS));
    }
    check_mesh(0,0);check_mesh(-1,0);check_mesh(1,0);check_mesh(0,-1);check_mesh(-1,-1);
    puts("PASS exact greedy/partial face coverage: no hidden or duplicate faces, water contacts, chunk edges and interior grass cuts");
}
int main(void) {dt=1.0/60;test_chunks();test_streaming();test_mesh();return 0;}
