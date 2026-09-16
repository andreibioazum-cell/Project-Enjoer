/*
 * TrueType text pass: parse once at font.load, rasterize glyphs on demand into
 * a per-font RGBA atlas, draw texts as tinted glyph quads.  See ds_ttf.h for
 * the contract.  Every table read is bounds-checked: font bytes come from game
 * assets and a truncated file must log an error, never crash the game.
 */
#include "ds_ttf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds_font.h"
#include "ds_image.h"
#include "engine.h"
#include "enjoer_draw.h"

/* Glyph raster size, supersampling factor and the atlas geometry.
 *
 * A glyph is baked at the em it is drawn with, so a 16 px label gets a 16 px
 * bitmap (sampled 1:1, which is what makes small text readable) instead of a
 * 48 px one that the sampler shrinks into mush.  DS_TTF_PX is the size used
 * when the wanted one is off the ladder or the atlas has no room left: 48 px
 * keeps body text crisp up to scale 3.0 exactly like the old fixed size did. */
#define DS_TTF_PX 48
#define DS_TTF_PX_MIN 12  /* below this the outlines are not worth rasterizing */
#define DS_TTF_PX_MAX 96  /* above this the atlas would fill with a handful of glyphs */
#define DS_TTF_SS 3
#define DS_TTF_ATLAS 512
#define DS_TTF_ATLAS_PAD 2
/* One slot per (codepoint, raster size): a HUD with three text sizes needs
 * three slots per glyph, so the cache is sized for a whole alphabet a few
 * times over.  The lookup is a linear scan, which stays cheap at this size. */
#define DS_TTF_CACHE 768
#define DS_TTF_MAX_POINTS 4096
#define DS_TTF_MAX_DEPTH 4
/* A scanline never needs more crossings than this; beyond it the row keeps the
 * crossings it has (a degraded but bounded glyph beats an overflow). */
#define DS_TTF_MAX_ROWS 768
#define DS_TTF_MAX_CROSS 256

/* ------------------------------------------------------------------ readers */

typedef struct {
    const uint8_t *bytes;
    size_t size;
} DsTtfFont;

static int ttf_u16(const DsTtfFont *font, size_t offset, uint16_t *out) {
    if (!font || offset + 2 > font->size) return 0;
    *out = (uint16_t)(((uint16_t)font->bytes[offset] << 8) | font->bytes[offset + 1]);
    return 1;
}

static int ttf_i16(const DsTtfFont *font, size_t offset, int16_t *out) {
    uint16_t raw = 0;
    if (!ttf_u16(font, offset, &raw)) return 0;
    *out = (int16_t)raw;
    return 1;
}

static int ttf_u32(const DsTtfFont *font, size_t offset, uint32_t *out) {
    if (!font || offset + 4 > font->size) return 0;
    *out = ((uint32_t)font->bytes[offset] << 24) | ((uint32_t)font->bytes[offset + 1] << 16) |
           ((uint32_t)font->bytes[offset + 2] << 8) | font->bytes[offset + 3];
    return 1;
}

static int ttf_table(const DsTtfFont *font, const char tag[4], size_t *offset, size_t *length) {
    uint16_t tables = 0;
    size_t index = 0;
    if (!font || font->size < 12) return 0;
    if (!ttf_u16(font, 4, &tables)) return 0;
    for (index = 0; index < tables; ++index) {
        size_t entry = 12 + index * 16;
        uint32_t table_offset = 0;
        uint32_t table_length = 0;
        if (entry + 16 > font->size) return 0;
        if (memcmp(font->bytes + entry, tag, 4) != 0) continue;
        if (!ttf_u32(font, entry + 8, &table_offset) || !ttf_u32(font, entry + 12, &table_length))
            return 0;
        if ((size_t)table_offset + (size_t)table_length > font->size) return 0;
        *offset = (size_t)table_offset;
        *length = (size_t)table_length;
        return 1;
    }
    return 0;
}

/* -------------------------------------------------------------------- faces */

typedef struct {
    uint32_t codepoint;
    int ready;
    /* Em box (in raster px) this glyph was baked for: every raster-px metric
     * below is divided by it, so a glyph that fell back to the base size
     * still lands in the same place as its neighbours. */
    int raster;
    /* Atlas cell of the ink bitmap (raster pixels, row 0 is the glyph top). */
    int32_t ax;
    int32_t ay;
    int32_t w;
    int32_t h;
    /* Pen position to bitmap left, and baseline to bitmap top, in raster px. */
    float left;
    float top;
    /* Pen advance in raster px (fractional: advances accumulate unrounded). */
    float advance;
} DsTtfGlyph;

typedef struct {
    int ready;
    int32_t layer;
    DsTtfFont font;
    uint16_t units_per_em;
    uint16_t num_glyphs;
    uint16_t num_h_metrics;
    int16_t ascent;
    int16_t descent;
    int16_t line_gap;
    int loca_long;
    size_t cmap; /* chosen subtable offset; 0 means "no usable cmap" */
    int cmap_format;
    size_t glyf;
    size_t glyf_length;
    size_t hmtx;
    size_t hmtx_length;
    size_t loca;
    uint8_t *atlas;
    int atlas_x;
    int atlas_y;
    int atlas_row_h;
    int atlas_full;
    DsTtfGlyph notdef;
    DsTtfGlyph cache[DS_TTF_CACHE];
    int cache_count;
} DsTtfFace;

static DsTtfFace faces[DS_MAX_FONTS];

static DsTtfFace *ttf_face(int32_t font) {
    if (font < 0 || font >= DS_MAX_FONTS) return NULL;
    return &faces[font];
}

/* ------------------------------------------------------------- cmap lookup */

static int ttf_cmap_format4(const DsTtfFace *face, uint32_t codepoint, uint16_t *glyph) {
    /* Format 4 covers the Basic Multilingual Plane; astral codepoints never
     * match here and fall through to .notdef unless format 12 was chosen. */
    const DsTtfFont *font = &face->font;
    uint16_t seg_count = 0;
    uint16_t index = 0;
    uint16_t seg_x2 = 0;
    size_t end_base = 0;
    size_t start_base = 0;
    size_t delta_base = 0;
    size_t range_base = 0;
    if (codepoint > 0xFFFFu) return 0;
    if (!ttf_u16(font, face->cmap + 6, &seg_x2)) return 0;
    seg_count = (uint16_t)(seg_x2 / 2);
    if (seg_count == 0) return 0;
    end_base = face->cmap + 14;
    start_base = end_base + (size_t)seg_count * 2 + 2;
    delta_base = start_base + (size_t)seg_count * 2;
    range_base = delta_base + (size_t)seg_count * 2;
    for (index = 0; index < seg_count; ++index) {
        uint16_t end = 0;
        uint16_t start = 0;
        if (!ttf_u16(font, end_base + (size_t)index * 2, &end)) return 0;
        if ((uint32_t)end < codepoint) continue;
        if (!ttf_u16(font, start_base + (size_t)index * 2, &start)) return 0;
        if ((uint32_t)start > codepoint) return 0;
        {
            int16_t delta = 0;
            uint16_t range_offset = 0;
            if (!ttf_i16(font, delta_base + (size_t)index * 2, &delta)) return 0;
            if (!ttf_u16(font, range_base + (size_t)index * 2, &range_offset)) return 0;
            if (range_offset == 0) {
                *glyph = (uint16_t)((codepoint + (uint32_t)(int32_t)delta) & 0xFFFFu);
                return 1;
            }
            {
                size_t entry = range_base + (size_t)index * 2 + (size_t)range_offset +
                               (codepoint - (uint32_t)start) * 2;
                uint16_t mapped = 0;
                if (!ttf_u16(font, entry, &mapped)) return 0;
                if (mapped == 0) return 0;
                *glyph = (uint16_t)((mapped + (uint32_t)(int32_t)delta) & 0xFFFFu);
                return 1;
            }
        }
    }
    return 0;
}

static int ttf_cmap_format12(const DsTtfFace *face, uint32_t codepoint, uint16_t *glyph) {
    const DsTtfFont *font = &face->font;
    uint32_t groups = 0;
    uint32_t index = 0;
    if (!ttf_u32(font, face->cmap + 12, &groups)) return 0;
    for (index = 0; index < groups; ++index) {
        size_t entry = face->cmap + 16 + (size_t)index * 12;
        uint32_t start = 0;
        uint32_t end = 0;
        uint32_t first = 0;
        if (!ttf_u32(font, entry, &start) || !ttf_u32(font, entry + 4, &end) ||
            !ttf_u32(font, entry + 8, &first))
            return 0;
        if (codepoint < start || codepoint > end) continue;
        if (first + (codepoint - start) > 0xFFFFu) return 0;
        *glyph = (uint16_t)(first + (codepoint - start));
        return 1;
    }
    return 0;
}

static uint16_t ttf_glyph_id(DsTtfFace *face, uint32_t codepoint) {
    uint16_t glyph = 0;
    if (!face->cmap) return 0;
    if (face->cmap_format == 12) {
        if (ttf_cmap_format12(face, codepoint, &glyph) && glyph < face->num_glyphs) return glyph;
        return 0;
    }
    if (ttf_cmap_format4(face, codepoint, &glyph) && glyph < face->num_glyphs) return glyph;
    return 0;
}

static int ttf_pick_cmap(DsTtfFace *face, size_t cmap, size_t length) {
    /* Prefer a Unicode BMP subtable, accept an astral (format 12) one, ignore
     * everything else (Mac Roman, symbol, ...). */
    const DsTtfFont *font = &face->font;
    uint16_t records = 0;
    uint16_t index = 0;
    size_t backup = 0;
    if (length < 4) return 0;
    if (!ttf_u16(font, cmap + 2, &records)) return 0;
    for (index = 0; index < records; ++index) {
        size_t entry = cmap + 4 + (size_t)index * 8;
        uint16_t platform = 0;
        uint16_t encoding = 0;
        uint32_t subtable = 0;
        uint16_t format = 0;
        if (entry + 8 > font->size) return 0;
        if (!ttf_u16(font, entry, &platform) || !ttf_u16(font, entry + 2, &encoding) ||
            !ttf_u32(font, entry + 4, &subtable))
            return 0;
        if (cmap + (size_t)subtable + 8 > font->size) continue;
        if (!ttf_u16(font, cmap + (size_t)subtable, &format)) continue;
        if (format != 4 && format != 12) continue;
        if ((platform == 3 && encoding == 1 && format == 4) ||
            (platform == 0 && encoding == 3 && format == 4)) {
            face->cmap = cmap + (size_t)subtable;
            face->cmap_format = 4;
            return 1;
        }
        if ((platform == 3 && encoding == 10) || (platform == 0 && encoding == 4)) {
            if (format == 4) {
                face->cmap = cmap + (size_t)subtable;
                face->cmap_format = 4;
                return 1;
            }
            backup = cmap + (size_t)subtable;
        }
    }
    if (backup) {
        face->cmap = backup;
        face->cmap_format = 12;
        return 1;
    }
    return 0;
}

/* ---------------------------------------------------------- outline points */

typedef struct {
    float x;
    float y;
} DsTtfPoint;

typedef struct {
    DsTtfPoint points[DS_TTF_MAX_POINTS];
    int count;
    /* Start index of every contour: edges close within a contour, never
     * across two of them. */
    int starts[DS_TTF_MAX_POINTS];
    int contours;
} DsTtfOutline;

static void ttf_begin_contour(DsTtfOutline *outline) {
    if (outline->contours < DS_TTF_MAX_POINTS) {
        outline->starts[outline->contours] = outline->count;
        ++outline->contours;
    }
}

static void ttf_push(DsTtfOutline *outline, float x, float y) {
    if (outline->count < DS_TTF_MAX_POINTS) {
        outline->points[outline->count].x = x;
        outline->points[outline->count].y = y;
        ++outline->count;
    }
}

/* Flatten one quadratic Bezier; tolerance is in the caller's units. */
static void ttf_flatten(DsTtfOutline *outline, float x0, float y0, float x1, float y1, float x2,
                        float y2, float tolerance, int depth) {
    float mx = (x0 + 2.0f * x1 + x2) * 0.25f;
    float my = (y0 + 2.0f * y1 + y2) * 0.25f;
    float dx = (x0 + x2) * 0.5f - mx;
    float dy = (y0 + y2) * 0.5f - my;
    if (depth <= 0 || dx * dx + dy * dy <= tolerance * tolerance) {
        ttf_push(outline, x2, y2);
        return;
    }
    {
        float ax = (x0 + x1) * 0.5f;
        float ay = (y0 + y1) * 0.5f;
        float bx = (x1 + x2) * 0.5f;
        float by = (y1 + y2) * 0.5f;
        ttf_flatten(outline, x0, y0, ax, ay, mx, my, tolerance, depth - 1);
        ttf_flatten(outline, mx, my, bx, by, x2, y2, tolerance, depth - 1);
    }
}

/* A contour becomes a closed loop of straight segments in `outline`.  Points
 * arrive in font units with the caller's transform applied; contours touch
 * only through shared endpoints, so every consecutive pair (including the
 * wrap-around) is one edge. */
static void ttf_emit_contour(DsTtfOutline *outline, const DsTtfPoint *points, const uint8_t *on_curve,
                             int count, float m00, float m01, float m10, float m11, float dx,
                             float dy, float tolerance) {
    /* The loop starts at an on-curve point (the first one, the wrapped last
     * one, or the implied midpoint between two off-curve ends); every node
     * after it is a line when on-curve and a quadratic through it when off,
     * ending at the next on-curve node or the implied midpoint.  Consecutive
     * off-curve nodes chain through their midpoints, exactly per spec. */
    float prev_x = 0.0f;
    float prev_y = 0.0f;
    int first = 0;
    int nodes = 0;
    int step = 0;
    int base = 0;
    int index = 0;
    if (count <= 0) return;
    ttf_begin_contour(outline);
    if (on_curve[0]) {
        prev_x = points[0].x;
        prev_y = points[0].y;
        first = 1;
        nodes = count - 1;
    } else if (on_curve[count - 1]) {
        prev_x = points[count - 1].x;
        prev_y = points[count - 1].y;
        first = 0;
        nodes = count - 1;
    } else {
        prev_x = (points[count - 1].x + points[0].x) * 0.5f;
        prev_y = (points[count - 1].y + points[0].y) * 0.5f;
        first = 0;
        nodes = count;
    }
    base = outline->count;
    ttf_push(outline, prev_x, prev_y);
    for (step = 0; step < nodes; ++step) {
        int at = (first + step) % count;
        int next = (at + 1) % count;
        if (on_curve[at]) {
            ttf_push(outline, points[at].x, points[at].y);
            prev_x = points[at].x;
            prev_y = points[at].y;
        } else {
            float end_x = 0.0f;
            float end_y = 0.0f;
            if (on_curve[next]) {
                end_x = points[next].x;
                end_y = points[next].y;
            } else {
                end_x = (points[at].x + points[next].x) * 0.5f;
                end_y = (points[at].y + points[next].y) * 0.5f;
            }
            ttf_flatten(outline, prev_x, prev_y, points[at].x, points[at].y, end_x, end_y,
                        tolerance, 8);
            prev_x = end_x;
            prev_y = end_y;
        }
    }
    /* Apply the component transform to the points this contour just added. */
    for (index = base; index < outline->count; ++index) {
        float x = outline->points[index].x;
        float y = outline->points[index].y;
        outline->points[index].x = m00 * x + m01 * y + dx;
        outline->points[index].y = m10 * x + m11 * y + dy;
    }
}

/* ------------------------------------------------------- glyph extraction */

static int ttf_glyph_range(const DsTtfFace *face, uint16_t glyph, size_t *start, size_t *end) {
    const DsTtfFont *font = &face->font;
    uint32_t first = 0;
    uint32_t last = 0;
    if ((uint32_t)glyph + 1 >= face->num_glyphs + 1u) return 0;
    if (face->loca_long) {
        if (!ttf_u32(font, face->loca + (size_t)glyph * 4, &first) ||
            !ttf_u32(font, face->loca + ((size_t)glyph + 1) * 4, &last))
            return 0;
    } else {
        uint16_t first_short = 0;
        uint16_t last_short = 0;
        if (!ttf_u16(font, face->loca + (size_t)glyph * 2, &first_short) ||
            !ttf_u16(font, face->loca + ((size_t)glyph + 1) * 2, &last_short))
            return 0;
        first = (uint32_t)first_short * 2;
        last = (uint32_t)last_short * 2;
    }
    if (first > last || face->glyf + (size_t)last > face->font.size) return 0;
    *start = face->glyf + (size_t)first;
    *end = face->glyf + (size_t)last;
    return 1;
}

static int ttf_advance(const DsTtfFace *face, uint16_t glyph, float raster_scale, float *advance) {
    /* hmtx holds num_h_metrics (advance, lsb) pairs; higher glyphs reuse the
     * last advance. */
    const DsTtfFont *font = &face->font;
    uint32_t slot = glyph < face->num_h_metrics ? glyph : (uint32_t)(face->num_h_metrics - 1);
    size_t offset = face->hmtx + (size_t)slot * 4;
    uint16_t width = 0;
    if (face->num_h_metrics == 0 || offset + 2 > font->size) return 0;
    if (!ttf_u16(font, offset, &width)) return 0;
    *advance = (float)width * raster_scale;
    return 1;
}

/* Simple glyph: flags (with repeats), packed x/y deltas, contour ends. */
static int ttf_simple_outline(const DsTtfFace *face, size_t start, size_t end, int contours,
                              DsTtfOutline *outline, float m00, float m01, float m10, float m11,
                              float dx, float dy, float tolerance) {
    const DsTtfFont *font = &face->font;
    size_t cursor = 0;
    int contour = 0;
    int points = 0;
    int index = 0;
    uint16_t last_end = 0;
    size_t flags_at = 0;
    size_t x_at = 0;
    size_t y_at = 0;
    DsTtfPoint *held = NULL;
    uint8_t *held_on = NULL;
    int16_t *ends = NULL;
    uint8_t *flags = NULL;
    int ok = 0;
    if (contours <= 0) return 1;
    if (start + 10 + (size_t)contours * 2 > end) return 0;
    if (!ttf_u16(font, start + 10 + ((size_t)contours - 1) * 2, &last_end)) return 0;
    points = (int)last_end + 1;
    if (points <= 0 || points > 32767) return 0;
    ends = (int16_t *)malloc(sizeof(int16_t) * (size_t)contours);
    flags = (uint8_t *)malloc((size_t)points);
    held = (DsTtfPoint *)malloc(sizeof(DsTtfPoint) * (size_t)points);
    held_on = (uint8_t *)malloc((size_t)points);
    if (!ends || !flags || !held || !held_on) goto done;
    for (contour = 0; contour < contours; ++contour) {
        uint16_t value = 0;
        if (!ttf_u16(font, start + 10 + (size_t)contour * 2, &value)) goto done;
        ends[contour] = (int16_t)value;
    }
    cursor = start + 10 + (size_t)contours * 2;
    {
        uint16_t instructions = 0;
        if (!ttf_u16(font, cursor, &instructions)) goto done;
        cursor += 2 + (size_t)instructions;
        if (cursor > end) goto done;
    }
    flags_at = cursor;
    for (index = 0; index < points; ++index) {
        uint8_t flag = 0;
        if (flags_at >= end) goto done;
        flag = font->bytes[flags_at++];
        flags[index] = flag;
        if (flag & 8) {
            uint8_t repeat = 0;
            int again = 0;
            if (flags_at >= end) goto done;
            repeat = font->bytes[flags_at++];
            for (again = 0; again < repeat; ++again) {
                if (++index >= points) goto done;
                flags[index] = flag;
            }
        }
    }
    x_at = flags_at;
    y_at = 0;
    {
        int16_t x = 0;
        int16_t y = 0;
        for (index = 0; index < points; ++index) {
            uint8_t flag = flags[index];
            if (flag & 2) {
                uint8_t delta = 0;
                if (x_at >= end) goto done;
                delta = font->bytes[x_at++];
                x = (int16_t)(x + ((flag & 16) ? (int16_t)delta : (int16_t)-(int16_t)delta));
            } else if (!(flag & 16)) {
                int16_t delta = 0;
                if (!ttf_i16(font, x_at, &delta)) goto done;
                x_at += 2;
                x = (int16_t)(x + delta);
            }
            held[index].x = (float)x;
            held[index].y = (float)y; /* y filled below; x pass must not touch it */
            held_on[index] = (flag & 1) ? 1 : 0;
        }
        y_at = x_at;
        for (index = 0; index < points; ++index) {
            uint8_t flag = flags[index];
            if (flag & 4) {
                uint8_t delta = 0;
                if (y_at >= end) goto done;
                delta = font->bytes[y_at++];
                y = (int16_t)(y + ((flag & 32) ? (int16_t)delta : (int16_t)-(int16_t)delta));
            } else if (!(flag & 32)) {
                int16_t delta = 0;
                if (!ttf_i16(font, y_at, &delta)) goto done;
                y_at += 2;
                y = (int16_t)(y + delta);
            }
            held[index].y = (float)y;
        }
    }
    {
        int from = 0;
        for (contour = 0; contour < contours; ++contour) {
            int to = (int)ends[contour];
            if (to < from || to >= points) goto done;
            ttf_emit_contour(outline, held + from, held_on + from, to - from + 1, m00, m01, m10,
                             m11, dx, dy, tolerance);
            from = to + 1;
        }
    }
    ok = 1;
done:
    free(ends);
    free(flags);
    free(held);
    free(held_on);
    return ok;
}

/* Compound glyph: place transformed components; recurses for nested ones. */
static int ttf_outline(const DsTtfFace *face, uint16_t glyph, DsTtfOutline *outline, float m00,
                       float m01, float m10, float m11, float dx, float dy, float tolerance,
                       int depth);

static int ttf_compound_outline(const DsTtfFace *face, size_t start, size_t end,
                                DsTtfOutline *outline, float m00, float m01, float m10, float m11,
                                float dx, float dy, float tolerance, int depth) {
    const DsTtfFont *font = &face->font;
    size_t cursor = start + 10;
    /* Components keep appending contours to the same outline; anchor-matched
     * ones align a component point with an already placed point. */
    for (;;) {
        uint16_t flags = 0;
        uint16_t component = 0;
        int16_t arg0 = 0;
        int16_t arg1 = 0;
        float local00 = 1.0f;
        float local01 = 0.0f;
        float local10 = 0.0f;
        float local11 = 1.0f;
        float local_dx = 0.0f;
        float local_dy = 0.0f;
        int base = 0;
        if (cursor + 4 > end) return 0;
        if (!ttf_u16(font, cursor, &flags) || !ttf_u16(font, cursor + 2, &component)) return 0;
        cursor += 4;
        if (flags & 1) {
            if (!ttf_i16(font, cursor, &arg0) || !ttf_i16(font, cursor + 2, &arg1)) return 0;
            cursor += 4;
        } else {
            if (cursor + 2 > end) return 0;
            arg0 = (int16_t)(int8_t)font->bytes[cursor];
            arg1 = (int16_t)(int8_t)font->bytes[cursor + 1];
            cursor += 2;
        }
        if (flags & 8) {
            int16_t scale = 0;
            if (!ttf_i16(font, cursor, &scale)) return 0;
            cursor += 2;
            local00 = local11 = (float)scale / 16384.0f;
        } else if (flags & 64) {
            int16_t sx = 0;
            int16_t sy = 0;
            if (!ttf_i16(font, cursor, &sx) || !ttf_i16(font, cursor + 2, &sy)) return 0;
            cursor += 4;
            local00 = (float)sx / 16384.0f;
            local11 = (float)sy / 16384.0f;
        } else if (flags & 128) {
            int16_t a = 0;
            int16_t b = 0;
            int16_t c = 0;
            int16_t d = 0;
            if (!ttf_i16(font, cursor, &a) || !ttf_i16(font, cursor + 2, &b) ||
                !ttf_i16(font, cursor + 4, &c) || !ttf_i16(font, cursor + 6, &d))
                return 0;
            cursor += 8;
            local00 = (float)a / 16384.0f;
            local01 = (float)b / 16384.0f;
            local10 = (float)c / 16384.0f;
            local11 = (float)d / 16384.0f;
        }
        /* The component transform composes with the caller's: first the local
         * scale/offset, then the outer matrix. */
        base = outline->count;
        if (flags & 2) {
            local_dx = (float)arg0;
            local_dy = (float)arg1;
            if (!ttf_outline(face, component, outline, m00 * local00 + m01 * local10,
                             m00 * local01 + m01 * local11, m10 * local00 + m11 * local10,
                             m10 * local01 + m11 * local11, m00 * local_dx + m01 * local_dy + dx,
                             m10 * local_dx + m11 * local_dy + dy, tolerance, depth + 1))
                return 0;
        } else {
            /* Anchor matching: shift the component so its point arg1 lands on
             * the already placed point arg0.  Out-of-range anchors fall back
             * to a zero offset (a misplaced accent beats a crash). */
            DsTtfPoint anchor = {0.0f, 0.0f};
            DsTtfPoint match = {0.0f, 0.0f};
            int placed = 0;
            if (arg0 >= 0 && arg0 < base) {
                anchor = outline->points[arg0];
                placed = 1;
            }
            if (!ttf_outline(face, component, outline, m00 * local00 + m01 * local10,
                             m00 * local01 + m01 * local11, m10 * local00 + m11 * local10,
                             m10 * local01 + m11 * local11, dx, dy, tolerance, depth + 1))
                return 0;
            if (placed && arg1 >= 0 && base + arg1 < outline->count) {
                int index = 0;
                match = outline->points[base + arg1];
                for (index = base; index < outline->count; ++index) {
                    outline->points[index].x += anchor.x - match.x;
                    outline->points[index].y += anchor.y - match.y;
                }
            }
        }
        if (!(flags & 32)) break;
    }
    return 1;
}

static int ttf_outline(const DsTtfFace *face, uint16_t glyph, DsTtfOutline *outline, float m00,
                       float m01, float m10, float m11, float dx, float dy, float tolerance,
                       int depth) {
    size_t start = 0;
    size_t end = 0;
    int16_t contours = 0;
    if (depth > DS_TTF_MAX_DEPTH || glyph >= face->num_glyphs) return 0;
    if (!ttf_glyph_range(face, glyph, &start, &end)) return 0;
    if (start == end) return 1; /* whitespace: no outline, advance only */
    if (end - start < 10) return 0;
    if (!ttf_i16(&face->font, start, &contours)) return 0;
    if (contours >= 0) {
        return ttf_simple_outline(face, start, end, contours, outline, m00, m01, m10, m11, dx, dy,
                                  tolerance);
    }
    return ttf_compound_outline(face, start, end, outline, m00, m01, m10, m11, dx, dy, tolerance,
                                depth);
}

/* ------------------------------------------------------------- rasterizer */

typedef struct {
    float x0;
    float y0;
    float x1;
    float y1;
} DsTtfEdge;

/* Nonzero-winding scanline fill of one contour soup into an SS-times bitmap.
 * Contours arrive in font units (y up); the bitmap is raster pixels (y down)
 * translated by (shift_x, shift_y). */
static void ttf_fill(const DsTtfOutline *outline, float scale, float shift_x, float shift_y,
                     uint8_t *cover, int width, int height) {
    int contour = 0;
    int row = 0;
    float crossings[DS_TTF_MAX_CROSS * 2];
    for (row = 0; row < height; ++row) {
        float scan = (float)row + 0.5f;
        int found = 0;
        int index = 0;
        float winding = 0.0f;
        float span_start = 0.0f;
        int span_open = 0;
        int pixel = 0;
        for (contour = 0; contour < outline->contours; ++contour) {
            int from = outline->starts[contour];
            int to = contour + 1 < outline->contours ? outline->starts[contour + 1] : outline->count;
            int edge = 0;
            if (to - from < 2) continue;
            for (edge = from; edge < to; ++edge) {
                const DsTtfPoint *a = &outline->points[edge];
                const DsTtfPoint *b = &outline->points[edge + 1 < to ? edge + 1 : from];
                /* Font units run y-up, bitmap rows run y-down: y is negated
                 * (the winding direction flips with it, zero stays zero). */
                float ax = a->x * scale + shift_x;
                float ay = -(a->y) * scale + shift_y;
                float bx = b->x * scale + shift_x;
                float by = -(b->y) * scale + shift_y;
                float top = ay < by ? ay : by;
                float bottom = ay < by ? by : ay;
                float x = 0.0f;
                /* Horizontal edges never cross a scanline; half-open [top,
                 * bottom) keeps shared vertices counted exactly once. */
                if (!(scan >= top && scan < bottom)) continue;
                x = ax + (scan - ay) * (bx - ax) / (by - ay);
                if (found + 2 > DS_TTF_MAX_CROSS * 2) continue;
                crossings[found++] = x;
                crossings[found++] = by > ay ? 1.0f : -1.0f;
            }
        }
        /* Insertion sort keeps crossings ordered; rows hold a handful, and
         * qsort's callback churn would cost more than it saves here. */
        for (index = 2; index < found; index += 2) {
            float x = crossings[index];
            float dir = crossings[index + 1];
            int hole = index - 2;
            while (hole >= 0 && crossings[hole] > x) {
                crossings[hole + 2] = crossings[hole];
                crossings[hole + 3] = crossings[hole + 1];
                hole -= 2;
            }
            crossings[hole + 2] = x;
            crossings[hole + 3] = dir;
        }
        for (index = 0; index < found; index += 2) {
            if (!span_open && winding == 0.0f) {
                span_start = crossings[index];
                span_open = 1;
            }
            winding += crossings[index + 1];
            if (span_open && winding == 0.0f) {
                int x0 = (int)span_start;
                int x1 = (int)crossings[index];
                if (x0 < 0) x0 = 0;
                if (x1 > width) x1 = width;
                for (pixel = x0; pixel < x1; ++pixel) cover[(size_t)row * (size_t)width + pixel] = 1;
                span_open = 0;
            }
        }
    }
}

/* ------------------------------------------------------------ atlas/cache */

static DsTtfGlyph *ttf_cached(DsTtfFace *face, uint32_t codepoint, int raster) {
    int index = 0;
    for (index = 0; index < face->cache_count; ++index)
        if (face->cache[index].ready && face->cache[index].codepoint == codepoint &&
            face->cache[index].raster == raster)
            return &face->cache[index];
    return NULL;
}

/* Em box -> raster size to bake at.  Whole pixels inside the ladder, so the
 * sampler maps one texel to one screen pixel; outside it the nearest allowed
 * size, because an oversized atlas costs more than the softness it saves. */
static int ttf_pick_raster(float em) {
    int px = 0;
    if (!(em > 0.0f)) return DS_TTF_PX;
    px = (int)((float)floor((double)em + 0.5));
    if (px < DS_TTF_PX_MIN) px = DS_TTF_PX_MIN;
    if (px > DS_TTF_PX_MAX) px = DS_TTF_PX_MAX;
    return px;
}

static int ttf_atlas_place(DsTtfFace *face, int w, int h, int32_t *ax, int32_t *ay) {
    if (w <= 0 || h <= 0 || w + DS_TTF_ATLAS_PAD * 2 > DS_TTF_ATLAS ||
        h + DS_TTF_ATLAS_PAD * 2 > DS_TTF_ATLAS)
        return 0;
    if (face->atlas_x + w + DS_TTF_ATLAS_PAD > DS_TTF_ATLAS) {
        face->atlas_x = DS_TTF_ATLAS_PAD;
        face->atlas_y += face->atlas_row_h + DS_TTF_ATLAS_PAD;
        face->atlas_row_h = 0;
    }
    if (face->atlas_y + h + DS_TTF_ATLAS_PAD > DS_TTF_ATLAS) return 0;
    *ax = face->atlas_x;
    *ay = face->atlas_y;
    face->atlas_x += w + DS_TTF_ATLAS_PAD;
    if (h > face->atlas_row_h) face->atlas_row_h = h;
    return 1;
}

/* ttf_bake outcomes.  FULL is the only one that is worth a second try: it says
 * "no room at this size", not "this font is broken". */
#define DS_TTF_BAKE_OK 0
#define DS_TTF_BAKE_BROKEN 1 /* the metrics themselves are unreadable */
#define DS_TTF_BAKE_FULL 2   /* the atlas is full at this raster size */

/* Rasterize one glyph id at `raster` px em into the face atlas and fill `slot`.
 * Whitespace yields an advance-only entry, a broken outline an empty box: both
 * are OK outcomes, because the advance still lays the line out correctly. */
static int ttf_bake(DsTtfFace *face, uint32_t codepoint, uint16_t glyph, int raster,
                    DsTtfGlyph *slot) {
    float raster_scale = (float)raster / (float)face->units_per_em;
    DsTtfOutline *outline = NULL;
    size_t start = 0;
    size_t end = 0;
    int16_t x_min = 0;
    int16_t y_min = 0;
    int16_t x_max = 0;
    int16_t y_max = 0;
    float advance = 0.0f;
    int bitmap_x0 = 0;
    int bitmap_y0 = 0;
    int bitmap_w = 0;
    int bitmap_h = 0;
    int32_t ax = 0;
    int32_t ay = 0;
    uint8_t *cover = NULL;
    int x = 0;
    int y = 0;
    int status = DS_TTF_BAKE_OK;
    if (!ttf_advance(face, glyph, raster_scale, &advance)) return DS_TTF_BAKE_BROKEN;
    memset(slot, 0, sizeof(*slot));
    slot->codepoint = codepoint;
    slot->raster = raster;
    slot->advance = advance;
    slot->ready = 1;
    if (!ttf_glyph_range(face, glyph, &start, &end)) return DS_TTF_BAKE_OK;
    if (start == end) return DS_TTF_BAKE_OK; /* whitespace */
    if (end - start < 10) return DS_TTF_BAKE_OK;
    if (!ttf_i16(&face->font, start + 2, &x_min) || !ttf_i16(&face->font, start + 4, &y_min) ||
        !ttf_i16(&face->font, start + 6, &x_max) || !ttf_i16(&face->font, start + 8, &y_max))
        return DS_TTF_BAKE_OK;
    if (x_max <= x_min || y_max <= y_min) return DS_TTF_BAKE_OK;
    bitmap_x0 = (int)(x_min < 0 ? (float)x_min * raster_scale - 1.0f : (float)x_min * raster_scale);
    bitmap_y0 = (int)(y_max > 0 ? -(float)y_max * raster_scale - 1.0f : -(float)y_max * raster_scale);
    {
        int bitmap_x1 = (int)((float)x_max * raster_scale + 1.0f);
        int bitmap_y1 = (int)(-(float)y_min * raster_scale + 1.0f);
        bitmap_w = bitmap_x1 - bitmap_x0;
        bitmap_h = bitmap_y1 - bitmap_y0;
    }
    if (bitmap_w <= 0 || bitmap_h <= 0) return DS_TTF_BAKE_OK;
    if (bitmap_w > DS_TTF_ATLAS / 2 || bitmap_h > DS_TTF_ATLAS / 2) return DS_TTF_BAKE_BROKEN;
    outline = (DsTtfOutline *)calloc(1, sizeof(DsTtfOutline));
    cover = (uint8_t *)calloc((size_t)bitmap_w * DS_TTF_SS, (size_t)bitmap_h * DS_TTF_SS);
    if (!outline || !cover) {
        free(outline);
        free(cover);
        return DS_TTF_BAKE_BROKEN;
    }
    if (!ttf_outline(face, glyph, outline, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                     0.5f / raster_scale / (float)DS_TTF_SS, 0) ||
        outline->count == 0) {
        free(outline);
        free(cover);
        return DS_TTF_BAKE_OK;
    }
    ttf_fill(outline, raster_scale * (float)DS_TTF_SS, -(float)bitmap_x0 * (float)DS_TTF_SS,
             -(float)bitmap_y0 * (float)DS_TTF_SS, cover, bitmap_w * DS_TTF_SS,
             bitmap_h * DS_TTF_SS);
    free(outline);
    if (!ttf_atlas_place(face, bitmap_w, bitmap_h, &ax, &ay)) {
        free(cover);
        return DS_TTF_BAKE_FULL;
    }
    for (y = 0; y < bitmap_h; ++y) {
        for (x = 0; x < bitmap_w; ++x) {
            int sum = 0;
            int sx = 0;
            int sy = 0;
            for (sy = 0; sy < DS_TTF_SS; ++sy)
                for (sx = 0; sx < DS_TTF_SS; ++sx)
                    sum += cover[((size_t)(y * DS_TTF_SS + sy) * (size_t)bitmap_w + (size_t)x) *
                                     (size_t)DS_TTF_SS +
                                 (size_t)sx];
            {
                size_t at = ((size_t)(ay + y) * DS_TTF_ATLAS + (size_t)(ax + x)) * 4;
                uint8_t alpha = (uint8_t)((sum * 255 + DS_TTF_SS * DS_TTF_SS / 2) /
                                          (DS_TTF_SS * DS_TTF_SS));
                face->atlas[at] = 255;
                face->atlas[at + 1] = 255;
                face->atlas[at + 2] = 255;
                face->atlas[at + 3] = alpha;
            }
        }
    }
    free(cover);
    slot->ax = ax;
    slot->ay = ay;
    slot->w = bitmap_w;
    slot->h = bitmap_h;
    slot->left = (float)bitmap_x0;
    slot->top = (float)bitmap_y0;
    /* The atlas moved, this layer only: the renderer copies one slice instead of
       rebuilding the texture array, which is what a per-tap digit deserves. */
    ds_image_touch_layer(face->layer);
    return status;
}

/* Bake one codepoint and hand out its slot.  A glyph that no longer fits the
 * atlas at the wanted size gets a second chance at the base size (which is the
 * only size the pass knew before per-size atlases existed) and only then
 * degrades to .notdef, so a big banner costs softness, never letters. */
static DsTtfGlyph *ttf_rasterize(DsTtfFace *face, uint32_t codepoint, uint16_t glyph,
                                 int raster) {
    DsTtfGlyph *slot = NULL;
    int status = 0;
    if (face->cache_count >= DS_TTF_CACHE) return &face->notdef;
    slot = &face->cache[face->cache_count];
    status = ttf_bake(face, codepoint, glyph, raster, slot);
    if (status == DS_TTF_BAKE_FULL && raster != DS_TTF_PX)
        status = ttf_bake(face, codepoint, glyph, DS_TTF_PX, slot);
    if (status != DS_TTF_BAKE_OK) {
        memset(slot, 0, sizeof(*slot));
        if (status == DS_TTF_BAKE_FULL) {
            /* Only "the atlas is shut" is worth remembering: from here on the
             * face hands out .notdef without walking the atlas again. */
            if (!face->atlas_full) face->atlas_full = 1;
            else app_log_error("font atlas full, '%u' becomes tofu", codepoint);
        }
        return &face->notdef;
    }
    ++face->cache_count;
    return slot;
}

static DsTtfGlyph *ttf_glyph(DsTtfFace *face, uint32_t codepoint, int raster) {
    DsTtfGlyph *found = ttf_cached(face, codepoint, raster);
    uint16_t glyph = 0;
    if (found) return found;
    if (face->atlas_full) return &face->notdef;
    glyph = ttf_glyph_id(face, codepoint);
    if (glyph == 0) return &face->notdef;
    return ttf_rasterize(face, codepoint, glyph, raster);
}

/* ------------------------------------------------------------------ layout */

static uint32_t ttf_next_codepoint(const char **text) {
    /* Strict-ish UTF-8: overlongs, surrogates and out-of-range sequences
     * become U+FFFD instead of smuggling bytes into the cmap. */
    static const uint32_t limits[] = {0x7Fu, 0x7FFu, 0xFFFFu, 0x10FFFFu};
    const uint8_t *cursor = (const uint8_t *)*text;
    uint8_t lead = *cursor;
    uint32_t codepoint = 0;
    int length = 0;
    int index = 0;
    if (lead < 0x80u) {
        *text += 1;
        return lead;
    }
    if (lead < 0xC2u) goto bad;
    if (lead < 0xE0u) {
        length = 1;
        codepoint = (uint32_t)(lead & 0x1Fu);
    } else if (lead < 0xF0u) {
        length = 2;
        codepoint = (uint32_t)(lead & 0x0Fu);
    } else if (lead < 0xF5u) {
        length = 3;
        codepoint = (uint32_t)(lead & 0x07u);
    } else {
        goto bad;
    }
    for (index = 0; index < length; ++index) {
        uint8_t part = cursor[1 + index];
        if (part < 0x80u || part > 0xBFu) goto bad;
        codepoint = (codepoint << 6) | (uint32_t)(part & 0x3Fu);
    }
    *text += 1 + length;
    if (codepoint > limits[length] || (codepoint >= 0xD800u && codepoint <= 0xDFFFu) ||
        codepoint > 0x10FFFFu)
        return 0xFFFDu;
    return codepoint;
bad:
    *text += 1;
    return 0xFFFDu;
}

static void ttf_draw_text(DsTtfFace *face, const EnjoerTextCommand *command) {
    /* Scale 1.0 is a 16 px em (the browser overlay's contract); the pen starts
     * at the command's top-left corner and the baseline sits one ascent below
     * it.  Glyphs bake at the em this text is drawn with, so `unit` maps one
     * atlas texel to one screen pixel for the whole ladder.
     *
     * Pen origin, baseline and every glyph box are rounded to whole pixels:
     * text then lands on the same texels on every frame instead of drifting
     * half a pixel around, which is what makes a HUD that redraws with a
     * changing scale (or after a resize) look steady rather than shaky.
     * Advances still accumulate unrounded, so rounding never smears the
     * spacing across a long line. */
    float em = 16.0f * command->scale;
    int raster = 0;
    float ascent = (float)face->ascent / (float)face->units_per_em * em;
    float line = (float)(face->ascent - face->descent + face->line_gap) /
                 (float)face->units_per_em * em;
    float pen_x = 0.0f;
    float baseline = 0.0f;
    const char *cursor = command->text;
    float inv_atlas = 1.0f / (float)DS_TTF_ATLAS;
    if (!(em > 0.0f) || !command->text[0]) return;
    raster = ttf_pick_raster(em);
    pen_x = (float)floor((double)command->x + 0.5);
    baseline = (float)floor((double)(command->y + ascent) + 0.5);
    while (*cursor) {
        uint32_t codepoint = ttf_next_codepoint(&cursor);
        DsTtfGlyph *glyph = NULL;
        float unit = 1.0f;
        float left = 0.0f;
        float top = 0.0f;
        if (codepoint == (uint32_t)'\n') {
            pen_x = (float)floor((double)command->x + 0.5);
            baseline = (float)floor((double)(baseline + line) + 0.5);
            continue;
        }
        if (codepoint == (uint32_t)'\r' || codepoint == 0u) continue;
        if (codepoint == (uint32_t)'\t') {
            DsTtfGlyph *space = ttf_glyph(face, (uint32_t)' ', raster);
            pen_x += 4.0f * space->advance * (em / (float)space->raster);
            continue;
        }
        glyph = ttf_glyph(face, codepoint, raster);
        unit = em / (float)glyph->raster;
        left = glyph->left * unit;
        top = glyph->top * unit;
        if (glyph->w > 0 && glyph->h > 0) {
            float gx0 = (float)floor((double)(pen_x + left) + 0.5);
            float gy0 = (float)floor((double)(baseline + top) + 0.5);
            float gx1 = (float)floor((double)(pen_x + (glyph->left + glyph->w) * unit) + 0.5);
            float gy1 = (float)floor((double)(baseline + (glyph->top + glyph->h) * unit) + 0.5);
            /* Half a texel inside the cell on every side: the quad edge then
             * samples the centre of the first and last ink texel, so the
             * bilinear tap never reaches into the padding or the neighbour
             * glyph (which is what used to wash out thin strokes). */
            float u0 = ((float)glyph->ax + 0.5f) * inv_atlas;
            float v0 = ((float)glyph->ay + 0.5f) * inv_atlas;
            float u1 = ((float)(glyph->ax + glyph->w) - 0.5f) * inv_atlas;
            float v1 = ((float)(glyph->ay + glyph->h) - 0.5f) * inv_atlas;
            if (gx1 > gx0 && gy1 > gy0)
                enjoer_draw_image_quad(gx0, gy0, gx1 - gx0, gy1 - gy0, u0, v0, u1, v1,
                                       face->layer, command->r, command->g, command->b);
        }
        pen_x += glyph->advance * unit;
    }
}

/* -------------------------------------------------------------- public API */

int32_t ds_ttf_load_face(int32_t font) {
    const DsFont *source = ds_font_at(font);
    DsTtfFace *face = ttf_face(font);
    DsTtfFont parsed;
    size_t head = 0;
    size_t head_length = 0;
    size_t hhea = 0;
    size_t hhea_length = 0;
    size_t maxp = 0;
    size_t maxp_length = 0;
    size_t cmap = 0;
    size_t cmap_length = 0;
    size_t loca = 0;
    size_t loca_length = 0;
    size_t glyf = 0;
    size_t glyf_length = 0;
    size_t hmtx = 0;
    size_t hmtx_length = 0;
    int16_t format = 0;
    char name[DS_FONT_NAME_LENGTH + 8];
    (void)head_length;
    (void)hhea_length;
    (void)maxp_length;
    (void)cmap_length;
    (void)loca_length;
    (void)hmtx_length;
    if (!face || !source || !source->bytes || !source->size) return -1;
    if (face->ready) return face->layer;
    memset(face, 0, sizeof(*face));
    parsed.bytes = source->bytes;
    parsed.size = source->size;
    face->font = parsed;
    if (!ttf_table(&parsed, "head", &head, &head_length) || head_length < 54 ||
        !ttf_table(&parsed, "hhea", &hhea, &hhea_length) || hhea_length < 36 ||
        !ttf_table(&parsed, "maxp", &maxp, &maxp_length) || maxp_length < 6 ||
        !ttf_table(&parsed, "cmap", &cmap, &cmap_length) ||
        !ttf_table(&parsed, "loca", &loca, &loca_length) ||
        !ttf_table(&parsed, "glyf", &glyf, &glyf_length) ||
        !ttf_table(&parsed, "hmtx", &hmtx, &hmtx_length)) {
        app_log_error("font %s: missing TrueType tables", source->name);
        return -1;
    }
    if (!ttf_u16(&parsed, head + 18, &face->units_per_em) || face->units_per_em == 0 ||
        !ttf_i16(&parsed, head + 50, &format) ||
        !ttf_u16(&parsed, maxp + 4, &face->num_glyphs) || face->num_glyphs == 0 ||
        !ttf_u16(&parsed, hhea + 34, &face->num_h_metrics) || face->num_h_metrics == 0 ||
        !ttf_i16(&parsed, hhea + 4, &face->ascent) || !ttf_i16(&parsed, hhea + 6, &face->descent) ||
        !ttf_i16(&parsed, hhea + 8, &face->line_gap)) {
        app_log_error("font %s: broken TrueType headers", source->name);
        return -1;
    }
    face->loca_long = format != 0;
    {
        size_t need = ((size_t)face->num_glyphs + 1) * (face->loca_long ? 4u : 2u);
        if (loca_length < need || hmtx_length < (size_t)face->num_h_metrics * 4) {
            app_log_error("font %s: truncated TrueType tables", source->name);
            return -1;
        }
    }
    face->loca = loca;
    face->glyf = glyf;
    face->glyf_length = glyf_length;
    face->hmtx = hmtx;
    face->hmtx_length = hmtx_length;
    if (!ttf_pick_cmap(face, cmap, cmap_length)) {
        app_log_error("font %s: no Unicode cmap", source->name);
        return -1;
    }
    face->atlas = (uint8_t *)calloc((size_t)DS_TTF_ATLAS * DS_TTF_ATLAS, 4);
    if (!face->atlas) return -1;
    face->atlas_x = DS_TTF_ATLAS_PAD;
    face->atlas_y = DS_TTF_ATLAS_PAD;
    snprintf(name, sizeof(name), "@font%d", font);
    face->layer = ds_image_add(name, face->atlas, DS_TTF_ATLAS, DS_TTF_ATLAS);
    if (face->layer < 0) {
        app_log_error("font %s: too many images for the atlas", source->name);
        free(face->atlas);
        face->atlas = NULL;
        return -1;
    }
    face->ready = 1;
    /* The .notdef box takes the first atlas cell, so a missing glyph and a
     * full atlas both degrade to honest tofu instead of nothing. */
    {
        DsTtfGlyph box;
        memset(&box, 0, sizeof(box));
        box.ready = 1;
        box.raster = DS_TTF_PX; /* the .notdef always lives at the base size */
        face->notdef = box;
        if (face->num_glyphs > 0) {
            DsTtfGlyph *slot = ttf_rasterize(face, 0xFFFDu, 0, DS_TTF_PX);
            if (slot && slot->raster > 0) face->notdef = *slot;
            face->cache_count = 0;
        }
        if (!face->notdef.w) {
            /* Empty .notdef (a blank first glyph): draw the classic hollow
             * box by hand so tofu never means invisible. */
            int32_t ax = 0;
            int32_t ay = 0;
            int side = DS_TTF_PX * 2 / 3;
            int x = 0;
            int y = 0;
            if (ttf_atlas_place(face, side, side, &ax, &ay)) {
                for (y = 0; y < side; ++y) {
                    for (x = 0; x < side; ++x) {
                        int edge = x < 2 || y < 2 || x >= side - 2 || y >= side - 2;
                        size_t at = ((size_t)(ay + y) * DS_TTF_ATLAS + (size_t)(ax + x)) * 4;
                        face->atlas[at] = 255;
                        face->atlas[at + 1] = 255;
                        face->atlas[at + 2] = 255;
                        face->atlas[at + 3] = edge ? 255 : 0;
                    }
                }
                face->notdef.ax = ax;
                face->notdef.ay = ay;
                face->notdef.w = side;
                face->notdef.h = side;
                face->notdef.left = 0.0f;
                face->notdef.top = -(float)face->ascent / (float)face->units_per_em *
                                   (float)DS_TTF_PX;
                face->notdef.advance = (float)side;
                face->notdef.raster = DS_TTF_PX;
                ds_image_touch_layer(face->layer);
            }
        }
    }
    return face->layer;
}

int32_t ds_ttf_atlas_size(void) { return DS_TTF_ATLAS; }

int32_t ds_ttf_layer(int32_t font) {
    const DsTtfFace *face = ttf_face(font);
    if (!face || !face->ready) return -1;
    return face->layer;
}

void ds_ttf_resolve_frame(void) {
    EnjoerFrame *frame = enjoer_frame();
    int index = 0;
    if (!frame || frame->texts_resolved) return;
    for (index = 0; index < frame->text_count; ++index) {
        const EnjoerTextCommand *command = &frame->texts[index];
        DsTtfFace *face = NULL;
        if (!ds_font_valid(command->font)) continue;
        face = ttf_face(command->font);
        if (!face || !face->ready) continue;
        ttf_draw_text(face, command);
    }
    frame->texts_resolved = 1;
}

void ds_ttf_reset(void) {
    /* Atlas pixels belong to the image registry (freed by ds_image_reset);
     * faces only forget them. */
    memset(faces, 0, sizeof(faces));
}

