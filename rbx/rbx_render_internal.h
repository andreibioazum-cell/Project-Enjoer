#ifndef RBX_RENDER_INTERNAL_H
#define RBX_RENDER_INTERNAL_H
#include "rbx_internal.h"
enum { TEXTURE_SIZE=32,PALETTE_SIZE=64,MIP_TEXELS=1365,FOG_LEVELS=64 };
typedef struct { float x,y,z,u,v; } RbxVertex;
typedef struct {
    uint32_t palette[PALETTE_SIZE]; /* ARGB source colors */
    unsigned char mip[MIP_TEXELS]; /* 32 + 16 + 8 + 4 + 2 + 1 squared */
    int colors;
    Image image;
    uint32_t shades[6][FOG_LEVELS*PALETTE_SIZE],fog_color;
    unsigned ready;
} RbxMaterial;
RbxMaterial *rbx_material(int block,int face);
const uint32_t *rbx_material_shades(RbxMaterial *material,int face,uint32_t fog);
void rbx3d_polygon(const RbxVertex *vertices,int n,float nx,float ny,float nz,uint32_t color,RbxMaterial *material);
#endif
