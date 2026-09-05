/* graphics/gfx_internal.h — общие внутренности 2D-рендера.
 * Публичный API — в runtime.h; здесь только то, что нужно файлам
 * модуля graphics/ между собой. */
#ifndef GFX_INTERNAL_H
#define GFX_INTERNAL_H

#include "runtime.h"
#include "ttf/ttf_internal.h"

/* ── цвет и базовые операции ─────────────────────────────────────────── */
/* 0xAARRGGBB (как в вызовах игры) -> 0xAABBGGRR (порядок в буфере кадра). */
uint32_t gfx_pack(uint32_t c);
/* альфа-смешивание поверх: s рисуется над d (оба в упакованном виде) */
uint32_t gfx_blend(uint32_t d, uint32_t s);
int gfx_cl_floor(float v, int lim);
int gfx_cl_ceil(float v, int lim);
void gfx_fill_span(uint32_t *d, int n, uint32_t c);
void gfx_paint_span(uint32_t *d, int n, uint32_t c);
void gfx_clear(Buffer *b, uint32_t c);

/* ── примитивы (gfx_draw.c) ──────────────────────────────────────────── */
void gfx_render_rect(Buffer *b, float x, float y, float w, float h, uint32_t c);
void gfx_render_roundrect(Buffer *b, float x, float y, float w, float h, float rad, uint32_t c);
void gfx_render_circle(Buffer *b, float x, float y, float rad, uint32_t c);
void gfx_render_ring(Buffer *b, float x, float y, float rad, float th, uint32_t c);
void gfx_render_triangle(Buffer *b, float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c);
void gfx_render_line(Buffer *b, float x1, float y1, float x2, float y2, float th, uint32_t c);

/* ── текстуры (gfx_texture.c) ────────────────────────────────────────── */
typedef struct GfxTexture GfxTexture;
struct GfxTexture {
    GfxTexture *next;
    char *name;
    int w, h, opaque;
    uint32_t *pixels;
};
GfxTexture *gfx_texture_load(const char *req);
void gfx_texture_release_all(void);
void gfx_draw_texture(Buffer *b, const GfxTexture *t, float x, float y, float a, float sc);
void gfx_draw_texture_tint(Buffer *b, const GfxTexture *t, float x, float y, float a, float sc, uint32_t tint);

/* ── ассеты (gfx_frame.c) ────────────────────────────────────────────── */
AAssetManager *gfx_assets(void);
void gfx_set_assets(AAssetManager *a);
int gfx_open_asset(const char *name, uint8_t **out, size_t *size);

/* ── текст (gfx_text.c) ──────────────────────────────────────────────── */
int gfx_font_ensure(void);
DSFont *gfx_font_get(void);
void gfx_font_release(void);
int gfx_utf8_decode(const char **cursor);
void gfx_render_text(Buffer *b, const char *s, float x, float y, uint32_t c, float sc);

/* ── очередь команд кадра (gfx_frame.c) ──────────────────────────────── */
typedef enum {
    GFX_CMD_RECT, GFX_CMD_ROUND, GFX_CMD_CIRCLE, GFX_CMD_RING, GFX_CMD_LINE,
    GFX_CMD_TEX, GFX_CMD_TEXT, GFX_CMD_TEX_TINT, GFX_CMD_TRI
} GfxCmdType;

typedef struct {
    GfxCmdType t;
    union {
        struct { float x, y, w, h; uint32_t c; } rc;
        struct { float x, y, w, h, r; uint32_t c; } rr;
        struct { float x, y, r; uint32_t c; } ci;
        struct { float x, y, r, th; uint32_t c; } rg;
        struct { float x1, y1, x2, y2, th; uint32_t c; } ln;
        struct { float x, y, a, sc; GfxTexture *tx; } tx;
        struct { char *s; float x, y, sc; uint32_t c; } tt;
        struct { float x, y, a, sc; GfxTexture *tx; uint32_t c; } tx2;
        struct { float x1, y1, x2, y2, x3, y3; uint32_t c; } tr;
    } v;
} GfxCmd;

GfxCmd *gfx_cmd_push(GfxCmdType t);
int gfx_frame_is_open(void);
Buffer *gfx_current_buffer(void);

#endif
