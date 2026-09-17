/* Material data shared by both render backends. The software rasterizer bakes
 * palettes and light palettes from these images; the Vulkan backend uploads the
 * very same PNGs as a texture array and indexes them by `layer`. */
#ifndef GEOMETRIUM_MATERIAL_H
#define GEOMETRIUM_MATERIAL_H

#include "engine.h"

enum { TEXTURE_SIZE=32,PALETTE_SIZE=64,MIP_TEXELS=1365,FOG_LEVELS=64,LIGHT_LEVELS=32 };
#define GEOMETRIUM_DARK_FLOOR .045f

typedef struct { float x,y,z,u,v; } GeometriumVertex;
typedef struct {
    uint32_t palette[PALETTE_SIZE];
    unsigned char mip[MIP_TEXELS];
    int colors;
    unsigned char alpha; /* 0 = opaque; water blends over the framebuffer */
    int layer;           /* index inside the Vulkan texture array */
    Image image;
    uint32_t shades[LIGHT_LEVELS][FOG_LEVELS*PALETTE_SIZE],fog_color;
    uint32_t ready;
} GeometriumMaterial;

/* Gentle face orientation, independent of occlusion. No cast/contact shadows. */
static inline float geometrium_face_shade(float nx,float ny,float nz) {return .82f+.14f*ny+.04f*nx+.03f*nz;}
/* One vertex of a voxel quad: face orientation times the baked skylight. Both
 * renderers use this exact expression, so software and Vulkan frames agree. */
static inline float geometrium_vertex_shade(float nx,float ny,float nz,unsigned int light) {
    float ambient=(float)light/255.0f;
    return geometrium_face_shade(nx,ny,nz)*(GEOMETRIUM_DARK_FLOOR+(1.0f-GEOMETRIUM_DARK_FLOOR)*ambient);
}

int geometrium_materials_load(AAssetManager *assets);
GeometriumMaterial *geometrium_material(int block,int face);
const uint32_t *geometrium_material_shades(GeometriumMaterial *material,int level,uint32_t fog);
const Image *geometrium_material_icon(int block);
/* Texture array layer of the icon returned by geometrium_material_icon(). */
int geometrium_material_layer(int block, int face);
int geometrium_material_layer_of(const Image *image);
/* Texture array contents for the Vulkan backend: layer count, the RGBA image of
 * one layer and the blend alpha attached to it (water is translucent). */
int geometrium_material_count(void);
const Image *geometrium_material_image(int layer);
int geometrium_material_alpha(int layer);
int geometrium_material_water(int layer);

#endif
