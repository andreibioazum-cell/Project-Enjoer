/* Кубы и отдельные грани вокселей. Геометрия и UV не зависят от камеры. */
#include "rbx_render_internal.h"
#include <math.h>

static const unsigned char corners[6][4][3] = {
    {{0,1,0},{0,1,1},{1,1,1},{1,1,0}},
    {{0,0,0},{1,0,0},{1,0,1},{0,0,1}},
    {{0,0,1},{1,0,1},{1,1,1},{0,1,1}},
    {{1,0,0},{0,0,0},{0,1,0},{1,1,0}},
    {{1,0,0},{1,1,0},{1,1,1},{1,0,1}},
    {{0,0,0},{0,0,1},{0,1,1},{0,1,0}}
};
static const int normals[6][3] = {{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{1,0,0},{-1,0,0}};

void rbx3d_block_face(float x, float y, float z, int face, int block) {
    if (face < 0 || face >= 6 || block <= BLOCK_AIR || block >= BLOCK_COUNT) return;
    RbxVertex w[4];
    for (int i = 0; i < 4; i++) {
        float lx = corners[face][i][0], ly = corners[face][i][1], lz = corners[face][i][2];
        w[i] = (RbxVertex){x + lx, y + ly, z + lz, face < 4 ? lx : lz, face < 2 ? lz : 1 - ly};
    }
    rbx3d_polygon(w, 4, normals[face][0], normals[face][1], normals[face][2], 0, rbx_material(block, face));
}
void rbx3d_box(float x, float y, float z, float hx, float hy, float hz, float yaw, uint32_t color) {
    if (hx <= 0 || hy <= 0 || hz <= 0 || !isfinite(x + y + z + hx + hy + hz + yaw) ||
        !rbx3d_visible(x, y, z, hx, hy, hz)) return;
    float c = cosf(yaw), s = sinf(yaw);
    for (int face = 0; face < 6; face++) {
        RbxVertex w[4];
        for (int i = 0; i < 4; i++) {
            float lx = (corners[face][i][0] * 2 - 1) * hx;
            float ly = (corners[face][i][1] * 2 - 1) * hy;
            float lz = (corners[face][i][2] * 2 - 1) * hz;
            w[i] = (RbxVertex){x + lx * c + lz * s, y + ly, z - lx * s + lz * c, 0, 0};
        }
        float nx = normals[face][0] * c + normals[face][2] * s;
        float nz = -normals[face][0] * s + normals[face][2] * c;
        rbx3d_polygon(w, 4, nx, normals[face][1], nz, color, NULL);
    }
}
