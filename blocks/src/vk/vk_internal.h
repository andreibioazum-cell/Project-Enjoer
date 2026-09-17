/* Internal interface of the Vulkan render backend.
 *
 * The backend replaces the software rasterizer behind the RenderBackend table
 * (src/render.h) and is split into:
 *
 *   vk_device.c     instance, device, memory, uploads, targets, swapchain
 *   vk_pipelines.c  render passes, pipelines, descriptors, textures
 *   vk_world.c      per-chunk vertex/index buffers and the two world passes
 *   vk_hud.c        the 2D HUD batch (shapes, images) and the upscale pass
 *   vk_text.c       glyph quads from the shared font atlas
 *   vk_backend.c    frame orchestration, the camera/depth API and the table
 *
 * Every GPU resource is owned by the single `vk` context below; there is no
 * hidden global state anywhere else. */
#ifndef ENJOER_VK_INTERNAL_H
#define ENJOER_VK_INTERNAL_H

#include "../render.h"
#include "../graphics/gfx_font.h"
#include "shaders/spv/shaders_spv.h"

/* Vulkan types, the runtime-resolved entry points and the vk* aliases. */
#include "vk_entry.h"
#include "vk_entry_alias.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

enum {
    VK_FRAMES_IN_FLIGHT = 2,
    VK_CHUNK_SLOTS = 256,
    VK_SWAPCHAIN_IMAGES = 4,
    VK_LINE_CAPACITY = 4096,      /* immediate outline/polygon staging */
    VK_TRIANGLE_CAPACITY = 4096,
    VK_UI_CAPACITY = 20000,
    VK_GLYPH_QUAD_CAPACITY = 4096
};

/* ── vertex formats ──────────────────────────────────────────────────── */
/* 28 bytes: position, block-unit UV, a colour (white for textured quads — the
 * hand's flat arm uses it), the baked light and the texture array layer. */
typedef struct {
    float x, y, z;
    float u, v;
    unsigned char r, g, b, a;
    unsigned char shade, layer, pad0, pad1;
} VkWorldVertex;
/* 36 bytes: pixel position, straight-alpha colour, atlas/image UV, layer. */
typedef struct { float x,y; float r,g,b,a; float u,v; float layer; } VkUiVertex;
/* 28 bytes: world or camera space segment with a colour. */
typedef struct { float x,y,z; float r,g,b,a; } VkLineVertex;

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    void *mapped;      /* non-NULL for host-visible buffers */
} VkGpuBuffer;

/* Immediate geometry ring: one host-visible buffer per frame in flight. */
typedef struct {
    VkGpuBuffer gpu;
    VkDeviceSize capacity;   /* bytes */
    size_t count;            /* vertices written this frame */
    size_t recorded;         /* vertices already turned into a draw call */
} VkImmediate;

typedef struct {
    int cx, cz, used;
    unsigned serial;
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size, vertex_offset, index_offset;
    uint32_t index_count, opaque_index_count;
} VkChunk;

typedef struct {
    int width, height, layers, mips;
    VkFormat format;
    VkImage image;
    VkImageView view;
    VkDeviceMemory memory;
} VkTexture;

typedef struct {
    int width, height;
    VkFramebuffer framebuffer;
    VkImage color;
    VkImageView color_view;
    VkDeviceMemory color_memory;
    VkImage depth;
    VkImageView depth_view;
    VkDeviceMemory depth_memory;
} VkSceneTarget;

/* Push constant blocks (128 bytes is the guaranteed minimum). */
typedef struct {
    float viewProj[16];   /* 64 */
    float camPosAlpha[4]; /* 16: eye xyz + pass alpha */
    float fog[4];         /* 16: start, 1/(end-start), enabled, 0 */
    float fogColor[4];    /* 16 */
    float flags[4];       /* 16: viewmodel, depth bias, 0, 0 */
} VkPush3D;

typedef struct {
    float screen[4];      /* frame w,h + atlas w,h */
    float tint[4];
    float mode[4];        /* 0 solid, 1 glyphs, 2 image */
} VkPush2D;

typedef struct {
    float geometry[4];    /* focal length, internal height, sin(yaw), cos(yaw) */
    float top[4];
    float bottom[4];
} VkPushSky;

typedef struct {
    float screen[4];      /* frame w,h + scene w,h */
    float tint[4];
    float mode[4];
} VkPushPresent;

typedef struct {
    int ready;
    char error[256];

    /* surface */
    int windowed;             /* presents through the platform surface */
    int width, height;        /* frame size in pixels */
    int scene_width, scene_height; /* internal 3D resolution */
    int scale;
    void *native_window;

    /* device */
    VkInstance instance;
    VkPhysicalDevice physical;
    VkPhysicalDeviceProperties properties;
    VkDevice device;
    uint32_t queue_family;
    VkQueue queue;
    VkPhysicalDeviceMemoryProperties memory;
    VkCommandPool command_pool;
    VkCommandBuffer cmd[VK_FRAMES_IN_FLIGHT];
    VkFence fence[VK_FRAMES_IN_FLIGHT];
    VkSemaphore image_ready;      /* swapchain acquire */
    VkSemaphore render_done;      /* present wait */
    uint32_t frame_index;
    int frame_active;
    int pass3d_active;

    /* pipelines */
    VkRenderPass pass3d, pass2d;
    VkPipelineLayout layout3d, layout2d, layout_sky, layout_line, layout_present;
    VkPipeline pipe_world_opaque, pipe_world_water, pipe_sky, pipe_line;
    VkPipeline pipe_ui_solid, pipe_ui_glyph, pipe_ui_image, pipe_present;
    VkDescriptorSetLayout layout_tiles, layout_glyphs, layout_scene;
    VkDescriptorPool descriptor_pool;
    VkDescriptorSet set_tiles, set_glyphs, set_scene;
    VkSampler sampler_tiles, sampler_glyphs, sampler_scene;
    VkTexture tiles;   /* 32x32 RGBA block textures, one layer each */
    VkTexture glyphs;  /* baked font coverage, R8 */

    /* targets */
    VkSceneTarget scene;
    int target_ready;
    VkImage target_image;           /* offscreen colour target */
    VkImageView target_view;
    VkDeviceMemory target_memory;
    VkFormat target_format;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    void *readback_mapped;
    VkDeviceSize readback_size;
    VkSwapchainKHR swapchain;
    VkImage swapchain_images[VK_SWAPCHAIN_IMAGES];
    VkImageView swapchain_views[VK_SWAPCHAIN_IMAGES];
    uint32_t swapchain_count;
    uint32_t swapchain_index;
    int swapchain_dirty;
    VkFormat swapchain_format;
    VkExtent2D swapchain_extent;

    /* frame input/output */
    Buffer *frame;
    int frame_has_pixels;
    int ui_dirty;

    /* immediate geometry */
    VkImmediate lines[VK_FRAMES_IN_FLIGHT];
    VkImmediate triangles[VK_FRAMES_IN_FLIGHT];
    VkImmediate ui[VK_FRAMES_IN_FLIGHT];

    /* chunk meshes */
    VkChunk chunks[VK_CHUNK_SLOTS];
    int chunk_count;

    /* camera state (mirrors the software renderer's clipping planes) */
    float camx, camy, camz;
    float yaw_s, yaw_c, pitch_s, pitch_c;
    float foc, view_x, view_y, side_x, side_y;
    float view_matrix[16];
    float proj_matrix[16];
    float fog_start, fog_end;
    uint32_t fog_color;
    uint32_t sky_top, sky_bottom;
    int viewmodel;
    int scene_started;
} VkContext;

extern VkContext vk;

/* ── vk_device.c ─────────────────────────────────────────────────────── */
int  vk_device_init(AAssetManager *assets);
void vk_device_shutdown(void);
void vk_set_error(const char *format, ...);
int  vk_create_buffer(VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags properties,VkGpuBuffer *out);
void vk_free_buffer(VkGpuBuffer *buffer);
int  vk_upload(VkBuffer destination,VkDeviceSize offset,const void *data,VkDeviceSize size);
int  vk_create_color_image(int width,int height,VkFormat format,VkImageUsageFlags usage,
                           VkImage *image,VkImageView *view,VkDeviceMemory *memory);
void vk_destroy_image(VkImage *image,VkImageView *view,VkDeviceMemory *memory);
VkCommandBuffer vk_begin_once(void);
void vk_end_once(VkCommandBuffer cmd);
int  vk_frame_begin(Buffer *buffer);
void vk_frame_end(void);
void vk_frame_cancel(void);
int  vk_attach_window(void *native_window);
void vk_require_targets(int width,int height,int scale);
int  vk_draw_offscreen(void);

/* ── vk_pipelines.c ──────────────────────────────────────────────────── */
int  vk_pipelines_init(AAssetManager *assets);
void vk_pipelines_shutdown(void);
int  vk_scene_target_ensure(int width,int height);
/* Records the 3D render pass (clear + sky-ready) for the given camera. */
void vk_begin_scene_pass(void);
void vk_end_scene_pass(void);
void vk_bind_world(int water);
void vk_push3d(float alpha);
void vk_push_line(void);
void vk_push_sky(void);
void vk_push2d(int mode,float alpha);
void vk_push_present(void);

/* ── vk_world.c ──────────────────────────────────────────────────────── */
void vk_world_draw(int center_x,int center_z,int radius);
void vk_world_shutdown(void);

/* ── vk_hud.c ────────────────────────────────────────────────────────── */
void vk_ui_reset(void);
void vk_ui_quad(const VkUiVertex quad[4],int mode);
void vk_ui_triangle(const VkUiVertex *a,const VkUiVertex *b,const VkUiVertex *c,int mode);
void vk_ui_rect(float x,float y,float w,float h,uint32_t color);
void vk_ui_roundrect(float x,float y,float w,float h,float radius,uint32_t color);
void vk_ui_circle(float x,float y,float radius,uint32_t color);
void vk_ui_ring(float x,float y,float radius,float thickness,uint32_t color);
void vk_ui_line(float x1,float y1,float x2,float y2,float thickness,uint32_t color);
void vk_ui_image(const Image *image,float x,float y,float w,float h);
void vk_ui_flush(void);
void vk_ui_draw_present(VkImageView scene_view,int scene_width,int scene_height);

/* ── vk_text.c ───────────────────────────────────────────────────────── */
int  vk_textures_init(AAssetManager *assets);
void vk_textures_shutdown(void);
void vk_font_upload(void);
int  vk_text_white_layer(void);
void vk_text_draw(const char *string,float x,float y,uint32_t color,float scale);

/* ── vk_backend.c ────────────────────────────────────────────────────── */
void vk_stage_triangle(const VkWorldVertex *vertices,int count);
void vk_stage_line(const VkLineVertex *vertices,int count);
void vk_flush_lines(void);
void vk_flush_triangles(void);
void vk_clear_depth_region(float x0,float y0,float x1,float y1);
void vk_immediate_reset(void);

#endif
