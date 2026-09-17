/* graphics/gfx_frame.c — software frame lifecycle and the 2D HUD primitives.
 * The software backend draws straight into the Buffer it is given; the Vulkan
 * backend (src/vk/) implements the same entry points through a swapchain or an
 * offscreen image. src/render.c decides which one is active. */
#include "gfx_internal.h"
#include "gfx_sw.h"

static Buffer *current;

Buffer *sw_gfx_current_buffer(void) { return current; }

int sw_gfx_init(AAssetManager *a) {
    return gfx_font_load(a);
}
int sw_gfx_begin_frame(Buffer *b) {
    if (!b || !b->pixels || b->width<=0 || b->height<=0 || b->stride<b->width) return 0;
    current=b;
    /* The 3D pass fills the frame; no redundant full-screen clear here. */
    return 1;
}
void sw_gfx_end_frame(void) { current=NULL; }
void sw_gfx_cancel_frame(void) { current=NULL; }
void sw_gfx_shutdown(void) { current=NULL; gfx_font_unload(); }

void sw_rect(float x,float y,float w,float h,uint32_t c) { gfx_render_rect(current,x,y,w,h,gfx_pack(c)); }
void sw_roundrect(float x,float y,float w,float h,float r,uint32_t c) { gfx_render_roundrect(current,x,y,w,h,r,gfx_pack(c)); }
void sw_circle(float x,float y,float r,uint32_t c) { gfx_render_circle(current,x,y,r,gfx_pack(c)); }
void sw_ring(float x,float y,float r,float th,uint32_t c) { gfx_render_ring(current,x,y,r,th,gfx_pack(c)); }
void sw_line(float x,float y,float x2,float y2,float th,uint32_t c) { gfx_render_line(current,x,y,x2,y2,th,gfx_pack(c)); }
void sw_image_draw(const Image *im,float x,float y,float w,float h) {
    if (!current || !im || !im->pixels || w<=0 || h<=0) return;
    int x0=gfx_cl_floor(x,current->width),y0=gfx_cl_floor(y,current->height);
    int x1=gfx_cl_ceil(x+w,current->width),y1=gfx_cl_ceil(y+h,current->height);
    for (int py=y0;py<y1;py++) for (int px=x0;px<x1;px++) {
        int tx=(int)((px+.5f-x)*im->width/w),ty=(int)((py+.5f-y)*im->height/h);
        if (tx<0 || ty<0 || tx>=im->width || ty>=im->height) continue;
        uint32_t *p=&current->pixels[py*current->stride+px];
        *p=gfx_blend(*p,im->pixels[ty*im->width+tx]);
    }
}
