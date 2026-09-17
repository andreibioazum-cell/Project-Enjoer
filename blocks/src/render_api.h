/* The drawing API the game calls. Every function is forwarded to the active
 * backend by src/render.c; the implementations live in the software rasterizer
 * (src/geometrium/geometrium_render.c, src/graphics/) and in src/vk/.
 *
 * The 2D half of this API (rect/roundrect/circle/ring/line/image_draw/
 * text_scaled/text_width and the gfx_* frame calls) is declared in engine.h. */
#ifndef ENJOER_RENDER_API_H
#define ENJOER_RENDER_API_H

#include "engine.h"
#include "geometrium/geometrium_material.h"

/* Frame lifecycle and 2D drawing are in engine.h: gfx_init, gfx_begin_frame,
 * gfx_end_frame, gfx_cancel_frame, gfx_shutdown, gfx_error_screen, rect,
 * roundrect, circle, ring, line, image_draw, text_scaled, text_width. */

int geometrium3d_begin(Buffer *buffer,int scale,float cx,float cy,float cz,float yaw,float pitch,float fov_deg);
void geometrium3d_sky(uint32_t top,uint32_t bottom);
void geometrium3d_fog(float start,float end);
void geometrium3d_surface(int sx,int sy,int sz,int u,int v,int face,int block,const unsigned char light[4]);
/* Shared helper (geometrium_shapes.c): one half-cell quad becomes a polygon
 * through the active backend. Both backends use it for the CPU submission path
 * (the Vulkan backend normally draws whole chunks instead). */
void geometrium3d_quad_surface(int sx,int sy,int sz,int u,int v,int face,int block,const unsigned char light[4]);
/* Geometry of one quad in world coordinates, plus the light of each corner in
 * the quad's own winding order (geometrium_shapes.c). */
void geometrium_quad_build(int sx,int sy,int sz,int u,int v,int face,const unsigned char light[4],
                           GeometriumVertex out[4],unsigned char out_light[4],float normal[3]);
void geometrium_quad_vertices(int sx,int sy,int sz,int u,int v,int face,
                              GeometriumVertex out[4],float normal[3]);
void geometrium3d_segment(float x,float y,float z,float x2,float y2,float z2,uint32_t color);
int geometrium3d_visible(float x,float y,float z,float hx,float hy,float hz);
int geometrium3d_face_visible(int face,float plane);
int geometrium3d_project(float x,float y,float z,float *sx,float *sy);
void geometrium3d_depth_clear(float x0,float y0,float x1,float y1);
void geometrium3d_viewmodel(int enabled);
void geometrium3d_polygon(const GeometriumVertex *vertices,int n,float nx,float ny,float nz,
                          uint32_t color,GeometriumMaterial *material,const unsigned char light[4]);
void geometrium3d_end(void);

#endif
