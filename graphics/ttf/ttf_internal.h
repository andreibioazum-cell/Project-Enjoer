/* graphics/ttf/ttf_internal.h — общий интерфейс минимального TTF-движка.
 * Парсер контуров живёт в ttf_outline.c, запекание атласа — в ttf_font.c. */
#ifndef TTF_INTERNAL_H
#define TTF_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

typedef struct DSFont DSFont;

typedef struct {
    uint32_t codepoint;
    float advance;
    float bearing_x;
    float bearing_top;
    int width;
    int height;
    float u0, v0, u1, v1;
} DSFontGlyph;

/* ttf_font.c */
DSFont *ds_font_create(const uint8_t *data, size_t size, int pixel_height);
void ds_font_destroy(DSFont *font);
const DSFontGlyph *ds_font_glyph(const DSFont *font, uint32_t cp);
int ds_font_aw(const DSFont *font);
int ds_font_ah(const DSFont *font);
const uint8_t *ds_font_alpha(const DSFont *font);
float ds_font_lineh(const DSFont *font);
float ds_font_ascent(const DSFont *font);

/* ── внутренности для ttf_outline.c / ttf_font.c ─────────────────────── */
#define DS_FONT_ATLAS_W 1024
#define DS_FONT_ATLAS_H 2048
#define DS_FONT_SS 4
#define DS_FONT_MAX 640

#define TTF_ARG_WORDS    0x0001
#define TTF_ARG_XY       0x0002
#define TTF_HAVE_SCALE   0x0008
#define TTF_MORE         0x0020
#define TTF_HAVE_XY_SCALE 0x0040
#define TTF_HAVE_2X2     0x0080

#define TTF_TAG(a,b,c,d) ((uint32_t)(a)<<24|(uint32_t)(b)<<16|(uint32_t)(c)<<8|(uint32_t)(d))

typedef struct { float x, y; int on_curve; } DSPoint;

typedef struct {
    DSPoint *points; int pc, pp;
    int *ends;     int cc, cp;
} DSOutline;

typedef struct { float x, y; } DSFPoint;
typedef struct { DSFPoint *points; int count, cap; } DSFC;

struct DSFont {
    uint8_t *data; size_t size;
    int upem, loc_format, ng, nhm, asc_u, desc_u, lg_u;
    uint32_t head, hhea, hmtx, maxp, loca, glyf, cmap, cmap4, cmap12;
    float scale, ascent, line_h;
    int aw, ah;
    uint8_t *alpha;
    DSFontGlyph *glyphs; int gcount;
};

/* чтение бинарных таблиц (безопасные, с проверкой границ) */
int ttf_in_range(const DSFont *f, size_t off, size_t len);
uint16_t ttf_u16(const DSFont *f, size_t off);
int16_t ttf_s16(const DSFont *f, size_t off);
uint32_t ttf_u32(const DSFont *f, size_t off);
int ttf_table_bound(const DSFont *f, uint32_t tag, uint32_t *off, uint32_t *len);
int ttf_glyph_offsets(const DSFont *f, int glyph, uint32_t *start, uint32_t *end);
int ttf_glyph_metrics(const DSFont *f, int glyph, int *advance, int *lsb);
int ttf_glyph_for_cp(const DSFont *f, uint32_t cp);

/* контуры */
void ttf_outline_init(DSOutline *o);
void ttf_outline_free(DSOutline *o);
int ttf_outline_reserve_points(DSOutline *o, int n);
int ttf_outline_reserve_contours(DSOutline *o, int n);
int ttf_outline_add(DSOutline *o, const DSPoint *p, int n);
int ttf_read_outline(const DSFont *f, int glyph, int depth,
                     float a, float b, float c, float d,
                     float tx, float ty, DSOutline *dst);
int ttf_flatten(const DSPoint *p, int n, DSFC *flat);

#endif
