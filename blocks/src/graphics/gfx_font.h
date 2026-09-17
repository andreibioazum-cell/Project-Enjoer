/* Shared font services: asset reads, TTF atlas baking and text metrics.
 * Both render backends (the software rasterizer and the Vulkan one) need the
 * same glyph atlas and the same measured widths; only the way a glyph reaches
 * the framebuffer differs. */
#ifndef GFX_FONT_H
#define GFX_FONT_H

#include "engine.h"
#include "ttf/ttf_internal.h"

/* The pixel height the font atlas is baked at. */
#define GFX_FONT_PIXEL_HEIGHT 48

int gfx_font_load(AAssetManager *assets);
void gfx_font_unload(void);
const Font *gfx_font(void);
AAssetManager *gfx_assets(void);

int gfx_utf8_decode(const char **cursor);
/* Ink extent of a string in atlas units (same measurement the HUD uses). */
int gfx_metrics_width(const char *string);

#endif
