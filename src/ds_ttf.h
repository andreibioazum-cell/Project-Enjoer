/* TrueType text pass: the device counterpart of the preview's browser overlay.
 *
 * A script calls `font.load("font.ttf")`, the runtime keeps the bytes in the
 * font registry, and this pass turns every recorded `render.text` with a valid
 * font handle into tinted glyph quads on top of a per-font RGBA atlas — plain
 * textured triangles, so the Vulkan pipeline and the software fallback draw
 * exactly the same pixels with zero shader changes.
 *
 * Glyphs rasterize on demand (CPU scanline fill, nonzero winding, 3x3 box
 * antialiasing) at the size the text asks for — a 16 px label is a 16 px
 * bitmap, not a shrunk 48 px one — and stay cached in the atlas.  Each bake
 * marks its own atlas layer dirty, which is the only texture work a growing
 * score costs.  Glyphs that no longer fit the atlas retry at the base size
 * before they degrade.  Texts with the default face (-1) stay recorded but
 * undrawn, same as before: a game that wants pixels must load a font first.
 *
 * Scale contract (matches what the old browser overlay did): scale 1.0 is a
 * 16 px em box, so `render.text("hi", x, y, 2.0)` is 32 px tall.  Phones want
 * 2.0 and up for body text.
 *
 * Deliberately unsupported: hinting bytecode (glyph bytecode is skipped, the
 * outlines rasterize unhinted), kerning/GPOS (advances only) and colour emoji
 * (outline .notdef instead).  Anchor-matched composites and cmap formats 4/12
 * are handled, which covers every sane TrueType/OpenType font.
 */
#ifndef DS_TTF_H
#define DS_TTF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Parse the font's tables, allocate its glyph atlas and register the atlas as
 * an image layer.  Called by ds_font_load; returns the image layer, or -1
 * when the font bytes are broken (the registry entry stays valid, its texts
 * simply stay recorded but undrawn). */
int32_t ds_ttf_load_face(int32_t font);

/* Image layer of a font's atlas, or -1 when the face is not ready. */
int32_t ds_ttf_layer(int32_t font);

/* Edge of the square atlas, in texels.  A tool or a test needs it to turn a
 * glyph quad's uv span back into the cell size it was rasterized at, which is
 * how one checks that text is sampled 1:1 instead of shrunk. */
int32_t ds_ttf_atlas_size(void);

struct EnjoerTextCommand;

/* Resolve a single recorded text into glyph quads right where it was
 * recorded.  Texts go through this at record time (ds_render_text), which is
 * what keeps a label in its painter's-order slot between the shapes its
 * script drew before and after it. */
void ds_ttf_draw_command(const struct EnjoerTextCommand *command);

/* Pixel width of `text` laid out at `scale` (scale 1.0 is the 16 px em),
 * using the same glyph advances the draw path uses; multi-line text measures
 * its longest line.  0 when the face is missing or the string is empty. */
float ds_ttf_measure(int32_t font, const char *text, float scale);

/* Backstop for callers that record texts without resolving them (tests that
 * poke the batch by hand): turns every not-yet-resolved record with a valid
 * font into glyph quads and flags the frame resolved.  Called once per frame
 * by renderer_render, before the batch is uploaded. */
void ds_ttf_resolve_frame(void);

/* Drop every face and atlas.  Atlas pixels belong to the image registry and
 * are freed by ds_image_reset, never here. */
void ds_ttf_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* DS_TTF_H */
