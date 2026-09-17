/* Software 3D backend entry points (src/geometrium/geometrium_render.c).
 * src/render.c selects between these and the Vulkan ones at runtime. */
#ifndef GEOMETRIUM_SW_H
#define GEOMETRIUM_SW_H

#include "geometrium_material.h"

int sw_geometrium3d_begin(Buffer *buffer,int scale,float cx,float cy,float cz,float yaw,float pitch,float fov_deg);
void sw_geometrium3d_sky(uint32_t top,uint32_t bottom);
void sw_geometrium3d_fog(float start,float end);
void sw_geometrium3d_polygon(const GeometriumVertex *vertices,int n,float nx,float ny,float nz,
                             uint32_t color,GeometriumMaterial *material,const unsigned char *light);
void sw_geometrium3d_viewmodel(int enabled);
int sw_geometrium3d_visible(float x,float y,float z,float hx,float hy,float hz);
int sw_geometrium3d_face_visible(int face,float plane);
int sw_geometrium3d_project(float x,float y,float z,float *sx,float *sy);
void sw_geometrium3d_depth_clear(float x0,float y0,float x1,float y1);
void sw_geometrium3d_segment(float x,float y,float z,float x2,float y2,float z2,uint32_t color);
void sw_geometrium3d_end(void);

#endif
