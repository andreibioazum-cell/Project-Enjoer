/* The geometry batch that DimScript builds and the renderer eats.
 *
 * Both halves of the engine talk to each other through plain triangle lists.
 * That keeps the Vulkan pipeline boring (one vertex format, one push constant
 * matrix, one texture array) and lets the software rasterizer used by the tests
 * reuse exactly what the GPU would have drawn.
 *
 * Coordinates are screen pixels with the origin in the top left corner, which
 * is what a 2D game author expects.  The renderer multiplies by an orthographic
 * matrix, so a script never has to care about clip space.
 *
 * Every vertex carries a texture coordinate and a layer:
 *
 *   layer < 0   untextured — the vertex colour is the pixel (shapes);
 *   layer >= 0  index into the image array loaded by `image.load("x.png")`.
 *
 * A shape therefore costs the same six floats it always did, and a sprite is
 * the same quad with its layer set, so nothing in the renderer needs a second
 * pipeline.
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
    float a; /* 1 is opaque; both rasterizers blend src-over-dst with it */
    float u;
    float v;
    float layer;
} EnjoerVertex;

/* One recorded render.text call.  `font` is the handle returned by font.load,
 * or -1 for the default face; the TrueType text pass turns every command with
 * a loaded font into tinted glyph quads before the batch is uploaded. */
typedef struct EnjoerTextCommand {
    char text[ENJOER_DRAW_TEXT_LENGTH];
    float x;
    float y;
    float scale;
    float r;
    float g;
    float b;
    float a;
    int32_t font;
    /* Set when the TrueType pass already turned this record into glyph quads.
     * Texts resolve the moment they are recorded, so a label keeps its place
     * in the painter's order instead of being appended after every shape. */
    int resolved;
} EnjoerTextCommand;

typedef struct EnjoerFrame {
    EnjoerVertex *vertices;
    int vertex_count;
    int vertex_capacity;
    EnjoerTextCommand texts[ENJOER_DRAW_MAX_TEXT];
    int text_count;
    /* Set by the TrueType text pass once it turned this frame's texts with
     * loaded fonts into glyph quads; the records stay for debugging.  As texts
     * now resolve at record time this is simply "every record with a loaded
     * font is quads already". */
    int texts_resolved;
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
 * renderer never implements shapes itself.  Every vertex carries a straight
 * (non-premultiplied) alpha alongside its colour: the renderers blend with
 * src-over-dst, so a translucent shadow or screen fade is just geometry. */
void enjoer_draw_triangle(float x0, float y0, float x1, float y1, float x2, float y2,
                          float r, float g, float b, float a);
void enjoer_draw_quad(float x0, float y0, float x1, float y1, float x2, float y2,
                      float x3, float y3, float r, float g, float b, float a);
void enjoer_draw_rect(float x, float y, float width, float height,
                      float r, float g, float b, float a);
void enjoer_draw_frame_rect(float x, float y, float width, float height, float thickness,
                            float r, float g, float b, float a);
void enjoer_draw_circle(float x, float y, float radius, int segments,
                        float r, float g, float b, float a);
void enjoer_draw_ring(float x, float y, float radius, float thickness, int segments,
                      float r, float g, float b, float a);
void enjoer_draw_line(float x0, float y0, float x1, float y1, float thickness,
                      float r, float g, float b, float a);

/* Textured quad: a sprite sheet region in normalised coordinates, tinted by the
 * colour (1, 1, 1, 1 is untinted). */
void enjoer_draw_image_quad(float x, float y, float width, float height,
                            float u0, float v0, float u1, float v1, int32_t layer,
                            float r, float g, float b, float a);

/* Same quad, rotated `angle` screen-space radians about its own centre (the
 * y-down rotation a game measures with atan2(dy, dx)): the way a spinning cube
 * or snowflake is drawn without a second pipeline. */
void enjoer_draw_image_quad_rot(float x, float y, float width, float height,
                                float u0, float v0, float u1, float v1, int32_t layer,
                                float r, float g, float b, float a, float angle);

#ifdef __cplusplus
}
#endif
#endif /* ENJOER_DRAW_H */
