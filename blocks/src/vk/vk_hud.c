/* src/vk/vk_hud.c — the 2D HUD batch and the upscale draw.
 *
 * The HUD is drawn by one vertex buffer per frame in flight. Every primitive
 * appends triangles and remembers which pipeline it needs (flat colour, the
 * glyph atlas or a block icon); the segments are replayed in order when the
 * frame ends, so the blend order is exactly the order the HUD code draws in.
 *
 * The same pass also scales the internal 3D image onto the frame, which is the
 * Vulkan equivalent of the software renderer's bilinear upscale. */
#include "vk_internal.h"

typedef struct {
    int mode;          /* 0 solid, 1 glyphs, 2 image */
    size_t first, count;
} VkUiSegment;

enum { VK_UI_SEGMENTS = 1024 };
static VkUiSegment segments[VK_UI_SEGMENTS];
static int segment_count;
static int overflow_reported;

static VkImmediate *batch(void) { return &vk.ui[vk.frame_index]; }

void vk_ui_reset(void) {
    segment_count = 0;
    if (vk.ui[vk.frame_index].gpu.buffer) { vk.ui[vk.frame_index].count = 0; vk.ui[vk.frame_index].recorded = 0; }
}

static void append(int mode, const VkUiVertex *vertices, int count) {
    VkImmediate *target = batch();
    if (!target->gpu.buffer || !target->gpu.mapped) return;
    if (target->count + (size_t)count > target->capacity) {
        if (!overflow_reported) { app_log_error("vulkan: HUD batch is full, dropping geometry"); overflow_reported = 1; }
        return;
    }
    VkUiVertex *destination = (VkUiVertex *)target->gpu.mapped + target->count;
    memcpy(destination, vertices, (size_t)count * sizeof(VkUiVertex));
    if (segment_count == 0 || segments[segment_count - 1].mode != mode) {
        if (segment_count >= VK_UI_SEGMENTS) return;
        segments[segment_count].mode = mode;
        segments[segment_count].first = target->count;
        segments[segment_count].count = 0;
        segment_count++;
    }
    segments[segment_count - 1].count += (size_t)count;
    target->count += (size_t)count;
}

void vk_ui_triangle(const VkUiVertex *a, const VkUiVertex *b, const VkUiVertex *c, int mode) {
    VkUiVertex triangle[3] = { *a, *b, *c };
    append(mode, triangle, 3);
}

void vk_ui_quad(const VkUiVertex quad[4], int mode) {
    VkUiVertex triangles[6] = { quad[0], quad[1], quad[2], quad[0], quad[2], quad[3] };
    append(mode, triangles, 6);
}

static VkUiVertex vertex(float x, float y, uint32_t color) {
    VkUiVertex out;
    out.x = x; out.y = y;
    out.r = ((color >> 16) & 0xff) / 255.0f;
    out.g = ((color >> 8) & 0xff) / 255.0f;
    out.b = (color & 0xff) / 255.0f;
    out.a = (color >> 24) & 0xff;
    out.a = out.a == 0.0f ? 1.0f : out.a / 255.0f;   /* gfx_pack() semantics */
    out.u = out.v = 0.0f;
    out.layer = 0.0f;
    return out;
}

void vk_ui_rect(float x, float y, float w, float h, uint32_t color) {
    if (!isfinite(x + y + w + h) || w <= 0 || h <= 0) return;
    VkUiVertex quad[4] = {
        vertex(x,     y,     color), vertex(x + w, y,     color),
        vertex(x + w, y + h, color), vertex(x,     y + h, color),
    };
    vk_ui_quad(quad, 0);
}

/* Circular arc fan: the centre plus points along the arc. Used for the
 * round-rectangle corners, circle bodies and the line caps. */
static void arc(float cx, float cy, float radius, float from, float to, int segments, uint32_t color) {
    VkUiVertex centre = vertex(cx, cy, color);
    float step = (to - from) / (float)segments;
    VkUiVertex previous;
    for (int i = 0; i <= segments; i++) {
        float angle = from + step * (float)i;
        VkUiVertex point = vertex(cx + cosf(angle) * radius, cy + sinf(angle) * radius, color);
        if (i) vk_ui_triangle(&centre, &previous, &point, 0);
        previous = point;
    }
}

void vk_ui_roundrect(float x, float y, float w, float h, float radius, uint32_t color) {
    if (!isfinite(x + y + w + h + radius) || w <= 0 || h <= 0) return;
    if (radius <= 0.5f) { vk_ui_rect(x, y, w, h, color); return; }
    if (radius > w * 0.5f) radius = w * 0.5f;
    if (radius > h * 0.5f) radius = h * 0.5f;
    const float half_pi = 1.57079632679490f;
    /* Straight band plus four quarter arcs: the same shape the software
     * renderer fills row by row. */
    vk_ui_rect(x, y + radius, w, h - 2.0f * radius, color);
    arc(x + radius,         y + radius,         radius, 2.0f * half_pi, 3.0f * half_pi, 6, color);
    arc(x + w - radius,     y + radius,         radius, 3.0f * half_pi, 4.0f * half_pi, 6, color);
    arc(x + w - radius,     y + h - radius,     radius, 0.0f,           half_pi,         6, color);
    arc(x + radius,         y + h - radius,     radius, half_pi,        2.0f * half_pi,   6, color);
}

void vk_ui_circle(float x, float y, float radius, uint32_t color) {
    if (!isfinite(x + y + radius) || radius <= 0) return;
    arc(x, y, radius, 0.0f, 6.28318530717959f, 40, color);
}

void vk_ui_ring(float x, float y, float radius, float thickness, uint32_t color) {
    if (!isfinite(x + y + radius + thickness) || radius <= 0 || thickness <= 0) return;
    float inner = radius - thickness;
    if (inner <= 0.5f) { vk_ui_circle(x, y, radius, color); return; }
    enum { SEGMENTS = 48 };
    for (int i = 0; i < SEGMENTS; i++) {
        float a0 = 6.28318530717959f * (float)i / (float)SEGMENTS;
        float a1 = 6.28318530717959f * (float)(i + 1) / (float)SEGMENTS;
        VkUiVertex quad[4] = {
            vertex(x + cosf(a0) * radius, y + sinf(a0) * radius, color),
            vertex(x + cosf(a1) * radius, y + sinf(a1) * radius, color),
            vertex(x + cosf(a1) * inner,  y + sinf(a1) * inner,  color),
            vertex(x + cosf(a0) * inner,  y + sinf(a0) * inner,  color),
        };
        vk_ui_quad(quad, 0);
    }
}

void vk_ui_line(float x1, float y1, float x2, float y2, float thickness, uint32_t color) {
    if (!isfinite(x1 + y1 + x2 + y2 + thickness) || thickness <= 0) return;
    float dx = x2 - x1, dy = y2 - y1;
    float length = sqrtf(dx * dx + dy * dy);
    float half = thickness * 0.5f;
    if (length < 0.01f) { vk_ui_circle(x1, y1, half, color); return; }
    float nx = -dy / length * half, ny = dx / length * half;
    VkUiVertex quad[4] = {
        vertex(x1 + nx, y1 + ny, color), vertex(x2 + nx, y2 + ny, color),
        vertex(x2 - nx, y2 - ny, color), vertex(x1 - nx, y1 - ny, color),
    };
    vk_ui_quad(quad, 0);
    float base = atan2f(dy, dx);
    arc(x1, y1, half, base + 1.57079632679490f, base + 4.71238898038469f, 8, color);
    arc(x2, y2, half, base - 1.57079632679490f, base + 1.57079632679490f, 8, color);
}

void vk_ui_image(const Image *image, float x, float y, float w, float h) {
    if (!image || !image->pixels || w <= 0 || h <= 0) return;
    int layer = geometrium_material_layer_of(image);
    VkUiVertex quad[4] = {
        vertex(x,     y,     0xffffffffu), vertex(x + w, y,     0xffffffffu),
        vertex(x + w, y + h, 0xffffffffu), vertex(x,     y + h, 0xffffffffu),
    };
    /* Sample inside the tile (a half texel inset) to avoid bleeding. */
    const float inset = 0.5f / 32.0f;
    quad[0].u = inset;         quad[0].v = inset;
    quad[1].u = 1.0f - inset;  quad[1].v = inset;
    quad[2].u = 1.0f - inset;  quad[2].v = 1.0f - inset;
    quad[3].u = inset;         quad[3].v = 1.0f - inset;
    for (int i = 0; i < 4; i++) quad[i].layer = (float)layer / 255.0f;
    vk_ui_quad(quad, 2);
}

void vk_ui_flush(void) {
    VkImmediate *target = batch();
    if (!target->gpu.buffer || !target->gpu.mapped || target->count == 0) return;
    VkCommandBuffer cmd = vk.cmd[vk.frame_index];
    VkDeviceSize offset = 0;
    for (int i = 0; i < segment_count; i++) {
        const VkUiSegment *segment = &segments[i];
        if (segment->count < 3) continue;
        VkPipeline pipeline = segment->mode == 1 ? vk.pipe_ui_glyph :
                              segment->mode == 2 ? vk.pipe_ui_image : vk.pipe_ui_solid;
        VkDescriptorSet set = segment->mode == 1 ? vk.set_glyphs : vk.set_tiles;
        if (!pipeline) continue;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.layout2d, 0, 1, &set, 0, NULL);
        vk_push2d(segment->mode, 1.0f);
        offset = (VkDeviceSize)(segment->first * sizeof(VkUiVertex));
        vkCmdBindVertexBuffers(cmd, 0, 1, &target->gpu.buffer, &offset);
        vkCmdDraw(cmd, (uint32_t)(segment->count - segment->count % 3), 1, (uint32_t)segment->first, 0);
    }
}

void vk_ui_draw_present(VkImageView scene_view, int scene_width, int scene_height) {
    (void)scene_view;
    if (!vk.pipe_present || !vk.scene.color) return;
    VkCommandBuffer cmd = vk.cmd[vk.frame_index];
    VkViewport viewport = {0, 0, (float)vk.width, (float)vk.height, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {(uint32_t)vk.width, (uint32_t)vk.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.pipe_present);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.layout_present, 0, 1, &vk.set_scene, 0, NULL);
    vk_push_present();
    vkCmdDraw(cmd, 3, 1, 0, 0);
}
