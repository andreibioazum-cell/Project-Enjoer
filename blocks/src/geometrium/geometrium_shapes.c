/* World-aligned voxel surfaces; no legacy scene objects. */
#include "geometrium_render_internal.h"

static const unsigned char corners[6][4][3] = {
    {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},
    {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},
    {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},
    {{1,0,0},{0,0,0},{0,1,0},{1,1,0}},
    {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},
    {{0,0,0},{0,0,1},{0,1,1},{0,1,0}}
};
static const int normals[6][3] = {{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{1,0,0},{-1,0,0}};

/* Geometry of one half-cell quad, shared by the per-quad submission path of
 * both backends and by the Vulkan chunk uploader. */
void geometrium_quad_build(int sx,int sy,int sz,int u,int v,int face,
                           const unsigned char light[4],
                           GeometriumVertex out[4],unsigned char out_light[4],float normal[3]) {
    int base[3]={sx,sy,sz},ua=face<4 ? 0 : 2,va=face<2 ? 2 : 1;
    float phase_u=((base[ua]%2+2)%2)*.5f,phase_v=((base[va]%2+2)%2)*.5f;
    for (int i=0;i<4;i++) {
        float p[3]={sx*.5f,sy*.5f,sz*.5f};
        float du=corners[face][i][ua]*u*.5f,dv=corners[face][i][va]*v*.5f;
        p[ua]+=du;p[va]+=dv;
        /* World-aligned repeating UV: a half face uses a quarter of a PNG. */
        out[i]=(GeometriumVertex){p[0],p[1],p[2],phase_u+du,face<2 ? phase_v+dv : -phase_v-dv};
    }
    if (normal) { normal[0]=normals[face][0]; normal[1]=normals[face][1]; normal[2]=normals[face][2]; }
    if (out_light) for (int i=0;i<4;i++)
        out_light[i]=light ? light[corners[face][i][ua]+2*corners[face][i][va]] : 255;
}
void geometrium_quad_vertices(int sx,int sy,int sz,int u,int v,int face,
                              GeometriumVertex out[4],float normal[3]) {
    geometrium_quad_build(sx,sy,sz,u,v,face,NULL,out,NULL,normal);
}
void geometrium3d_quad_surface(int sx,int sy,int sz,int u,int v,int face,int block,const unsigned char light[4]) {
    if (face<0 || face>=6 || block<=BLOCK_AIR || block>=BLOCK_COUNT || u<=0 || v<=0) return;
    GeometriumVertex w[4];unsigned char vertex_light[4];float normal[3];
    geometrium_quad_build(sx,sy,sz,u,v,face,light,w,vertex_light,normal);
    geometrium3d_polygon(w,4,normal[0],normal[1],normal[2],0,geometrium_material(block,face),vertex_light);
}
