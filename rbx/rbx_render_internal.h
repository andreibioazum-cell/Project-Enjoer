#ifndef RBX_RENDER_INTERNAL_H
#define RBX_RENDER_INTERNAL_H
#include "rbx_internal.h"
enum { TEXTURE_SIZE=32,PALETTE_SIZE=64,MIP_TEXELS=1365,FOG_LEVELS=64,SHADE_FACES=12 };
/* Множитель яркости граней в тени: резкая граница, как направленный свет в three.js. */
#define RBX_SUN_SHADOW .62f
typedef struct { float x,y,z,u,v; } RbxVertex;
typedef struct {
    uint32_t palette[PALETTE_SIZE]; /* ARGB source colors */
    unsigned char mip[MIP_TEXELS]; /* 32 + 16 + 8 + 4 + 2 + 1 squared */
    int colors;
    Image image;
    /* Индексы 0..5 — освещённые грани, 6..11 — те же грани в тени. */
    uint32_t shades[SHADE_FACES][FOG_LEVELS*PALETTE_SIZE],fog_color;
    unsigned ready;
} RbxMaterial;
RbxMaterial *rbx_material(int block,int face);
const uint32_t *rbx_material_shades(RbxMaterial *material,int face,uint32_t fog);
void rbx3d_polygon(const RbxVertex *vertices,int n,float nx,float ny,float nz,uint32_t color,RbxMaterial *material,int shadow);
int rbx3d_project(float x,float y,float z,float *sx,float *sy);
void rbx3d_depth_clear(float x0,float y0,float x1,float y1);
#endif
