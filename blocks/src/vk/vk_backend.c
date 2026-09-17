/* src/vk/vk_backend.c — the Vulkan renderer behind the RenderBackend table.
 *
 * Frame shape (mirrors the software renderer exactly):
 *
 *   begin_frame   acquires the frame, resets the staging batches
 *   begin3d       creates/resizes the internal 3D target, sets up the camera
 *                 matrices and opens the 3D render pass
 *   sky / fog     full-screen gradient and the fog range
 *   world         vk_world.c draws the chunk meshes (opaque, then water)
 *   hand/target   camera-space polygons and world-space outline segments
 *   end3d         closes the 3D pass
 *   HUD           the 2D primitives go into the HUD batch
 *   end_frame     upscales the 3D image, replays the HUD, reads the frame back
 *
 * Culling, the projection and the depth ranges replicate the software
 * rasterizer (geometrium_render.c) so both backends agree pixel for pixel:
 * the same view rotation, the same focal length, the same radial fog and the
 * same "clear the depth under the hand" trick (vkCmdClearAttachments here). */
#include "vk_internal.h"
#include "../geometrium/geometrium_internal.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* The software projection clips at 0.04; keep the same near plane so the
 * hand's depth-clear rectangle covers exactly the same pixels. */
#define VK_NEAR_Z 0.04f

/* ── camera helpers (same math as geometrium_render.c) ───────────────── */

static void vk_to_view(float x, float y, float z, float out[3]) {
    if (vk.viewmodel) { out[0] = x; out[1] = y; out[2] = z; return; }
    float dx = x - vk.camx, dy = y - vk.camy, dz = z - vk.camz;
    float rx = dx * vk.yaw_c - dz * vk.yaw_s;
    float rz = dx * vk.yaw_s + dz * vk.yaw_c;
    out[0] = rx;
    out[1] = dy * vk.pitch_c - rz * vk.pitch_s;
    out[2] = dy * vk.pitch_s + rz * vk.pitch_c;
}

static int vk_project_view(const float v[3], float *sx, float *sy) {
    if (v[2] < VK_NEAR_Z) return 0;
    float inverse = vk.foc / v[2];
    *sx = (float)vk.scene.width * 0.5f + v[0] * inverse;
    *sy = (float)vk.scene.height * 0.5f - v[1] * inverse;
    return 1;
}

/* Column-major clip matrices: `view_matrix` is projection * view for world
 * geometry, `proj_matrix` is the bare projection for camera-space geometry
 * (the hand). vk_push3d() picks between them with the viewmodel flag.
 *
 * The software renderer projects sx = w/2 + x*foc/z and sy = h/2 - y*foc/z, so
 * the Vulkan projection is px = 2*foc/w on x, py = -2*foc/h on y (the sign
 * flips y for the framebuffer's top-left origin) and depth in [0,1]. */
static void vk_update_matrices(void) {
    float rotation[3][3] = {
        { vk.yaw_c, 0.0f, -vk.yaw_s },
        { -vk.yaw_s * vk.pitch_s, vk.pitch_c, -vk.yaw_c * vk.pitch_s },
        { vk.yaw_s * vk.pitch_c, vk.pitch_s, vk.yaw_c * vk.pitch_c },
    };
    float far_z = GEOMETRIUM_FAR_Z;
    float px = 2.0f * vk.foc / (float)vk.scene.width;
    float py = -2.0f * vk.foc / (float)vk.scene.height;
    float depth_scale = far_z / (far_z - VK_NEAR_Z);
    float depth_shift = -far_z * VK_NEAR_Z / (far_z - VK_NEAR_Z);
    float eye[3] = { vk.camx, vk.camy, vk.camz };

    for (int i = 0; i < 16; i++) vk.view_matrix[i] = vk.proj_matrix[i] = 0.0f;
    vk.proj_matrix[0] = px;
    vk.proj_matrix[5] = py;
    vk.proj_matrix[10] = depth_scale;
    vk.proj_matrix[14] = depth_shift;
    vk.proj_matrix[11] = 1.0f;

    for (int column = 0; column < 3; column++) {
        vk.view_matrix[column * 4 + 0] = px * rotation[0][column];
        vk.view_matrix[column * 4 + 1] = py * rotation[1][column];
        vk.view_matrix[column * 4 + 2] = depth_scale * rotation[2][column];
        vk.view_matrix[column * 4 + 3] = rotation[2][column];
    }
    float dot0 = rotation[0][0] * eye[0] + rotation[0][1] * eye[1] + rotation[0][2] * eye[2];
    float dot1 = rotation[1][0] * eye[0] + rotation[1][1] * eye[1] + rotation[1][2] * eye[2];
    float dot2 = rotation[2][0] * eye[0] + rotation[2][1] * eye[1] + rotation[2][2] * eye[2];
    vk.view_matrix[12] = -px * dot0;
    vk.view_matrix[13] = -py * dot1;
    vk.view_matrix[14] = depth_shift - depth_scale * dot2;
    vk.view_matrix[15] = -dot2;
}

/* ── staging batches ─────────────────────────────────────────────────── */

void vk_immediate_reset(void) {
    for (int i = 0; i < VK_FRAMES_IN_FLIGHT; i++) {
        vk.lines[i].count = vk.lines[i].recorded = 0;
        vk.triangles[i].count = vk.triangles[i].recorded = 0;
    }
}

static int append_world(VkWorldVertex *vertices, int count) {
    VkImmediate *batch = &vk.triangles[vk.frame_index];
    if (!batch->gpu.buffer || !batch->gpu.mapped) return 0;
    if (batch->count + (size_t)count > batch->capacity) {
        app_log_error("vulkan: 3D staging buffer is full, dropping geometry");
        return 0;
    }
    memcpy((VkWorldVertex *)batch->gpu.mapped + batch->count, vertices, (size_t)count * sizeof(*vertices));
    batch->count += (size_t)count;
    return 1;
}

void vk_stage_triangle(const VkWorldVertex *vertices, int count) {
    if (count < 3) return;
    /* Convex fan: every polygon the game submits is convex. */
    for (int i = 1; i + 1 < count; i++) {
        VkWorldVertex triangle[3] = { vertices[0], vertices[i], vertices[i + 1] };
        if (!append_world(triangle, 3)) return;
    }
}

void vk_stage_line(const VkLineVertex *vertices, int count) {
    VkImmediate *batch = &vk.lines[vk.frame_index];
    if (count < 2 || !batch->gpu.buffer || !batch->gpu.mapped) return;
    if (batch->count + (size_t)count > batch->capacity) {
        app_log_error("vulkan: line staging buffer is full, dropping geometry");
        return;
    }
    memcpy((VkLineVertex *)batch->gpu.mapped + batch->count, vertices, (size_t)count * sizeof(*vertices));
    batch->count += (size_t)count;
}

void vk_flush_triangles(void) {
    VkImmediate *batch = &vk.triangles[vk.frame_index];
    if (!vk.pass3d_active || !batch->gpu.buffer || batch->recorded >= batch->count) return;
    VkCommandBuffer cmd = vk.cmd[vk.frame_index];
    VkViewport viewport = {0.0f, 0.0f, (float)vk.scene.width, (float)vk.scene.height, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {(uint32_t)vk.scene.width, (uint32_t)vk.scene.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipe_world_opaque);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.layout3d, 0, 1, &vk.set_tiles, 0, NULL);
    vk_push3d(1.0f);
    VkDeviceSize offset = (VkDeviceSize)batch->recorded * sizeof(VkWorldVertex);
    vkCmdBindVertexBuffers(cmd, 0, 1, &batch->gpu.buffer, &offset);
    vkCmdDraw(cmd, (uint32_t)(batch->count - batch->recorded), 1, (uint32_t)batch->recorded, 0);
    batch->recorded = batch->count;
}

void vk_flush_lines(void) {
    VkImmediate *batch = &vk.lines[vk.frame_index];
    if (!vk.pass3d_active || !batch->gpu.buffer || batch->recorded >= batch->count) return;
    VkCommandBuffer cmd = vk.cmd[vk.frame_index];
    VkViewport viewport = {0.0f, 0.0f, (float)vk.scene.width, (float)vk.scene.height, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {(uint32_t)vk.scene.width, (uint32_t)vk.scene.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipe_line);
    vk_push_line();
    VkDeviceSize offset = (VkDeviceSize)batch->recorded * sizeof(VkLineVertex);
    vkCmdBindVertexBuffers(cmd, 0, 1, &batch->gpu.buffer, &offset);
    vkCmdDraw(cmd, (uint32_t)(batch->count - batch->recorded), 1, (uint32_t)batch->recorded, 0);
    batch->recorded = batch->count;
}

/* Clears the depth of a rectangle, like the software zbuffer clear that keeps
 * the viewmodel from being eaten by the world behind it. */
void vk_clear_depth_region(float x0, float y0, float x1, float y1) {
    if (!vk.pass3d_active) return;
    vk_flush_triangles();
    vk_flush_lines();
    float width = (float)vk.scene.width, height = (float)vk.scene.height;
    if (x0 > x1) { float swap = x0; x0 = x1; x1 = swap; }
    if (y0 > y1) { float swap = y0; y0 = y1; y1 = swap; }
    int ix0 = (int)fmaxf(0.0f, floorf(x0)), iy0 = (int)fmaxf(0.0f, floorf(y0));
    int ix1 = (int)fminf(width, ceilf(x1)), iy1 = (int)fminf(height, ceilf(y1));
    if (ix1 <= ix0 || iy1 <= iy0) return;
    VkClearAttachment attachment = {0};
    attachment.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    attachment.colorAttachment = 1;    /* the depth attachment of pass3d */
    attachment.clearValue.depthStencil.depth = 1.0f;   /* "infinitely far" */
    VkClearRect rect = {{{ix0, iy0}, {(uint32_t)(ix1 - ix0), (uint32_t)(iy1 - iy0)}}, 0, 1};
    vkCmdClearAttachments(vk.cmd[vk.frame_index], 1, &attachment, 1, &rect);
}

/* ── frame and 3D scene ──────────────────────────────────────────────── */

static int vk_begin_frame(Buffer *buffer) { return vk_frame_begin(buffer); }
static void vk_end_frame(void) { vk_frame_end(); }
static void vk_cancel_frame(void) { vk_frame_cancel(); }

static int vk_begin_3d(Buffer *buffer, int scale, float cx, float cy, float cz,
                       float yaw, float pitch, float fov_deg) {
    vk.pass3d_active = 0;
    if (!vk.ready || !buffer || !buffer->pixels || buffer->width <= 0 || buffer->height <= 0) return 0;
    if (!isfinite(cx + cy + cz + yaw + pitch + fov_deg) || fov_deg < 5.0f || fov_deg > 175.0f) return 0;
    if (scale < 1) scale = 1;
    vk.scale = scale;
    vk.scene_width = buffer->width / scale;
    vk.scene_height = buffer->height / scale;
    if (vk.scene_width < 8 || vk.scene_height < 8) return 0;
    if (!vk_scene_target_ensure(vk.scene_width, vk.scene_height)) return 0;
    vk.frame = buffer;
    vk.frame_has_pixels = 1;

    vk.camx = cx; vk.camy = cy; vk.camz = cz;
    vk.yaw_s = sinf(yaw); vk.yaw_c = cosf(yaw);
    vk.pitch_s = sinf(pitch); vk.pitch_c = cosf(pitch);
    float focal = (0.5f * (float)vk.scene_height) / tanf(fov_deg * (float)M_PI / 360.0f);
    vk.foc = focal;
    vk.view_x = 0.5f * (float)vk.scene_width / focal;
    vk.view_y = 0.5f * (float)vk.scene_height / focal;
    vk.side_x = sqrtf(1.0f + vk.view_x * vk.view_x);
    vk.side_y = sqrtf(1.0f + vk.view_y * vk.view_y);
    vk.viewmodel = 0;
    vk.fog_start = GEOMETRIUM_FOG_START;
    vk.fog_end = GEOMETRIUM_FOG_END;
    vk_update_matrices();
    vk_begin_scene_pass();
    return 1;
}

static void vk_sky(uint32_t top, uint32_t bottom) {
    vk.sky_top = top;
    vk.sky_bottom = bottom;
    vk.fog_color = bottom;   /* distant blocks fade into the horizon colour */
    if (!vk.pass3d_active) return;
    VkCommandBuffer cmd = vk.cmd[vk.frame_index];
    VkViewport viewport = {0.0f, 0.0f, (float)vk.scene.width, (float)vk.scene.height, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {(uint32_t)vk.scene.width, (uint32_t)vk.scene.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipe_sky);
    vk_push_sky();
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

static void vk_fog(float start, float end) {
    if (!isfinite(start + end) || start < 0.0f || end <= start) return;
    vk.fog_start = start;
    vk.fog_end = end;
}

static VkWorldVertex vk_world_vertex(const GeometriumVertex *in, const float normal[3],
                                     unsigned char light, uint32_t color, int layer) {
    float shade = geometrium_vertex_shade(normal[0], normal[1], normal[2], light);
    if (!isfinite(shade)) shade = 0.0f;
    VkWorldVertex out;
    out.x = in->x; out.y = in->y; out.z = in->z;
    out.u = in->u; out.v = in->v;
    out.r = (unsigned char)((color >> 16) & 0xff);
    out.g = (unsigned char)((color >> 8) & 0xff);
    out.b = (unsigned char)(color & 0xff);
    out.a = (unsigned char)((color >> 24) & 0xff);
    if (out.a == 0) out.a = 255;
    out.shade = (unsigned char)(fminf(fmaxf(shade, 0.0f), 1.0f) * 255.0f + 0.5f);
    out.layer = (unsigned char)layer;
    out.pad0 = out.pad1 = 0;
    return out;
}

static int vk_face_front(float nx, float ny, float nz, const float point[3]) {
    float ex = vk.viewmodel ? 0.0f : vk.camx;
    float ey = vk.viewmodel ? 0.0f : vk.camy;
    float ez = vk.viewmodel ? 0.0f : vk.camz;
    return nx * (ex - point[0]) + ny * (ey - point[1]) + nz * (ez - point[2]) > 0.0f;
}

static void vk_surface(int sx, int sy, int sz, int u, int v, int face, int block, const unsigned char light[4]) {
    if (!vk.pass3d_active || face < 0 || face >= 6 || u <= 0 || v <= 0) return;
    GeometriumVertex corners[4];
    unsigned char corner_light[4];
    float normal[3];
    geometrium_quad_build(sx, sy, sz, u, v, face, light, corners, corner_light, normal);
    if (!vk_face_front(normal[0], normal[1], normal[2], (float[]){ corners[0].x, corners[0].y, corners[0].z })) return;
    int layer = geometrium_material_layer(block, face);
    VkWorldVertex quad[4];
    for (int i = 0; i < 4; i++)
        quad[i] = vk_world_vertex(&corners[i], normal, corner_light[i], 0xffffffffu, layer);
    vk_stage_triangle(quad, 4);
}

static void vk_polygon(const GeometriumVertex *vertices, int n, float nx, float ny, float nz,
                       uint32_t color, GeometriumMaterial *material, const unsigned char light[4]) {
    if (!vk.pass3d_active || !vertices || n < 3 || n > 8) return;
    float point[3] = { vertices[0].x, vertices[0].y, vertices[0].z };
    if (!vk_face_front(nx, ny, nz, point)) return;
    float normal[3] = { nx, ny, nz };
    int layer = material ? material->layer : vk_text_white_layer();
    VkWorldVertex fan[8];
    for (int i = 0; i < n; i++)
        fan[i] = vk_world_vertex(&vertices[i], normal, light ? light[i] : 255,
                                 material ? 0xffffffffu : color, layer);
    vk_stage_triangle(fan, n);
}

static void vk_segment(float x, float y, float z, float x2, float y2, float z2, uint32_t color) {
    if (!vk.pass3d_active || !isfinite(x + y + z + x2 + y2 + z2)) return;
    VkLineVertex line[2];
    float alpha = ((color >> 24) & 0xff) / 255.0f;
    if (alpha == 0.0f) alpha = 1.0f;
    line[0].x = x; line[0].y = y; line[0].z = z;
    line[1].x = x2; line[1].y = y2; line[1].z = z2;
    for (int i = 0; i < 2; i++) {
        line[i].r = ((color >> 16) & 0xff) / 255.0f;
        line[i].g = ((color >> 8) & 0xff) / 255.0f;
        line[i].b = (color & 0xff) / 255.0f;
        line[i].a = alpha;
    }
    vk_stage_line(line, 2);
}

static int vk_visible(float x, float y, float z, float hx, float hy, float hz) {
    if (!vk.pass3d_active || !isfinite(x + y + z + hx + hy + hz)) return 0;
    float center[3];
    vk_to_view(x, y, z, center);
    float radius = sqrtf(hx * hx + hy * hy + hz * hz);
    float far_plane = vk.fog_end > vk.fog_start ? vk.fog_end : GEOMETRIUM_FOG_END;
    if (center[0] * center[0] + center[1] * center[1] + center[2] * center[2] >
        (far_plane + radius) * (far_plane + radius)) return 0;
    return center[2] + radius >= VK_NEAR_Z && center[2] - radius <= GEOMETRIUM_FAR_Z &&
           fabsf(center[0]) - center[2] * vk.view_x <= radius * vk.side_x &&
           fabsf(center[1]) - center[2] * vk.view_y <= radius * vk.side_y;
}

static int vk_face_visible(int face, float plane) {
    if (!vk.pass3d_active) return 0;
    float eye = face < 2 ? vk.camy : face < 4 ? vk.camz : vk.camx;
    return (face & 1) ? eye < plane : eye > plane;
}

static int vk_project(float x, float y, float z, float *sx, float *sy) {
    if (!vk.pass3d_active || !sx || !sy || !isfinite(x + y + z)) return 0;
    float view[3];
    vk_to_view(x, y, z, view);
    return vk_project_view(view, sx, sy);
}

static void vk_depth_clear(float x0, float y0, float x1, float y1) {
    vk_clear_depth_region(x0, y0, x1, y1);
}

static void vk_viewmodel(int enabled) {
    if (!vk.pass3d_active) return;
    int wanted = enabled != 0;
    if (wanted != vk.viewmodel) {
        /* Anything already staged belongs to the other transform. */
        vk_flush_triangles();
        vk_flush_lines();
    }
    vk.viewmodel = wanted;
}

static void vk_end_3d(void) {
    vk_flush_triangles();
    vk_flush_lines();
    vk_end_scene_pass();
}

/* ── 2D ──────────────────────────────────────────────────────────────── */

static void vk_rect(float x, float y, float w, float h, uint32_t color) { vk_ui_rect(x, y, w, h, color); }
static void vk_roundrect(float x, float y, float w, float h, float radius, uint32_t color) {
    vk_ui_roundrect(x, y, w, h, radius, color);
}
static void vk_circle(float x, float y, float radius, uint32_t color) { vk_ui_circle(x, y, radius, color); }
static void vk_ring(float x, float y, float radius, float thickness, uint32_t color) {
    vk_ui_ring(x, y, radius, thickness, color);
}
static void vk_line_2d(float x1, float y1, float x2, float y2, float thickness, uint32_t color) {
    vk_ui_line(x1, y1, x2, y2, thickness, color);
}
static void vk_image(const Image *image, float x, float y, float w, float h) { vk_ui_image(image, x, y, w, h); }
static void vk_text(const char *string, float x, float y, uint32_t color, float scale) {
    vk_text_draw(string, x, y, color, scale);
}
static int vk_text_width(const char *string) { return gfx_metrics_width(string); }

static void vk_error_screen(const char *message) {
    if (!vk.ready) return;
    vk_ui_reset();
    vk_ui_rect(0.0f, 0.0f, (float)vk.width, (float)vk.height, 0xff201a1au);
    vk_text_draw("Enjoer - error", 16.0f, 16.0f, 0xffffffffu, 0.7f);
    if (message) vk_text_draw(message, 16.0f, 56.0f, 0xffffb0b0u, 0.45f);
}

/* ── backend table ───────────────────────────────────────────────────── */

static int vk_backend_init(AAssetManager *assets) {
    if (!vk_device_init(assets)) { vk_device_shutdown(); return 0; }
    vk_immediate_reset();
    vk.sky_top = 0xff78b8e8u;
    vk.sky_bottom = 0xffc7e5f5u;
    vk.fog_color = vk.sky_bottom;
    vk.fog_start = GEOMETRIUM_FOG_START;
    vk.fog_end = GEOMETRIUM_FOG_END;
    vk.scene_width = vk.width;
    vk.scene_height = vk.height;
    return 1;
}

static void vk_backend_shutdown(void) { vk_device_shutdown(); }

const char *vk_last_error(void) { return vk.error[0] ? vk.error : "no Vulkan device"; }

const RenderBackend vk_render_backend = {
    .name = "Vulkan",
    .windowed = 0,
    .init = vk_backend_init,
    .shutdown = vk_backend_shutdown,
    .attach_window = vk_attach_window,
    .begin_frame = vk_begin_frame,
    .end_frame = vk_end_frame,
    .cancel_frame = vk_cancel_frame,
    .error_screen = vk_error_screen,
    .rect = vk_rect,
    .roundrect = vk_roundrect,
    .circle = vk_circle,
    .ring = vk_ring,
    .line = vk_line_2d,
    .image_draw = vk_image,
    .text_scaled = vk_text,
    .text_width = vk_text_width,
    .begin3d = vk_begin_3d,
    .sky = vk_sky,
    .fog = vk_fog,
    .surface = vk_surface,
    .segment = vk_segment,
    .visible = vk_visible,
    .face_visible = vk_face_visible,
    .project = vk_project,
    .depth_clear = vk_depth_clear,
    .viewmodel = vk_viewmodel,
    .polygon = vk_polygon,
    .chunks = vk_world_draw,
    .end3d = vk_end_3d,
};
