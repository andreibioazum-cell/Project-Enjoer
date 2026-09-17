/* graphics/gfx_font.c — font loading, UTF-8 and text metrics.
 *
 * This file is renderer agnostic: it owns the baked glyph atlas and the
 * measurements, while gfx_text.c (software) and src/vk/vk_text.c (Vulkan)
 * only decide how to put those glyphs on screen. */
#include "gfx_font.h"

static Font *font;
static int font_tried;
static AAssetManager *assets;

AAssetManager *gfx_assets(void) { return assets; }

int gfx_font_load(AAssetManager *manager) {
    if (assets != manager) { gfx_font_unload(); assets = manager; }
    if (font) return 1;
    if (font_tried) return 0;
    font_tried = 1;
    uint8_t *data = NULL;
    size_t size = 0;
    if (!asset_read(assets, "fonts/ChillRoundGothic_Heavy.ttf", &data, &size)) {
        app_log_error("font not loaded: fonts/ChillRoundGothic_Heavy.ttf not found");
        return 0;
    }
    font = font_create(data, size, GFX_FONT_PIXEL_HEIGHT);
    free(data);
    if (!font) { app_log_error("font not loaded: could not parse TrueType font"); return 0; }
    app_log("font loaded: fonts/ChillRoundGothic_Heavy.ttf");
    return 1;
}

void gfx_font_unload(void) {
    font_destroy(font);
    font = NULL;
    font_tried = 0;
}

const Font *gfx_font(void) { return font; }

int gfx_utf8_decode(const char **c) {
    const uint8_t *p = (const uint8_t *)*c;
    int r;
    if (!p || !*p) return -1;
    if (*p < 0x80) r = *p++;
    else if ((*p&0xe0)==0xc0 && (p[1]&0xc0)==0x80) { r = ((*p&0x1f)<<6)|(p[1]&0x3f); p+=2; }
    else if ((*p&0xf0)==0xe0 && (p[1]&0xc0)==0x80 && (p[2]&0xc0)==0x80) { r=((*p&0x0f)<<12)|((p[1]&0x3f)<<6)|(p[2]&0x3f); p+=3; }
    else if ((*p&0xf8)==0xf0 && (p[1]&0xc0)==0x80 && (p[2]&0xc0)==0x80 && (p[3]&0xc0)==0x80) { r=((*p&7)<<18)|((p[1]&0x3f)<<12)|((p[2]&0x3f)<<6)|(p[3]&0x3f); p+=4; }
    else r = *p++;
    *c = (const char *)p; return r;
}

/* Same measurement as the HUD labels: the ink box of the whole string, so a
 * centred label stays centred whatever glyphs it uses. */
int gfx_metrics_width(const char *s) {
    if (!s || !gfx_font_load(assets)) return 0;
    const FontGlyph *ref = font_glyph(font, 'S');
    float pen = -(ref ? ref->bearing_x : 0);
    int first = 1; float minL = 0, maxR = 0;
    for (const char *c = s; *c;) {
        int cp = gfx_utf8_decode(&c);
        const FontGlyph *g = font_glyph(font, (uint32_t)cp);
        if (!g) continue;
        float dl = pen + g->bearing_x, dr = dl + g->width;
        if (first || dl < minL) minL = dl;
        if (first || dr > maxR) maxR = dr;
        first = 0; pen += g->advance;
    }
    return first ? 0 : (int)(maxR - minL + 0.5f);
}
