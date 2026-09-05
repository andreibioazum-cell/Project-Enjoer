/* Генерация, стыки чанков, ограниченная память и скрытые грани. */
#include "rbx/rbx_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"%s:%d: %s\n",__func__,__LINE__,#x); exit(1); } } while(0)
void ds_runtime_error(const char *s,...) { fprintf(stderr,"%s\n",s); abort(); }
int rbx3d_visible(float x,float y,float z,float hx,float hy,float hz) { (void)x;(void)y;(void)z;(void)hx;(void)hy;(void)hz; return 0; }
void rbx3d_block_face(float x,float y,float z,int face,int block) { (void)x;(void)y;(void)z;(void)face;(void)block; }

static void test_chunks(void) {
    unsigned char a[CHUNK_SIZE*CHUNK_SIZE*WORLD_HEIGHT],b[sizeof(a)];
    int types[BLOCK_COUNT]={0},low=100,high=0;
    const int coords[][2]={{0,0},{1,-1},{-1,1},{-3,-2},{2,3},{3,-4}};
    rbx_terrain_seed(RBX_WORLD_SEED);
    for(unsigned i=0;i<sizeof(coords)/sizeof(*coords);i++) {
        int cx=coords[i][0],cz=coords[i][1];
        rbx_terrain_chunk(cx,cz,a); rbx_terrain_chunk(cx,cz,b); CHECK(memcmp(a,b,sizeof(a))==0);
        for(int z=0;z<CHUNK_SIZE;z++) for(int x=0;x<CHUNK_SIZE;x++) {
            int wx=cx*CHUNK_SIZE+x,wz=cz*CHUNK_SIZE+z;
            int h=rbx_terrain_height(wx,wz); if(h<low)low=h; if(h>high)high=h;
            for(int y=0;y<WORLD_HEIGHT;y++) {
                int block=a[(y*CHUNK_SIZE+z)*CHUNK_SIZE+x];
                CHECK(block>=0&&block<BLOCK_COUNT); types[block]++;
                /* Полные граничные колонки и выборка внутри, включая кроны. */
                if(x==0||x==15||z==0||z==15||(x%5==0&&z%5==0)) CHECK(block==rbx_terrain_block(wx,y,wz));
            }
        }
    }
    CHECK(high-low>=5);
    for(int i=0;i<BLOCK_COUNT;i++) CHECK(types[i]>0);
    rbx_terrain_chunk(3,-4,a); rbx_terrain_seed(RBX_WORLD_SEED+1); rbx_terrain_chunk(3,-4,b);
    CHECK(memcmp(a,b,sizeof(a))!=0);
    puts("PASS deterministic seeds, negative chunk coordinates, tree seams and all terrain materials");
}
static uint32_t fingerprint(void) {
    uint32_t hash=2166136261u;
    for(int z=-45;z<60;z+=3) for(int x=-45;x<60;x+=3) for(int y=4;y<30;y+=3)
        hash=(hash^(uint32_t)rbx_world_block(x,y,z))*16777619u;
    return hash;
}
static void test_streaming(void) {
    rbx_world_build(RBX_WORLD_SEED);
    uint32_t original=fingerprint();
    int chunks,faces; rbx_world_stats(&chunks,&faces);
    int expected=(WORLD_RADIUS*2+1)*(WORLD_RADIUS*2+1);
    CHECK(chunks==expected && faces>expected*256 && faces<expected*3000);
    CHECK(rbx_world_solid(8,12,8)); CHECK(!rbx_world_solid(8,13,8));
    CHECK(rbx_world_block(1,-1,1)==BLOCK_STONE && rbx_world_block(1,WORLD_HEIGHT,1)==BLOCK_AIR);
    const int positions[][2]={{80,0},{-80,-96},{1024,-1024},{-10000,20000},{8,8}};
    for(unsigned i=0;i<sizeof(positions)/sizeof(*positions);i++) {
        int x=positions[i][0],z=positions[i][1];
        rbx_world_update(x+.5f,z+.5f); rbx_world_stats(&chunks,&faces);
        CHECK(chunks==expected && faces>0 && faces<expected*3000);
        for(int dx=-17;dx<=17;dx+=3) for(int dz=-17;dz<=17;dz+=3) for(int y=5;y<30;y+=4)
            CHECK(rbx_world_block(x+dx,y,z+dz)==rbx_terrain_block(x+dx,y,z+dz));
    }
    CHECK(original==fingerprint());
    rbx_world_build(RBX_WORLD_SEED); CHECK(original==fingerprint());
    puts("PASS bounded chunk cache, exposed-face meshes, long-distance streaming and return-trip determinism");
}
int main(void) { test_chunks(); test_streaming(); return 0; }
