/* Только для отдельных единиц компиляции 3D-рендера. */
#ifndef RBX_RENDER_INTERNAL_H
#define RBX_RENDER_INTERNAL_H
#include "rbx_internal.h"

typedef struct { float x, y, z, u, v; } RbxVertex;
typedef struct {
    uint32_t palette[16]; /* ARGB, преобразуется в цвет грани один раз */
    unsigned char mip[341]; /* 16x16 + 8x8 + 4x4 + 2x2 + 1x1 */
} RbxMaterial;

const RbxMaterial *rbx_material(int block, int face);
void rbx3d_polygon(const RbxVertex *vertices, int n, float nx, float ny, float nz,
                   uint32_t color, const RbxMaterial *material);
#endif
