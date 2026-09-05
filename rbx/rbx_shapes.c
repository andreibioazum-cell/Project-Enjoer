/* World-aligned voxel surfaces; no legacy scene objects. */
#include "rbx_render_internal.h"

static const unsigned char corners[6][4][3] = {
    {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},
    {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},
    {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},
    {{1,0,0},{0,0,0},{0,1,0},{1,1,0}},
    {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},
    {{0,0,0},{0,0,1},{0,1,1},{0,1,0}}
};
static const int normals[6][3] = {{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{1,0,0},{-1,0,0}};

void rbx3d_surface(int sx,int sy,int sz,int u,int v,int face,int block) {
    if (face<0 || face>=6 || block<=BLOCK_AIR || block>=BLOCK_COUNT || u<=0 || v<=0) return;
    int base[3]={sx,sy,sz},ua=face<4 ? 0 : 2,va=face<2 ? 2 : 1;
    float phase_u=((base[ua]%2+2)%2)*.5f,phase_v=((base[va]%2+2)%2)*.5f;
    RbxVertex w[4];
    for (int i=0;i<4;i++) {
        float p[3]={sx*.5f,sy*.5f,sz*.5f};
        float du=corners[face][i][ua]*u*.5f,dv=corners[face][i][va]*v*.5f;
        p[ua]+=du;p[va]+=dv;
        /* World-aligned repeating UV: a half face uses a quarter of a PNG. */
        w[i]=(RbxVertex){p[0],p[1],p[2],phase_u+du,face<2 ? phase_v+dv : -phase_v-dv};
    }
    rbx3d_polygon(w,4,normals[face][0],normals[face][1],normals[face][2],0,rbx_material(block,face));
}
