/* graphics/gfx_text.c — software text rasterization.
 *
 * Anti-aliasing: glyphs are baked into the atlas larger (48px instead of 32)
 * and sampled bilinearly when drawn, not nearest-pixel — text stays smooth
 * at any scale (see atlas_sample). The atlas itself is owned by gfx_font.c,
 * which the Vulkan backend uses too. */
#include "gfx_internal.h"
#include "gfx_sw.h"
#include <math.h>

/* Bilinear sampling of the alpha atlas, clamped to the glyph box so
 * neighbouring glyphs cannot bleed into the edges. */
static uint8_t atlas_sample(const uint8_t *al, int aw, int ah, float x, float y,
                            float minx, float miny, float maxx, float maxy) {
    if (x < minx) x = minx;
    if (y < miny) y = miny;
    if (x > maxx) x = maxx;
    if (y > maxy) y = maxy;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= aw) x1 = aw - 1;
    if (y1 >= ah) y1 = ah - 1;
    float fx = x - (float)x0, fy = y - (float)y0;
    float a00 = (float)al[y0*aw + x0], a10 = (float)al[y0*aw + x1];
    float a01 = (float)al[y1*aw + x0], a11 = (float)al[y1*aw + x1];
    float top = a00 + (a10 - a00) * fx;
    float bot = a01 + (a11 - a01) * fx;
    float v = top + (bot - top) * fy;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (uint8_t)(v + 0.5f);
}

void gfx_render_text(Buffer *b, const char *s, float x, float y, uint32_t c, float sc) {
    const Font *font = gfx_font();
    if (!b || !font || !s || !isfinite(x+y+sc) || sc <= 0) return;
    int aw = font_aw(font), ah = font_ah(font);
    const uint8_t *al = font_alpha(font);
    float asc = font_ascent(font), lb = 0;
    const FontGlyph *ref = font_glyph(font, 'S');
    if (ref) { asc = ref->bearing_top; lb = ref->bearing_x; }
    float pen = x - lb*sc, base = y + asc*sc;
    float inv = 1.0f / sc;
    for (const char *cur = s; *cur;) {
        int cp = gfx_utf8_decode(&cur);
        if (cp == '\n') { pen = x - lb*sc; base += font_lineh(font)*sc; continue; }
        const FontGlyph *g = font_glyph(font, (uint32_t)cp);
        if (!g || g->width <= 0 || g->height <= 0) { if (g) pen += g->advance * sc; continue; }
        float sx0 = g->u0*aw, sy0 = g->v0*ah;
        float sx1 = g->u1*aw, sy1 = g->v1*ah;
        int dw = (int)ceilf(g->width*sc), dh = (int)ceilf(g->height*sc);
        int dx = (int)floorf(pen + g->bearing_x*sc), dy = (int)floorf(base - g->bearing_top*sc);
        for (int yy = 0; yy < dh; yy++) {
            int scr_y = dy + yy;
            if (scr_y < 0 || scr_y >= b->height) continue;
            /* screen-pixel center in atlas coordinates */
            float fy = sy0 + ((float)yy + 0.5f) * inv - 0.5f;
            for (int xx = 0; xx < dw; xx++) {
                int scr_x = dx + xx;
                if (scr_x < 0 || scr_x >= b->width) continue;
                float fx = sx0 + ((float)xx + 0.5f) * inv - 0.5f;
                uint8_t cov = atlas_sample(al, aw, ah, fx, fy, sx0, sy0, sx1 - 1.0f, sy1 - 1.0f);
                if (cov) {
                    uint32_t ca = (c & 0xffffff) | (((uint32_t)cov * (c>>24) / 255) << 24);
                    b->pixels[scr_y*b->stride + scr_x] = gfx_blend(b->pixels[scr_y*b->stride + scr_x], ca);
                }
            }
        }
        pen += g->advance * sc;
    }
}

/* HUD strings are consumed immediately; no per-frame copies or queue. */
void sw_text_scaled(const char *s,float x,float y,uint32_t color,float scale) {
    if (!s || !sw_gfx_current_buffer() || !gfx_font_load(gfx_assets())) return;
    gfx_render_text(sw_gfx_current_buffer(),s,x,y,gfx_pack(color),scale);
}

int sw_text_width(const char *s) { return gfx_metrics_width(s); }

/* Recoverable engine error, not an interpreter console. */
void sw_gfx_error_screen(const char *message) {
    Buffer *b=sw_gfx_current_buffer();
    if (!b) return;
    gfx_clear(b,gfx_pack(0xFF201A1Au));
    if (!gfx_font()) return;
    gfx_render_text(b,"Enjoer — error",16,16,gfx_pack(0xFFFFFFFFu),.7f);
    if (message) gfx_render_text(b,message,16,56,gfx_pack(0xFFFFB0B0u),.45f);
}
