/* TrueType text pass: the device counterpart of the preview's browser overlay.
 *
 * A script calls `font.load("font.ttf")`, the runtime keeps the bytes in the
 * font registry, and this pass turns every recorded `render.text` with a valid
 * font handle into tinted glyph quads on top of a per-font RGBA atlas — plain
 * textured triangles, so the Vulkan pipeline and the software fallback draw
 * exactly the same pixels with zero shader changes.
 *
 * Glyphs rasterize on demand (CPU scanline fill, nonzero winding, 3x3 box
 * antialiasing) and stay cached in the atlas; the image revision is bumped
 * when new glyphs land, which the renderer already watches to re-upload
 * textures.  Texts with the default face (-1) stay recorded but undrawn, same
 * as before: a game that wants pixels must load a font first.
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

/* Turn this frame's recorded texts with valid fonts into tinted glyph quads.
 * Called once per frame by renderer_render, before the batch is uploaded. */
void ds_ttf_resolve_frame(void);

/* Drop every face and atlas.  Atlas pixels belong to the image registry and
 * are freed by ds_image_reset, never here. */
void ds_ttf_reset(void);

#ifdef __cplusplus
}
#endif
#endif /* DS_TTF_H */
