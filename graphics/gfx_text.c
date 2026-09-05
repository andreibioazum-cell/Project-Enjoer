/* graphics/gfx_text.c — текст: загрузка шрифта, команды, метрики,
 * растеризация глифов и экран ошибки с консолью.
 *
 * Антипикселизация: глифы запекаются в атлас крупнее (48px вместо 32)
 * и при отрисовке сэмплируются билинейно, а не «ближайшим пикселем» —
 * текст гладкий при любом масштабе (см. atlas_sample). */
#include "gfx_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

static DSFont *font;
static int font_tried;

/* Высота, в которой запекается шрифтовой атлас. Раньше было 32 — при
 * масштабе текста 0.4…0.6 (типичный интерфейс) из 32 пикселей получалось
 * 13–19, и глифы рассыпались на «пиксельные лесенки». 48 даёт запас
 * детализации даже для крупных надписей. */
#define GFX_FONT_PIXEL_HEIGHT 48

int gfx_font_ensure(void) {
    if (font) return 1;
    if (font_tried) return 0;
    font_tried = 1;
    uint8_t *data = NULL; size_t sz = 0;
    if (!gfx_open_asset("fonts/ChillRoundGothic_Heavy.ttf", &data, &sz)) {
        ds_log_err("font not loaded: fonts/ChillRoundGothic_Heavy.ttf not found");
        return 0;
    }
    font = ds_font_create(data, sz, GFX_FONT_PIXEL_HEIGHT);
    free(data);
    if (!font) { ds_log_err("font not loaded: could not parse TrueType font"); return 0; }
    ds_log("font loaded: fonts/ChillRoundGothic_Heavy.ttf");
    return 1;
}

DSFont *gfx_font_get(void) { return font; }

void gfx_font_release(void) {
    ds_font_destroy(font); font = NULL; font_tried = 0;
}

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

static char *strdup_safe(const char *s) {
    if (!s) { char *o = (char *)malloc(1); if (o) o[0] = '\0'; return o; }
    size_t n = strlen(s) + 1;
    char *o = (char *)malloc(n);
    if (o) memcpy(o, s, n);
    return o;
}

/* Билинейное сэмплирование альфа-атласа с ограничением рамкой глифа,
 * чтобы соседние глифы не «подтекали» на края. */
static uint8_t atlas_sample(const uint8_t *al, int aw, int ah, float x, float y,
                            float minx, float miny, float maxx, float maxy) {
    if (x < minx) x = minx;
    if (y < miny) y = miny;
    if (x > maxx) x = maxx;
    if (y > maxy) y = maxy;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    int x1 = x0 + 1, y1 = y0 + 1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;
    if (x1 >= aw) x1 = aw - 1;
    if (y1 >= ah) y1 = ah - 1;
    float fx = x - (float)x0, fy = y - (float)y0;
    float a00 = (float)al[y0*aw + x0], a10 = (float)al[y0*aw + x1];
    float a01 = (float)al[y1*aw + x0], a11 = (float)al[y1*aw + x1];
    float top = a00 + (a10 - a00) * fx;
    float bot = a01 + (a11 - a01) * fx;
    float v = top + (bot - top) * fy;
    if (v < 0) v = 0; if (v > 255) v = 255;
    return (uint8_t)(v + 0.5f);
}

void gfx_render_text(Buffer *b, const char *s, float x, float y, uint32_t c, float sc) {
    if (!b || !font || !s || !isfinite(x+y+sc) || sc <= 0) return;
    int aw = ds_font_aw(font), ah = ds_font_ah(font);
    const uint8_t *al = ds_font_alpha(font);
    float asc = ds_font_ascent(font), lb = 0;
    const DSFontGlyph *ref = ds_font_glyph(font, 'S');
    if (ref) { asc = ref->bearing_top; lb = ref->bearing_x; }
    float pen = x - lb*sc, base = y + asc*sc;
    float inv = 1.0f / sc;
    for (const char *cur = s; *cur;) {
        int cp = gfx_utf8_decode(&cur);
        if (cp == '\n') { pen = x - lb*sc; base += ds_font_lineh(font)*sc; continue; }
        const DSFontGlyph *g = ds_font_glyph(font, (uint32_t)cp);
        if (!g || g->width <= 0 || g->height <= 0) { if (g) pen += g->advance * sc; continue; }
        float sx0 = g->u0*aw, sy0 = g->v0*ah;
        float sx1 = g->u1*aw, sy1 = g->v1*ah;
        int dw = (int)ceilf(g->width*sc), dh = (int)ceilf(g->height*sc);
        int dx = (int)floorf(pen + g->bearing_x*sc), dy = (int)floorf(base - g->bearing_top*sc);
        for (int yy = 0; yy < dh; yy++) {
            int scr_y = dy + yy; if (scr_y < 0 || scr_y >= b->height) continue;
            /* центр экранного пикселя в координатах атласа */
            float fy = sy0 + ((float)yy + 0.5f) * inv - 0.5f;
            for (int xx = 0; xx < dw; xx++) {
                int scr_x = dx + xx; if (scr_x < 0 || scr_x >= b->width) continue;
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

/* ── команды текста ──────────────────────────────────────────────────── */
/* Раньше игра силой красила весь текст в белый. Мессенджеру нужны обычные
 * цвета (тёмный текст на светлых пузырях, серые подписи, зелёные статусы),
 * поэтому теперь цвет передаётся в рендер как есть. */
void text_scaled(const char *s, float x, float y, uint32_t c, float sc) {
    if (!gfx_frame_is_open() || !s || !gfx_font_ensure()) return;
    GfxCmd *p = gfx_cmd_push(GFX_CMD_TEXT); if (!p) return;
    /* Network and script strings can come from reused buffers; renderer commands
     * must own text until the frame is flushed or cancelled. */
    p->v.tt.s = strdup_safe(s);
    if (!p->v.tt.s) { ds_runtime_error("out of memory copying renderer text"); return; }
    p->v.tt.x=x; p->v.tt.y=y; p->v.tt.sc=sc; p->v.tt.c=gfx_pack(c);
}

void text(const char *s, float x, float y, uint32_t c) { text_scaled(s, x, y, c, 1.0f); }

/* ── метрики ─────────────────────────────────────────────────────────── */
int text_ink_width(const char *s) {
    if (!s || !gfx_font_ensure()) return 0;
    const DSFontGlyph *ref = ds_font_glyph(font, 'S');
    float pen = -(ref ? ref->bearing_x : 0);
    int first = 1; float minL = 0, maxR = 0;
    for (const char *c = s; *c;) {
        int cp = gfx_utf8_decode(&c);
        const DSFontGlyph *g = ds_font_glyph(font, (uint32_t)cp);
        if (!g) continue;
        float dl = pen + g->bearing_x, dr = dl + g->width;
        if (first || dl < minL) minL = dl;
        if (first || dr > maxR) maxR = dr;
        first = 0; pen += g->advance;
    }
    return first ? 0 : (int)(maxR - minL + 0.5f);
}

int text_ink_height(const char *s) {
    if (!s || !gfx_font_ensure()) return 0;
    const DSFontGlyph *ref = ds_font_glyph(font, 'S');
    float base = ref ? ref->bearing_top : 0;
    int first = 1; float minT = 0, maxB = 0;
    for (const char *c = s; *c;) {
        int cp = gfx_utf8_decode(&c);
        const DSFontGlyph *g = ds_font_glyph(font, (uint32_t)cp);
        if (!g) continue;
        float dt = base - g->bearing_top, db = dt + g->height;
        /* Нижние выносные элементы ('р','у','д','g','y' и т.п.) не должны
         * раздувать высоту ink-бокса: иначе строка с ними центрируется выше
         * строк без них, и текст «уезжает вверх» на часть выносного элемента.
         * Обрезаем низ по базовой линии — она ровно в dt == base. */
        if (db > base) db = base;
        if (first || dt < minT) minT = dt;
        if (first || db > maxB) maxB = db;
        first = 0;
    }
    return first ? 0 : (int)(maxB - minT + 0.5f);
}

int text_width(const char *s) { return text_ink_width(s); }
int text_height(const char *s) { return text_ink_height(s); }

/* Шаг строки многострочного текста (тот самый, что использует '\n' внутри
 * gfx_render_text). Мессенджеру нужен для точного расчёта высоты пузырей. */
float text_line_height(void) {
    if (!gfx_font_ensure()) return 24.0f;
    return ds_font_lineh(font);
}

int text_ink_top(const char *s) {
    if (!s || !gfx_font_ensure()) return 0;
    const DSFontGlyph *ref = ds_font_glyph(font, 'S');
    float base = ref ? ref->bearing_top : 0;
    int first = 1; float minT = 0;
    for (const char *c = s; *c;) {
        int cp = gfx_utf8_decode(&c);
        const DSFontGlyph *g = ds_font_glyph(font, (uint32_t)cp);
        if (!g) continue;
        float dt = base - g->bearing_top;
        if (first || dt < minT) minT = dt;
        first = 0;
    }
    return first ? 0 : (int)floorf(minT);
}

/* ── экран ошибки рантайма ───────────────────────────────────────────── */
void ds_graphics_error_screen(const char *m) {
    Buffer *b = gfx_current_buffer();
    if (!b) return;
    gfx_clear(b, gfx_pack(0xff1c0b10));
    if (!font) return;
    int y = 12;
    gfx_render_text(b, "=== DIMSCRIPT ERROR ===", 16, y, gfx_pack(0xffffffff), 0.7f);
    y += 30;
    if (m && *m) {
        gfx_render_text(b, m, 16, y, gfx_pack(0xffffb0b0), 0.6f);
        y += 28;
    }
    y += 4;
    gfx_render_text(b, "--- console (last lines) ---", 16, y, gfx_pack(0xff9aa0b0), 0.55f);
    y += 24;
    int n = console_count();
    int start = n > 16 ? n - 16 : 0;
    for (int i = start; i < n; i++) {
        const char *line = console_line(i);
        if (!line || !*line) { y += 19; continue; }
        uint32_t col = console_type(i) ? gfx_pack(0xffff8a80) : gfx_pack(0xffc8d0dc);
        gfx_render_text(b, line, 16, y, col, 0.55f);
        y += 19;
        if (y > b->height - 8) break;
    }
}
