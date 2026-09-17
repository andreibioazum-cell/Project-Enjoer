/* Render backends.
 *
 * The game code never talks to a rasterizer directly: every drawing entry point
 * (the HUD primitives from engine.h, the frame lifecycle and the 3D surface API
 * from geometrium_internal.h) is forwarded through the table below. Two
 * implementations exist:
 *
 *   render_sw.c  software rasterizer — plain C, always available, no driver;
 *   render_vk.c  Vulkan backend      — src/vk/, the default whenever the
 *                                      platform exposes a usable device.
 *
 * src/render.c picks one at startup and can fall back to the software backend
 * if Vulkan cannot be initialised, so a missing driver degrades quality instead
 * of breaking the app. */
#ifndef ENJOER_RENDER_H
#define ENJOER_RENDER_H

#include "engine.h"
#include "render_api.h"
#include "geometrium/geometrium_material.h"

typedef struct RenderBackend {
    const char *name;
    int  windowed;                                  /* presents through the platform, not a Buffer */
    /* lifecycle */
    int  (*init)(AAssetManager *assets);
    void (*shutdown)(void);
    int  (*attach_window)(void *native_window);      /* Vulkan on Android; optional */
    /* frame */
    int  (*begin_frame)(Buffer *buffer);
    void (*end_frame)(void);
    void (*cancel_frame)(void);
    void (*error_screen)(const char *message);
    /* 2D */
    void (*rect)(float x,float y,float w,float h,uint32_t color);
    void (*roundrect)(float x,float y,float w,float h,float radius,uint32_t color);
    void (*circle)(float x,float y,float radius,uint32_t color);
    void (*ring)(float x,float y,float radius,float thickness,uint32_t color);
    void (*line)(float x1,float y1,float x2,float y2,float thickness,uint32_t color);
    void (*image_draw)(const Image *image,float x,float y,float w,float h);
    void (*text_scaled)(const char *string,float x,float y,uint32_t color,float scale);
    int  (*text_width)(const char *string);
    /* 3D */
    int  (*begin3d)(Buffer *buffer,int scale,float cx,float cy,float cz,float yaw,float pitch,float fov_deg);
    void (*sky)(uint32_t top,uint32_t bottom);
    void (*fog)(float start,float end);
    void (*surface)(int sx,int sy,int sz,int u,int v,int face,int block,const unsigned char *light);
    void (*segment)(float x,float y,float z,float x2,float y2,float z2,uint32_t color);
    int  (*visible)(float x,float y,float z,float hx,float hy,float hz);
    int  (*face_visible)(int face,float plane);
    int  (*project)(float x,float y,float z,float *sx,float *sy);
    void (*depth_clear)(float x0,float y0,float x1,float y1);
    void (*viewmodel)(int enabled);
    void (*polygon)(const GeometriumVertex *vertices,int n,float nx,float ny,float nz,
                    uint32_t color,GeometriumMaterial *material,const unsigned char light[4]);
    /* Whole-chunk submission (hardware backends only; NULL = per-quad path).
     * The backend iterates the same chunk rings, keeps GPU buffers per chunk
     * and records the opaque and water passes itself. */
    void (*chunks)(int center_x,int center_z,int radius);
    void (*end3d)(void);
} RenderBackend;

extern const RenderBackend *render_backend;
extern const RenderBackend sw_render_backend;
/* src/render_state.c */
void render_set_window(void *native_window);
void *render_platform_window(void);
int render_windowed(void);
const char *render_backend_name(void);
#if defined(ENJOER_VULKAN)
extern const RenderBackend vk_render_backend;
/* Why the last vk_render_backend.init() failed (src/vk/vk_backend.c). */
const char *vk_last_error(void);
#endif

/* Called once by the platform layer before the first frame.
 * preference: NULL/"auto", "vulkan", "vulkan!" (required) or "software". */
const RenderBackend *render_select(AAssetManager *assets,int width,int height,const char *preference);

#endif
