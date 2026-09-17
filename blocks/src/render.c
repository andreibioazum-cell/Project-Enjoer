/* src/render.c — backend selection and the drawing entry points every other
 * module calls. Nothing here knows how a triangle is filled. */
#include "render.h"
#include "geometrium/geometrium_internal.h"

const RenderBackend *render_select(AAssetManager *assets,int width,int height,const char *preference) {
    (void)width; (void)height;
    int strict = preference && !strcmp(preference,"vulkan!");
    int vulkan_requested = strict || (preference && (!strcmp(preference,"vulkan") || !strcmp(preference,"vk")));
#if defined(ENJOER_VULKAN)
    int software_requested = preference && (!strcmp(preference,"software") || !strcmp(preference,"sw"));
    if (!software_requested) {
        render_backend = &vk_render_backend;
        if (render_backend->attach_window) render_backend->attach_window(render_platform_window());
        if (render_backend->init(assets)) {
            app_log("renderer: Vulkan (%s)", render_backend->name);
            return render_backend;
        }
        /* "vulkan!" is the strict form: no silent downgrade, the caller gets
         * NULL and can show the driver problem instead of a slow frame rate. */
        if (strict) {
            app_log_error("Vulkan renderer unavailable: %s", vk_last_error());
            return NULL;
        }
        app_log_error("Vulkan renderer unavailable (%s); using the software renderer", vk_last_error());
        if (render_backend->cancel_frame) render_backend->cancel_frame();
        render_backend = &sw_render_backend;
        render_set_window(NULL);
    } else if (vulkan_requested) {
        app_log_error("this build has no Vulkan backend compiled in");
    }
#else
    if (vulkan_requested) app_log_error("this build has no Vulkan backend compiled in");
#endif
    render_backend = &sw_render_backend;
    return render_backend->init(assets) ? render_backend : NULL;
}

/* ── frame lifecycle ─────────────────────────────────────────────────── */
int gfx_init(AAssetManager *assets) { return render_backend->init(assets); }
int gfx_begin_frame(Buffer *buffer) { return render_backend->begin_frame(buffer); }
void gfx_end_frame(void) { render_backend->end_frame(); }
void gfx_cancel_frame(void) { render_backend->cancel_frame(); }
void gfx_shutdown(void) { render_backend->shutdown(); }
void gfx_error_screen(const char *message) { render_backend->error_screen(message); }

/* ── 2D HUD ──────────────────────────────────────────────────────────── */
void rect(float x,float y,float w,float h,uint32_t color) { render_backend->rect(x,y,w,h,color); }
void roundrect(float x,float y,float w,float h,float radius,uint32_t color) { render_backend->roundrect(x,y,w,h,radius,color); }
void circle(float x,float y,float radius,uint32_t color) { render_backend->circle(x,y,radius,color); }
void ring(float x,float y,float radius,float thickness,uint32_t color) { render_backend->ring(x,y,radius,thickness,color); }
void line(float x1,float y1,float x2,float y2,float thickness,uint32_t color) { render_backend->line(x1,y1,x2,y2,thickness,color); }
void image_draw(const Image *image,float x,float y,float w,float h) { render_backend->image_draw(image,x,y,w,h); }
void text_scaled(const char *string,float x,float y,uint32_t color,float scale) { render_backend->text_scaled(string,x,y,color,scale); }
int text_width(const char *string) { return render_backend->text_width(string); }

/* ── 3D ──────────────────────────────────────────────────────────────── */
int geometrium3d_begin(Buffer *buffer,int scale,float cx,float cy,float cz,float yaw,float pitch,float fov_deg) {
    return render_backend->begin3d(buffer,scale,cx,cy,cz,yaw,pitch,fov_deg);
}
void geometrium3d_sky(uint32_t top,uint32_t bottom) { render_backend->sky(top,bottom); }
void geometrium3d_fog(float start,float end) { render_backend->fog(start,end); }
void geometrium3d_surface(int sx,int sy,int sz,int u,int v,int face,int block,const unsigned char light[4]) {
    render_backend->surface(sx,sy,sz,u,v,face,block,light);
}
void geometrium3d_segment(float x,float y,float z,float x2,float y2,float z2,uint32_t color) {
    render_backend->segment(x,y,z,x2,y2,z2,color);
}
int geometrium3d_visible(float x,float y,float z,float hx,float hy,float hz) {
    return render_backend->visible(x,y,z,hx,hy,hz);
}
int geometrium3d_face_visible(int face,float plane) { return render_backend->face_visible(face,plane); }
int geometrium3d_project(float x,float y,float z,float *sx,float *sy) { return render_backend->project(x,y,z,sx,sy); }
void geometrium3d_depth_clear(float x0,float y0,float x1,float y1) { render_backend->depth_clear(x0,y0,x1,y1); }
void geometrium3d_viewmodel(int enabled) { render_backend->viewmodel(enabled); }
void geometrium3d_polygon(const GeometriumVertex *vertices,int n,float nx,float ny,float nz,
                          uint32_t color,GeometriumMaterial *material,const unsigned char light[4]) {
    render_backend->polygon(vertices,n,nx,ny,nz,color,material,light);
}
void geometrium3d_end(void) { render_backend->end3d(); }
