#ifndef GEOMETRIUM_RENDER_INTERNAL_H
#define GEOMETRIUM_RENDER_INTERNAL_H
#include "geometrium_internal.h"
enum { TEXTURE_SIZE=32,PALETTE_SIZE=64,MIP_TEXELS=1365,FOG_LEVELS=64,LIGHT_LEVELS=32 };
/* Gentle face orientation, independent of occlusion. No cast/contact shadows. */
static inline float geometrium_face_shade(float nx,float ny,float nz) {return .82f+.14f*ny+.04f*nx+.03f*nz;}
#define GEOMETRIUM_DARK_FLOOR .045f

typedef struct { float x,y,z,u,v; } GeometriumVertex;
typedef struct {
    uint32_t palette[PALETTE_SIZE];
    unsigned char mip[MIP_TEXELS];
    int colors;
    unsigned char alpha; /* 0 = opaque; water blends over the framebuffer */
    Image image;
    uint32_t shades[LIGHT_LEVELS][FOG_LEVELS*PALETTE_SIZE],fog_color;
    uint32_t ready;
} GeometriumMaterial;
GeometriumMaterial *geometrium_material(int block,int face);
const uint32_t *geometrium_material_shades(GeometriumMaterial *material,int level,uint32_t fog);
void geometrium3d_polygon(const GeometriumVertex *vertices,int n,float nx,float ny,float nz,uint32_t color,GeometriumMaterial *material,const unsigned char *light);
/* Camera-space overlay: stable projection/lighting, no world fog, own depth. */
void geometrium3d_viewmodel(int enabled);
int geometrium3d_project(float x,float y,float z,float *sx,float *sy);
void geometrium3d_depth_clear(float x0,float y0,float x1,float y1);
#endif
