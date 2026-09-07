#ifndef RBX_RENDER_INTERNAL_H
#define RBX_RENDER_INTERNAL_H
#include "rbx_internal.h"
enum { TEXTURE_SIZE=32,PALETTE_SIZE=64,MIP_TEXELS=1365,FOG_LEVELS=64,LIGHT_LEVELS=32 };
/* Gentle face orientation, independent of occlusion. No cast/contact shadows. */
static inline float rbx_face_shade(float nx,float ny,float nz) {return .82f+.14f*ny+.04f*nx+.03f*nz;}
#define RBX_DARK_FLOOR .045f

typedef struct { float x,y,z,u,v; } RbxVertex;
typedef struct {
    uint32_t palette[PALETTE_SIZE];
    unsigned char mip[MIP_TEXELS];
    int colors;
    Image image;
    uint32_t shades[LIGHT_LEVELS][FOG_LEVELS*PALETTE_SIZE],fog_color;
    uint32_t ready;
} RbxMaterial;
RbxMaterial *rbx_material(int block,int face);
const uint32_t *rbx_material_shades(RbxMaterial *material,int level,uint32_t fog);
void rbx3d_polygon(const RbxVertex *vertices,int n,float nx,float ny,float nz,uint32_t color,RbxMaterial *material,const unsigned char *light);
/* Camera-space overlay: stable projection/lighting, no world fog, own depth. */
void rbx3d_viewmodel(int enabled);
int rbx3d_project(float x,float y,float z,float *sx,float *sy);
void rbx3d_depth_clear(float x0,float y0,float x1,float y1);
#endif
