/* graphics/gfx_frame.c — очередь команд кадра и жизненный цикл 2D-рендера.
 * Игра только складывает команды (rect/circle/text/...), отрисовка
 * происходит в конце кадра — так рисование можно отменить при ошибке. */
#include "gfx_internal.h"
#include <stdlib.h>
#include <string.h>

static Buffer *cur_buf;
static GfxCmd *cmds;
static size_t cmd_n, cmd_cap;
static int frame_open;
static AAssetManager *amgr;

/* ── ассеты ──────────────────────────────────────────────────────────── */
AAssetManager *gfx_assets(void) { return amgr; }
void gfx_set_assets(AAssetManager *a) { amgr = a; }

int gfx_open_asset(const char *n, uint8_t **out, size_t *sz) {
    if (!n || !out || !sz || !amgr) return 0;
    AAsset *a = AAssetManager_open(amgr, n, AASSET_MODE_BUFFER);
    if (!a) return 0;
    off_t len = AAsset_getLength(a);
    if (len <= 0 || (uint64_t)len > SIZE_MAX) { AAsset_close(a); return 0; }
    uint8_t *buf = (uint8_t *)malloc((size_t)len);
    if (!buf) { AAsset_close(a); return 0; }
    size_t off = 0;
    while (off < (size_t)len) {
        int nr = AAsset_read(a, buf+off, (size_t)len-off);
        if (nr <= 0) break; off += nr;
    }
    AAsset_close(a);
    if (off != (size_t)len) { free(buf); return 0; }
    *out = buf; *sz = (size_t)len; return 1;
}

/* ── очередь команд ──────────────────────────────────────────────────── */
int gfx_frame_is_open(void) { return frame_open; }
Buffer *gfx_current_buffer(void) { return cur_buf; }

GfxCmd *gfx_cmd_push(GfxCmdType t) {
    if (!frame_open) return NULL;
    if (cmd_n == cmd_cap) {
        size_t cap = cmd_cap ? cmd_cap*2 : 256;
        if (cap < cmd_n || cap > SIZE_MAX/sizeof(*cmds)) { ds_runtime_error("too many renderer commands"); return NULL; }
        GfxCmd *nc = (GfxCmd *)realloc(cmds, cap*sizeof(*cmds));
        if (!nc) { ds_runtime_error("out of memory in command buffer"); return NULL; }
        cmds = nc; cmd_cap = cap;
    }
    GfxCmd *c = &cmds[cmd_n++];
    memset(c, 0, sizeof(*c)); c->t = t; return c;
}

static void flush(void) {
    if (!cur_buf) return;
    for (size_t i = 0; i < cmd_n; i++) {
        GfxCmd *c = &cmds[i];
        switch (c->t) {
            case GFX_CMD_RECT:  gfx_render_rect(cur_buf, c->v.rc.x, c->v.rc.y, c->v.rc.w, c->v.rc.h, c->v.rc.c); break;
            case GFX_CMD_ROUND: gfx_render_roundrect(cur_buf, c->v.rr.x, c->v.rr.y, c->v.rr.w, c->v.rr.h, c->v.rr.r, c->v.rr.c); break;
            case GFX_CMD_CIRCLE: gfx_render_circle(cur_buf, c->v.ci.x, c->v.ci.y, c->v.ci.r, c->v.ci.c); break;
            case GFX_CMD_RING:  gfx_render_ring(cur_buf, c->v.rg.x, c->v.rg.y, c->v.rg.r, c->v.rg.th, c->v.rg.c); break;
            case GFX_CMD_LINE:  gfx_render_line(cur_buf, c->v.ln.x1, c->v.ln.y1, c->v.ln.x2, c->v.ln.y2, c->v.ln.th, c->v.ln.c); break;
            case GFX_CMD_TRI:   gfx_render_triangle(cur_buf, c->v.tr.x1, c->v.tr.y1, c->v.tr.x2, c->v.tr.y2, c->v.tr.x3, c->v.tr.y3, c->v.tr.c); break;
            case GFX_CMD_TEX:   gfx_draw_texture(cur_buf, c->v.tx.tx, c->v.tx.x, c->v.tx.y, c->v.tx.a, c->v.tx.sc); break;
            case GFX_CMD_TEX_TINT: gfx_draw_texture_tint(cur_buf, c->v.tx2.tx, c->v.tx2.x, c->v.tx2.y, c->v.tx2.a, c->v.tx2.sc, c->v.tx2.c); break;
            case GFX_CMD_TEXT:
                gfx_render_text(cur_buf, c->v.tt.s, c->v.tt.x, c->v.tt.y, c->v.tt.c, c->v.tt.sc);
                free(c->v.tt.s); c->v.tt.s = NULL;
                break;
        }
    }
}

static void discard_commands(void) {
    for (size_t i = 0; i < cmd_n; i++) {
        if (cmds[i].t == GFX_CMD_TEXT) {
            free(cmds[i].v.tt.s);
            cmds[i].v.tt.s = NULL;
        }
    }
    cmd_n = 0;
}

/* ── публичные команды рисования (объявлены в runtime.h) ─────────────── */
void rect(float x, float y, float w, float h, uint32_t c) {
    GfxCmd *p = gfx_cmd_push(GFX_CMD_RECT); if (!p) return;
    p->v.rc.x=x; p->v.rc.y=y; p->v.rc.w=w; p->v.rc.h=h; p->v.rc.c=gfx_pack(c);
}

void clear_screen(uint32_t c) {
    rect(0.0f, 0.0f, (float)screen_w, (float)screen_h, c);
}

void roundrect(float x, float y, float w, float h, float r, uint32_t c) {
    GfxCmd *p = gfx_cmd_push(GFX_CMD_ROUND); if (!p) return;
    p->v.rr.x=x; p->v.rr.y=y; p->v.rr.w=w; p->v.rr.h=h; p->v.rr.r=r; p->v.rr.c=gfx_pack(c);
}

void circle(float x, float y, float r, uint32_t c) {
    GfxCmd *p = gfx_cmd_push(GFX_CMD_CIRCLE); if (!p) return;
    p->v.ci.x=x; p->v.ci.y=y; p->v.ci.r=r; p->v.ci.c=gfx_pack(c);
}

void ring(float x, float y, float r, float t, uint32_t c) {
    GfxCmd *p = gfx_cmd_push(GFX_CMD_RING); if (!p) return;
    p->v.rg.x=x; p->v.rg.y=y; p->v.rg.r=r; p->v.rg.th=t; p->v.rg.c=gfx_pack(c);
}

void line(float x1, float y1, float x2, float y2, float thickness, uint32_t c) {
    GfxCmd *p = gfx_cmd_push(GFX_CMD_LINE); if (!p) return;
    p->v.ln.x1=x1; p->v.ln.y1=y1; p->v.ln.x2=x2; p->v.ln.y2=y2;
    p->v.ln.th=thickness; p->v.ln.c=gfx_pack(c);
}

void tri(float x1, float y1, float x2, float y2, float x3, float y3, uint32_t c) {
    GfxCmd *p = gfx_cmd_push(GFX_CMD_TRI); if (!p) return;
    p->v.tr.x1=x1; p->v.tr.y1=y1; p->v.tr.x2=x2; p->v.tr.y2=y2;
    p->v.tr.x3=x3; p->v.tr.y3=y3; p->v.tr.c=gfx_pack(c);
}

/* ── жизненный цикл ──────────────────────────────────────────────────── */
void ds_release_assets(void) {
    gfx_texture_release_all();
    gfx_font_release();
    amgr = NULL;
}

void ds_set_asset_manager(AAssetManager *a) {
    if (amgr != a) { ds_release_assets(); amgr = a; }
    if (!amgr) ds_runtime_error("Android asset manager is unavailable");
}

int ds_graphics_init(AAssetManager *a) { if (amgr != a) { ds_release_assets(); amgr = a; } return 1; }

int ds_graphics_begin_frame(Buffer *b) {
    if (!b || !b->pixels || b->width <= 0 || b->height <= 0 || b->stride < b->width) return 0;
    discard_commands(); cur_buf = b; frame_open = 1;
    gfx_clear(b, gfx_pack(0x00000000));
    return 1;
}

void ds_graphics_end_frame(void) {
    if (!frame_open) return;
    flush(); cmd_n = 0; frame_open = 0; cur_buf = NULL;
}

void ds_graphics_cancel_frame(void) { discard_commands(); frame_open = 0; cur_buf = NULL; }

void ds_graphics_shutdown(void) {
    ds_graphics_cancel_frame();
    ds_release_assets();
    free(cmds); cmds = NULL; cmd_cap = 0; cmd_n = 0;
}
