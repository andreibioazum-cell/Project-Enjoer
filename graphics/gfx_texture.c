/* graphics/gfx_texture.c — PNG-текстуры из ассетов и их отрисовка
 * (с поворотом/без, обычная и тонированная — для силуэтов-теней). */
#include "gfx_internal.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb_image.h"
#ifdef STB_IMAGE_STATIC
#undef STB_IMAGE_STATIC
#endif

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static GfxTexture *textures;

static char *strdup_safe(const char *s) {
    if (!s) { char *o = (char *)malloc(1); if (o) o[0] = '\0'; return o; }
    size_t n = strlen(s) + 1;
    char *o = (char *)malloc(n);
    if (o) memcpy(o, s, n);
    return o;
}

static GfxTexture *find_tx(const char *n) {
    for (GfxTexture *t = textures; t; t = t->next) if (strcmp(t->name, n) == 0) return t;
    return NULL;
}

static const char *norm_name(const char *n) {
    if (!n) return NULL;
    while (strncmp(n, "./", 2) == 0) n += 2;
    if (strncmp(n, "game/assets/", 12) == 0) n += 12;
    else if (strncmp(n, "assets/", 7) == 0) n += 7;
    if (!*n || *n == '/' || strchr(n, '\\')) return NULL;
    for (const char *c = n; *c;) {
        const char *s = c; while (*c && *c != '/') c++;
        if (c-s == 2 && s[0]=='.' && s[1]=='.') return NULL;
        if (*c == '/') c++;
    }
    return n;
}

GfxTexture *gfx_texture_load(const char *req) {
    const char *n = norm_name(req);
    if (!n) { ds_log_err("texture not loaded: invalid PNG asset path '%s'", req ? req : "(null)"); return NULL; }
    GfxTexture *t = find_tx(n);
    if (t) return t->pixels ? t : NULL;
#ifdef __ANDROID__
    if (!gfx_assets()) { ds_log_err("texture not loaded: no asset manager for '%s'", n); return NULL; }
#endif
    t = (GfxTexture *)calloc(1, sizeof(*t));
    if (!t) { ds_log_err("texture not loaded: out of memory caching '%s'", n); return NULL; }
    t->name = strdup_safe(n);
    if (!t->name) { free(t); ds_log_err("texture not loaded: out of memory caching name '%s'", n); return NULL; }
    t->next = textures; textures = t;
    uint8_t *enc = NULL; size_t enc_sz = 0;
    if (!gfx_open_asset(n, &enc, &enc_sz) || enc_sz > (size_t)INT_MAX) {
        free(enc); ds_log_err("texture not loaded: asset not found or unreadable: '%s'", n); return NULL;
    }
    int ch = 0;
    stbi_uc *dec = stbi_load_from_memory(enc, (int)enc_sz, &t->w, &t->h, &ch, STBI_rgb_alpha);
    free(enc);
    if (!dec || t->w <= 0 || t->h <= 0) {
        ds_log_err("texture not loaded: could not decode PNG '%s': %s", n, stbi_failure_reason() ? stbi_failure_reason() : "unknown");
        stbi_image_free(dec); return NULL;
    }
    size_t pc = (size_t)t->w * t->h;
    t->pixels = (uint32_t *)malloc(pc * sizeof(*t->pixels));
    if (!t->pixels) { stbi_image_free(dec); ds_log_err("texture not loaded: out of memory uploading PNG '%s'", n); return NULL; }
    t->opaque = 1;
    for (size_t i = 0; i < pc; i++) {
        uint8_t a = dec[i*4+3];
        t->pixels[i] = dec[i*4] | ((uint32_t)dec[i*4+1] << 8) | ((uint32_t)dec[i*4+2] << 16) | ((uint32_t)a << 24);
        if (a != 255) t->opaque = 0;
    }
    stbi_image_free(dec);
    ds_log("texture loaded: %s (%dx%d)", n, t->w, t->h);
    return t;
}

void gfx_texture_release_all(void) {
    GfxTexture *t = textures;
    while (t) { GfxTexture *n = t->next; free(t->pixels); free(t->name); free(t); t = n; }
    textures = NULL;
}

int png_load(const char *n) { return gfx_texture_load(n) != NULL; }

/* ── отрисовка ───────────────────────────────────────────────────────── */
static void draw_tx_unrot(Buffer *b, const GfxTexture *t, float x, float y, float sc) {
    if (!b || !t || !t->pixels || !isfinite(x+y+sc) || sc <= 0) return;
    float w = t->w*sc, h = t->h*sc;
    if (x >= b->width || y >= b->height || x+w <= 0 || y+h <= 0) return;
    int l = gfx_cl_floor(floorf(x), b->width), t0 = gfx_cl_floor(floorf(y), b->height);
    int r = gfx_cl_ceil(ceilf(x+w), b->width),  bo = gfx_cl_ceil(ceilf(y+h), b->height);
    if (sc == 1.0f && x == floorf(x) && y == floorf(y) && t->opaque && l == (int)x && t0 == (int)y) {
        int sl = l - (int)x, st = t0 - (int)y;
        for (int sy = t0; sy < bo; sy++) {
            int src_y = st + sy - t0;
            int cw = r - l;
            if (src_y < 0 || src_y >= t->h) continue;
            if (sl < 0) { cw += sl; sl = 0; }
            if (sl + cw > t->w) cw = t->w - sl;
            if (cw > 0) memcpy(b->pixels + sy*b->stride + l, t->pixels + src_y*t->w + sl, (size_t)cw*4);
        }
        return;
    }
    for (int sy = t0; sy < bo; sy++) {
        int src_y = (int)(((float)sy + 0.5f - y) / sc);
        if (src_y < 0) src_y = 0; if (src_y >= t->h) src_y = t->h - 1;
        for (int sx = l; sx < r; sx++) {
            int src_x = (int)(((float)sx + 0.5f - x) / sc);
            if (src_x < 0) src_x = 0; if (src_x >= t->w) src_x = t->w - 1;
            b->pixels[sy*b->stride + sx] = gfx_blend(b->pixels[sy*b->stride + sx], t->pixels[src_y*t->w + src_x]);
        }
    }
}

static void draw_tx_rot(Buffer *b, const GfxTexture *t, float x, float y, float ang, float sc) {
    float hw = t->w*0.5f*sc, hh = t->h*0.5f*sc;
    float cx = x+hw, cy = y+hh, ca = cosf(ang), sa = sinf(ang);
    float dx = fabsf(hw*ca) + fabsf(hh*sa), dy = fabsf(hw*sa) + fabsf(hh*ca);
    int l = gfx_cl_floor(floorf(cx-dx), b->width), t0 = gfx_cl_floor(floorf(cy-dy), b->height);
    int r = gfx_cl_ceil(ceilf(cx+dx), b->width),   bo = gfx_cl_ceil(ceilf(cy+dy), b->height);
    for (int sy = t0; sy < bo; sy++) {
        float py = (float)sy + 0.5f - cy;
        for (int sx = l; sx < r; sx++) {
            float px = (float)sx + 0.5f - cx;
            int tx = (int)floorf((px*ca + py*sa)/sc + t->w*0.5f);
            int ty = (int)floorf((-px*sa + py*ca)/sc + t->h*0.5f);
            if (tx < 0 || tx >= t->w || ty < 0 || ty >= t->h) continue;
            b->pixels[sy*b->stride + sx] = gfx_blend(b->pixels[sy*b->stride + sx], t->pixels[ty*t->w + tx]);
        }
    }
}

void gfx_draw_texture(Buffer *b, const GfxTexture *t, float x, float y, float a, float sc) {
    if (!b || !t || !t->pixels || !isfinite(x+y+a+sc) || sc <= 0) return;
    if (fabsf(a) < 0.0005f) { draw_tx_unrot(b, t, x, y, sc); return; }
    draw_tx_rot(b, t, x, y, a, sc);
}

/* Тонированная текстура: рисуется силуэт по альфа-каналу текстуры, залитый
 * цветом tint (0xAABBGGRR после упаковки). Нужно для теней — текстура
 * становится полностью чёрной, сохраняя форму персонажа. */
static void draw_tx_unrot_tint(Buffer *b, const GfxTexture *t, float x, float y, float sc, uint32_t tint) {
    if (!b || !t || !t->pixels || !isfinite(x+y+sc) || sc <= 0) return;
    float w = t->w*sc, h = t->h*sc;
    if (x >= b->width || y >= b->height || x+w <= 0 || y+h <= 0) return;
    int l = gfx_cl_floor(floorf(x), b->width), t0 = gfx_cl_floor(floorf(y), b->height);
    int r = gfx_cl_ceil(ceilf(x+w), b->width),  bo = gfx_cl_ceil(ceilf(y+h), b->height);
    uint32_t tr = tint & 0xff, tg = (tint >> 8) & 0xff, tb = (tint >> 16) & 0xff, ta = (tint >> 24) & 0xff;
    for (int sy = t0; sy < bo; sy++) {
        int src_y = (int)(((float)sy + 0.5f - y) / sc);
        if (src_y < 0) src_y = 0; if (src_y >= t->h) src_y = t->h - 1;
        for (int sx = l; sx < r; sx++) {
            int src_x = (int)(((float)sx + 0.5f - x) / sc);
            if (src_x < 0) src_x = 0; if (src_x >= t->w) src_x = t->w - 1;
            uint32_t p = t->pixels[src_y*t->w + src_x];
            uint32_t sa = (p >> 24) & 0xff;
            if (!sa) continue;
            uint32_t a = (sa * ta) / 255;
            if (!a) continue;
            uint32_t s = tr | (tg << 8) | (tb << 16) | (a << 24);
            b->pixels[sy*b->stride + sx] = gfx_blend(b->pixels[sy*b->stride + sx], s);
        }
    }
}

static void draw_tx_rot_tint(Buffer *b, const GfxTexture *t, float x, float y, float ang, float sc, uint32_t tint) {
    float hw = t->w*0.5f*sc, hh = t->h*0.5f*sc;
    float cx = x+hw, cy = y+hh, ca = cosf(ang), sa = sinf(ang);
    float dx = fabsf(hw*ca) + fabsf(hh*sa), dy = fabsf(hw*sa) + fabsf(hh*ca);
    int l = gfx_cl_floor(floorf(cx-dx), b->width), t0 = gfx_cl_floor(floorf(cy-dy), b->height);
    int r = gfx_cl_ceil(ceilf(cx+dx), b->width),   bo = gfx_cl_ceil(ceilf(cy+dy), b->height);
    uint32_t tr = tint & 0xff, tg = (tint >> 8) & 0xff, tb = (tint >> 16) & 0xff, ta = (tint >> 24) & 0xff;
    for (int sy = t0; sy < bo; sy++) {
        float py = (float)sy + 0.5f - cy;
        for (int sx = l; sx < r; sx++) {
            float px = (float)sx + 0.5f - cx;
            int tx = (int)floorf((px*ca + py*sa)/sc + t->w*0.5f);
            int ty = (int)floorf((-px*sa + py*ca)/sc + t->h*0.5f);
            if (tx < 0 || tx >= t->w || ty < 0 || ty >= t->h) continue;
            uint32_t p = t->pixels[ty*t->w + tx];
            uint32_t sa = (p >> 24) & 0xff;
            if (!sa) continue;
            uint32_t a = (sa * ta) / 255;
            if (!a) continue;
            uint32_t s = tr | (tg << 8) | (tb << 16) | (a << 24);
            b->pixels[sy*b->stride + sx] = gfx_blend(b->pixels[sy*b->stride + sx], s);
        }
    }
}

void gfx_draw_texture_tint(Buffer *b, const GfxTexture *t, float x, float y, float a, float sc, uint32_t tint) {
    if (!b || !t || !t->pixels || !isfinite(x+y+a+sc) || sc <= 0) return;
    if (fabsf(a) < 0.0005f) { draw_tx_unrot_tint(b, t, x, y, sc, tint); return; }
    draw_tx_rot_tint(b, t, x, y, a, sc, tint);
}

/* ── публичные команды (объявлены в runtime.h) ───────────────────────── */
void tex(float x, float y, const char *name, float a, float s) {
    if (!gfx_frame_is_open()) return;
    GfxTexture *t = gfx_texture_load(name); if (!t) return;
    GfxCmd *p = gfx_cmd_push(GFX_CMD_TEX); if (!p) return;
    p->v.tx.x=x; p->v.tx.y=y; p->v.tx.a=a; p->v.tx.sc=s; p->v.tx.tx=t;
}

void tex_tint(float x, float y, const char *name, float a, float s, uint32_t c) {
    if (!gfx_frame_is_open()) return;
    GfxTexture *t = gfx_texture_load(name); if (!t) return;
    GfxCmd *p = gfx_cmd_push(GFX_CMD_TEX_TINT); if (!p) return;
    p->v.tx2.x=x; p->v.tx2.y=y; p->v.tx2.a=a; p->v.tx2.sc=s; p->v.tx2.tx=t; p->v.tx2.c=gfx_pack(c);
}
