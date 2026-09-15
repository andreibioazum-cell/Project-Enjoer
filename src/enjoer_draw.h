/* The geometry batch that the DimScript runtime builds and the renderer eats.
 *
 * Both halves of the engine talk to each other through plain triangle lists.
 * That keeps the Vulkan pipeline boring (one vertex format, one push constant
 * matrix) and lets the software preview rasterizer reuse exactly what the GPU
 * would have drawn: a game that looks right in the browser looks right on the
 * device.
 *
 * Coordinates are screen pixels with the origin in the top left corner, which
 * is what a 2D game author expects.  The renderer multiplies by an orthographic
 * matrix, so a script never has to care about clip space.
 */
#ifndef ENJOER_DRAW_H
#define ENJOER_DRAW_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ENJOER_DRAW_MAX_VERTICES (16 * 1024)
#define ENJOER_DRAW_MAX_TEXT 64
#define ENJOER_DRAW_TEXT_LENGTH 160

typedef struct EnjoerVertex {
    float x;
    float y;
    float z;
    float r;
    float g;
    float b;
} EnjoerVertex;

/* One recorded render.text call.  There is intentionally no font backend yet,
 * so the Vulkan path only counts these, while the HTTP preview overlays them as
 * real browser text to make fontless games debuggable. */
typedef struct EnjoerTextCommand {
    char text[ENJOER_DRAW_TEXT_LENGTH];
    float x;
    float y;
    float scale;
    float r;
    float g;
    float b;
} EnjoerTextCommand;

typedef struct EnjoerFrame {
    EnjoerVertex *vertices;
    int vertex_count;
    int vertex_capacity;
    EnjoerTextCommand texts[ENJOER_DRAW_MAX_TEXT];
    int text_count;
    uint64_t text_total;
    float clear_color[3];
    int has_clear;
    int overflow;
} EnjoerFrame;

/* The frame the runtime is filling right now. */
EnjoerFrame *enjoer_frame(void);
void enjoer_frame_begin(int width, int height);
void enjoer_frame_end(void);
void enjoer_frame_clear(int width, int height);

/* Triangle level API.  Anything a game asks for becomes triangles here, so the
 * renderer never implements shapes itself. */
void enjoer_draw_triangle(float x0, float y0, float x1, float y1, float x2, float y2,
                          float r, float g, float b);
void enjoer_draw_quad(float x0, float y0, float x1, float y1, float x2, float y2,
                      float x3, float y3, float r, float g, float b);
void enjoer_draw_rect(float x, float y, float width, float height,
                      float r, float g, float b);
void enjoer_draw_frame_rect(float x, float y, float width, float height, float thickness,
                            float r, float g, float b);
void enjoer_draw_circle(float x, float y, float radius, int segments,
                        float r, float g, float b);
void enjoer_draw_ring(float x, float y, float radius, float thickness, int segments,
                      float r, float g, float b);
void enjoer_draw_line(float x0, float y0, float x1, float y1, float thickness,
                      float r, float g, float b);

#ifdef __cplusplus
}
#endif
#endif /* ENJOER_DRAW_H */
