/* Immediate-mode triangle batch.  Shapes are tessellated here so the Vulkan
 * pipeline and the software preview both only have to draw triangles. */
#include "enjoer_draw.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static EnjoerFrame frame;
static int frame_width = 1;
static int frame_height = 1;

static float clamp01(float value) {
    if (!(value == value)) return 0.0f; /* NaN */
    return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value;
}

EnjoerFrame *enjoer_frame(void) {
    return &frame;
}

static int reserve(int vertices) {
    if (frame.vertex_capacity >= vertices) return 1;
    int capacity = frame.vertex_capacity ? frame.vertex_capacity : 512;
    while (capacity < vertices && capacity < ENJOER_DRAW_MAX_VERTICES) capacity *= 2;
    if (capacity > ENJOER_DRAW_MAX_VERTICES) capacity = ENJOER_DRAW_MAX_VERTICES;
    if (capacity < vertices) {
        frame.overflow = 1;
        return 0;
    }
    EnjoerVertex *grown = (EnjoerVertex *)realloc(frame.vertices, (size_t)capacity * sizeof(EnjoerVertex));
    if (!grown) {
        fputs("Enjoer: out of memory while growing the draw batch\n", stderr);
        abort();
    }
    frame.vertices = grown;
    frame.vertex_capacity = capacity;
    return 1;
}

void enjoer_frame_begin(int width, int height) {
    frame_width = width > 0 ? width : 1;
    frame_height = height > 0 ? height : 1;
    frame.vertex_count = 0;
    frame.text_count = 0;
    frame.texts_resolved = 0;
    frame.has_clear = 0;
    frame.overflow = 0;
}

void enjoer_frame_end(void) {
    /* Nothing to release: the batch is a persistent scratch buffer that is
     * reused by every frame. */
}

void enjoer_frame_clear(int width, int height) {
    enjoer_frame_begin(width, height);
}

/* One vertex of the batch.  `layer` is negative for shapes, otherwise it names
 * the image the fragment shader samples: the batch is the only thing the
 * renderer knows about, so a sprite and a rectangle cost the same. */
static void push_vertex_full(float x, float y, float r, float g, float b, float u, float v,
                             float layer) {
    if (!reserve(frame.vertex_count + 1)) return;
    EnjoerVertex *vertex = &frame.vertices[frame.vertex_count];
    /* The batch keeps script order: triangles paint over each other in the order
     * the game drew them, with no depth test anywhere in the pipeline. */
    vertex->x = x;
    vertex->y = y;
    vertex->z = 0.0f;
    vertex->r = clamp01(r);
    vertex->g = clamp01(g);
    vertex->b = clamp01(b);
    vertex->u = u;
    vertex->v = v;
    vertex->layer = layer;
    ++frame.vertex_count;
}

static void push_vertex(float x, float y, float r, float g, float b) {
    push_vertex_full(x, y, r, g, b, 0.0f, 0.0f, -1.0f);
}

void enjoer_draw_triangle(float x0, float y0, float x1, float y1, float x2, float y2,
                          float r, float g, float b) {
    push_vertex(x0, y0, r, g, b);
    push_vertex(x1, y1, r, g, b);
    push_vertex(x2, y2, r, g, b);
}

void enjoer_draw_quad(float x0, float y0, float x1, float y1, float x2, float y2,
                      float x3, float y3, float r, float g, float b) {
    enjoer_draw_triangle(x0, y0, x1, y1, x2, y2, r, g, b);
    enjoer_draw_triangle(x0, y0, x2, y2, x3, y3, r, g, b);
}

void enjoer_draw_rect(float x, float y, float width, float height,
                      float r, float g, float b) {
    if (width < 0.0f) { x += width; width = -width; }
    if (height < 0.0f) { y += height; height = -height; }
    if (width <= 0.0f || height <= 0.0f) return;
    enjoer_draw_quad(x, y, x + width, y, x + width, y + height, x, y + height, r, g, b);
}

void enjoer_draw_frame_rect(float x, float y, float width, float height, float thickness,
                            float r, float g, float b) {
    float t = thickness > 0.0f ? thickness : 1.0f;
    if (width <= 0.0f || height <= 0.0f) return;
    if (t * 2.0f > width) t = width * 0.5f;
    if (t * 2.0f > height) t = height * 0.5f;
    enjoer_draw_rect(x, y, width, t, r, g, b);
    enjoer_draw_rect(x, y + height - t, width, t, r, g, b);
    enjoer_draw_rect(x, y + t, t, height - t - t, r, g, b);
    enjoer_draw_rect(x + width - t, y + t, t, height - t - t, r, g, b);
}

void enjoer_draw_circle(float x, float y, float radius, int segments,
                        float r, float g, float b) {
    if (radius <= 0.0f) return;
    int count = segments < 3 ? 24 : (segments > 64 ? 64 : segments);
    const float step = 6.28318530718f / (float)count;
    for (int index = 0; index < count; ++index) {
        const float a0 = step * (float)index;
        const float a1 = step * (float)(index + 1);
        enjoer_draw_triangle(x, y,
                             x + cosf(a0) * radius, y + sinf(a0) * radius,
                             x + cosf(a1) * radius, y + sinf(a1) * radius,
                             r, g, b);
    }
}

void enjoer_draw_ring(float x, float y, float radius, float thickness, int segments,
                      float r, float g, float b) {
    if (radius <= 0.0f) return;
    const float t = thickness > 0.0f ? (thickness > radius ? radius : thickness) : 1.0f;
    const float inner = radius - t;
    int count = segments < 3 ? 24 : (segments > 64 ? 64 : segments);
    const float step = 6.28318530718f / (float)count;
    for (int index = 0; index < count; ++index) {
        const float a0 = step * (float)index;
        const float a1 = step * (float)(index + 1);
        const float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
        enjoer_draw_quad(x + c0 * radius, y + s0 * radius,
                         x + c1 * radius, y + s1 * radius,
                         x + c1 * inner, y + s1 * inner,
                         x + c0 * inner, y + s0 * inner, r, g, b);
    }
}

void enjoer_draw_line(float x0, float y0, float x1, float y1, float thickness,
                      float r, float g, float b) {
    const float dx = x1 - x0;
    const float dy = y1 - y0;
    const float length = sqrtf(dx * dx + dy * dy);
    if (length <= 0.0001f) {
        if (thickness > 0.0f) enjoer_draw_circle(x0, y0, thickness * 0.5f, 10, r, g, b);
        return;
    }
    const float t = thickness > 0.0f ? thickness : 1.0f;
    const float nx = -dy / length * (t * 0.5f);
    const float ny = dx / length * (t * 0.5f);
    enjoer_draw_quad(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny,
                     r, g, b);
}

void enjoer_draw_image_quad(float x, float y, float width, float height,
                            float u0, float v0, float u1, float v1, int32_t layer,
                            float r, float g, float b) {
    if (width < 0.0f) { x += width; width = -width; }
    if (height < 0.0f) { y += height; height = -height; }
    if (width <= 0.0f || height <= 0.0f || layer < 0) return;
    const float layer_coordinate = (float)layer;
    /* Wound clockwise on screen (top-left, top-right, bottom-right, bottom-left)
     * so the same triangle order works for the CPU rasterizer and the GPU. */
    push_vertex_full(x, y, r, g, b, u0, v0, layer_coordinate);
    push_vertex_full(x + width, y, r, g, b, u1, v0, layer_coordinate);
    push_vertex_full(x + width, y + height, r, g, b, u1, v1, layer_coordinate);
    push_vertex_full(x, y, r, g, b, u0, v0, layer_coordinate);
    push_vertex_full(x + width, y + height, r, g, b, u1, v1, layer_coordinate);
    push_vertex_full(x, y + height, r, g, b, u0, v1, layer_coordinate);
}
