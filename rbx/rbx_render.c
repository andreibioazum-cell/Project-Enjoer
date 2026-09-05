/* Софтверный 3D: перспективные кубы с z-буфером, отсечением и туманом. */
#include "rbx_internal.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NEAR_Z 0.18f
#define FAR_Z  110.0f

typedef struct { float x, y, z; } V3;

static Buffer *dst;
static uint32_t *pix;
static float *zbuf;
static int rw, rh, scale, cap;
static float camx, camy, camz;
static float yaw_s, yaw_c, pitch_s, pitch_c;
static float foc;
static uint32_t fog_rgb;
static float fog_a, fog_b;

static uint32_t pack(uint32_t c) {
    uint32_t a = (c >> 24) & 0xff, r = (c >> 16) & 0xff, g = (c >> 8) & 0xff, b = c & 0xff;
    if (!a) a = 255;
    return r | (g << 8) | (b << 16) | (a << 24);
}

static uint32_t shade_fog(uint32_t packed, float z, float shade) {
    uint32_t r = packed & 0xff, g = (packed >> 8) & 0xff, b = (packed >> 16) & 0xff;
    int ir = (int)(r * shade), ig = (int)(g * shade), ib = (int)(b * shade);
    if (ir > 255) ir = 255;
    if (ig > 255) ig = 255;
    if (ib > 255) ib = 255;
    float t = (z - fog_a) * fog_b;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    uint32_t fr = fog_rgb & 0xff, fg = (fog_rgb >> 8) & 0xff, fb = (fog_rgb >> 16) & 0xff;
    ir = (int)(ir + (fr - ir) * t);
    ig = (int)(ig + (fg - ig) * t);
    ib = (int)(ib + (fb - ib) * t);
    return (uint32_t)ir | ((uint32_t)ig << 8) | ((uint32_t)ib << 16) | 0xff000000u;
}

int rbx3d_begin(Buffer *b, int sc, float cx, float cy, float cz, float yaw, float pitch, float fov_deg) {
    if (!b || !b->pixels || b->width <= 0 || b->height <= 0) return 0;
    dst = b;
    scale = sc < 1 ? 1 : sc;
    rw = b->width / scale;
    rh = b->height / scale;
    if (rw < 8 || rh < 8) return 0;
    int need = rw * rh;
    if (need > cap) {
        uint32_t *np = (uint32_t *)realloc(pix, (size_t)need * 4);
        float *nz = (float *)realloc(zbuf, (size_t)need * sizeof(float));
        if (!np || !nz) return 0;
        pix = np;
        zbuf = nz;
        cap = need;
    }
    camx = cx; camy = cy; camz = cz;
    yaw_s = sinf(yaw); yaw_c = cosf(yaw);
    pitch_s = sinf(pitch); pitch_c = cosf(pitch);
    float fov = fov_deg * (float)M_PI / 180.0f;
    foc = (0.5f * (float)rh) / tanf(fov * 0.5f);
    return 1;
}

void rbx3d_sky(uint32_t top, uint32_t bot) {
    if (!pix) return;
    uint32_t t = pack(top), b = pack(bot);
    fog_rgb = t;
    fog_a = 28.0f;
    fog_b = 1.0f / 62.0f;
    int tr = t & 0xff, tg = (t >> 8) & 0xff, tb = (t >> 16) & 0xff;
    int br = b & 0xff, bg = (b >> 8) & 0xff, bb = (b >> 16) & 0xff;
    for (int y = 0; y < rh; y++) {
        float u = (float)y / (float)(rh - 1);
        /* горизонт чуть ниже середины — типичный «роблоксовский» небосвод */
        float k = u < 0.42f ? u / 0.42f : 1.0f;
        k = k * k * (3.0f - 2.0f * k);
        int r = (int)(tr + (br - tr) * k);
        int g = (int)(tg + (bg - tg) * k);
        int bl = (int)(tb + (bb - tb) * k);
        uint32_t c = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)bl << 16) | 0xff000000u;
        uint32_t *row = pix + y * rw;
        for (int x = 0; x < rw; x++) row[x] = c;
        float *zr = zbuf + y * rw;
        for (int x = 0; x < rw; x++) zr[x] = FAR_Z;
    }
}

static int to_view(float wx, float wy, float wz, V3 *o) {
    float dx = wx - camx, dy = wy - camy, dz = wz - camz;
    float rx = dx * yaw_c - dz * yaw_s;          /* right */
    float rz = dx * yaw_s + dz * yaw_c;          /* forward */
    float ry = dy;
    o->x = rx;
    o->y = ry * pitch_c + rz * pitch_s;
    o->z = -ry * pitch_s + rz * pitch_c;
    return o->z > 0.01f;
}

static int project_v(V3 v, float *sx, float *sy) {
    if (v.z < 0.08f) return 0;
    float iz = foc / v.z;
    *sx = (float)rw * 0.5f + v.x * iz;
    *sy = (float)rh * 0.5f - v.y * iz;
    return 1;
}

int rbx3d_project(float x, float y, float z, float *sx, float *sy) {
    V3 v;
    if (!to_view(x, y, z, &v) || v.z < NEAR_Z) return 0;
    float px, py;
    if (!project_v(v, &px, &py)) return 0;
    *sx = px * (float)scale;
    *sy = py * (float)scale;
    if (*sx < -80 || *sy < -80 || *sx > screen_w + 80 || *sy > screen_h + 80) return 0;
    return 1;
}

static V3 lerp3(V3 a, V3 b, float t) {
    V3 r;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t;
    return r;
}

static int clip_near(V3 *in, int n, V3 *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        V3 a = in[i], b = in[(i + 1) % n];
        int ia = a.z >= NEAR_Z, ib = b.z >= NEAR_Z;
        if (ia) out[m++] = a;
        if (ia != ib) {
            float t = (NEAR_Z - a.z) / (b.z - a.z);
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            out[m++] = lerp3(a, b, t);
        }
    }
    return m;
}

static void span(int y, float x0, float z0, float x1, float z1, uint32_t c) {
    if (y < 0 || y >= rh) return;
    if (x0 > x1) {
        float tx = x0; x0 = x1; x1 = tx;
        float tz = z0; z0 = z1; z1 = tz;
    }
    int i0 = (int)ceilf(x0);
    int i1 = (int)ceilf(x1);
    if (i0 < 0) i0 = 0;
    if (i1 > rw) i1 = rw;
    if (i0 >= i1) return;
    float dx = x1 - x0;
    float dz = dx > 1e-4f ? (z1 - z0) / dx : 0;
    float z = z0 + ((float)i0 + 0.5f - x0) * dz;
    uint32_t *row = pix + y * rw;
    float *zr = zbuf + y * rw;
    for (int x = i0; x < i1; x++) {
        if (z < zr[x] && z > NEAR_Z) {
            zr[x] = z;
            row[x] = c;
        }
        z += dz;
    }
}

static void fill_tri(float x0, float y0, float z0,
                     float x1, float y1, float z1,
                     float x2, float y2, float z2, uint32_t c) {
    /* сортировка по y */
    if (y0 > y1) { float t; t=x0;x0=x1;x1=t; t=y0;y0=y1;y1=t; t=z0;z0=z1;z1=t; }
    if (y0 > y2) { float t; t=x0;x0=x2;x2=t; t=y0;y0=y2;y2=t; t=z0;z0=z2;z2=t; }
    if (y1 > y2) { float t; t=x1;x1=x2;x2=t; t=y1;y1=y2;y2=t; t=z1;z1=z2;z2=t; }
    if (y2 - y0 < 0.01f) return;
    int ys = (int)ceilf(y0);
    int ye = (int)ceilf(y2);
    if (ys < 0) ys = 0;
    if (ye > rh) ye = rh;
    for (int y = ys; y < ye; y++) {
        float fy = (float)y + 0.5f;
        float tA = (fy - y0) / (y2 - y0);
        float xA = x0 + (x2 - x0) * tA;
        float zA = z0 + (z2 - z0) * tA;
        float xB, zB;
        if (fy < y1) {
            if (y1 - y0 < 0.01f) continue;
            float tB = (fy - y0) / (y1 - y0);
            xB = x0 + (x1 - x0) * tB;
            zB = z0 + (z1 - z0) * tB;
        } else {
            if (y2 - y1 < 0.01f) continue;
            float tB = (fy - y1) / (y2 - y1);
            xB = x1 + (x2 - x1) * tB;
            zB = z1 + (z2 - z1) * tB;
        }
        span(y, xA, zA, xB, zB, c);
    }
}

static void draw_poly(V3 *w, int n, uint32_t packed, float shade) {
    if (n < 3) return;
    V3 view[8], clip[10];
    for (int i = 0; i < n; i++) {
        to_view(w[i].x, w[i].y, w[i].z, &view[i]);
        /* всё равно кладём — клип по near вырежет */
    }
    int cn = clip_near(view, n, clip);
    if (cn < 3) return;
    float sx[10], sy[10];
    for (int i = 0; i < cn; i++) {
        if (!project_v(clip[i], &sx[i], &sy[i])) return;
    }
    /* изнанку уже отсекли по нормали; здесь только вырожденные */
    float area = 0;
    for (int i = 0; i < cn; i++) {
        int j = (i + 1) % cn;
        area += sx[i] * sy[j] - sx[j] * sy[i];
    }
    if (area < 0.0f && area > -0.5f) return;
    if (area > 0.0f && area < 0.5f) return;
    for (int i = 1; i < cn - 1; i++) {
        float zm = (clip[0].z + clip[i].z + clip[i + 1].z) * (1.0f / 3.0f);
        uint32_t c = shade_fog(packed, zm, shade);
        fill_tri(sx[0], sy[0], clip[0].z, sx[i], sy[i], clip[i].z,
                 sx[i + 1], sy[i + 1], clip[i + 1].z, c);
    }
}

static void rot_y(float x, float z, float c, float s, float *ox, float *oz) {
    *ox = x * c + z * s;
    *oz = -x * s + z * c;
}

void rbx3d_box(float x, float y, float z, float hx, float hy, float hz, float yaw, uint32_t color) {
    if (!pix || hx <= 0 || hy <= 0 || hz <= 0) return;
    uint32_t packed = pack(color);
    float c = cosf(yaw), s = sinf(yaw);
    /* свет сверху-сбоку */
    const float lx = 0.32f, ly = 0.88f, lz = 0.35f;
    float hx_ = hx, hy_ = hy, hz_ = hz;
    /* +Y -Y +Z -Z +X -X */
    V3 loc[6][4] = {
        {{-hx_, hy_,-hz_},{-hx_, hy_, hz_},{ hx_, hy_, hz_},{ hx_, hy_,-hz_}},
        {{-hx_,-hy_,-hz_},{ hx_,-hy_,-hz_},{ hx_,-hy_, hz_},{-hx_,-hy_, hz_}},
        {{-hx_,-hy_, hz_},{ hx_,-hy_, hz_},{ hx_, hy_, hz_},{-hx_, hy_, hz_}},
        {{ hx_,-hy_,-hz_},{-hx_,-hy_,-hz_},{-hx_, hy_,-hz_},{ hx_, hy_,-hz_}},
        {{ hx_,-hy_,-hz_},{ hx_, hy_,-hz_},{ hx_, hy_, hz_},{ hx_,-hy_, hz_}},
        {{-hx_,-hy_,-hz_},{-hx_,-hy_, hz_},{-hx_, hy_, hz_},{-hx_, hy_,-hz_}},
    };
    float nn[6][3] = {{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{1,0,0},{-1,0,0}};
    for (int f = 0; f < 6; f++) {
        float nx = nn[f][0], ny = nn[f][1], nz = nn[f][2];
        float nwx, nwz;
        rot_y(nx, nz, c, s, &nwx, &nwz);
        float nd = nwx * lx + ny * ly + nwz * lz;
        if (nd < 0) nd = 0;
        float sh = 0.52f + 0.50f * nd;
        V3 w[4];
        for (int i = 0; i < 4; i++) {
            float wx, wz;
            rot_y(loc[f][i].x, loc[f][i].z, c, s, &wx, &wz);
            w[i].x = x + wx;
            w[i].y = y + loc[f][i].y;
            w[i].z = z + wz;
        }
        /* изнанка в мире */
        float cx = camx - w[0].x, cy = camy - w[0].y, czv = camz - w[0].z;
        if (nwx * cx + ny * cy + nwz * czv <= 0.0f) continue;
        draw_poly(w, 4, packed, sh);
    }
}

/* Плавный (билинейный) апскейл внутреннего буфера в буфер кадра.
 * Раньше пиксели просто дублировались блоками 2x2/3x3 — из-за этого вся
 * картинка выглядела «квадратиками». Теперь, когда 3D рендерится в
 * половинном разрешении (большие экраны), апскейл сглаженный. */
static void upscale_smooth(void) {
    int W = dst->width, H = dst->height, st = dst->stride;
    float inv = 1.0f / (float)scale;
    for (int y = 0; y < H; y++) {
        float fy = ((float)y + 0.5f) * inv - 0.5f;
        int y0 = (int)floorf(fy);
        float ty = fy - (float)y0;
        if (y0 < 0) { y0 = 0; ty = 0; }
        int y1 = y0 + 1;
        if (y0 >= rh) y0 = rh - 1;
        if (y1 >= rh) y1 = rh - 1;
        const uint32_t *row0 = pix + y0 * rw;
        const uint32_t *row1 = pix + y1 * rw;
        uint32_t *out = dst->pixels + y * st;
        for (int x = 0; x < W; x++) {
            float fx = ((float)x + 0.5f) * inv - 0.5f;
            int x0 = (int)floorf(fx);
            float tx = fx - (float)x0;
            if (x0 < 0) { x0 = 0; tx = 0; }
            int x1 = x0 + 1;
            if (x0 >= rw) x0 = rw - 1;
            if (x1 >= rw) x1 = rw - 1;
            uint32_t c00 = row0[x0], c10 = row0[x1];
            uint32_t c01 = row1[x0], c11 = row1[x1];
            uint32_t r0 = c00 & 0xff, g0 = (c00 >> 8) & 0xff, b0 = (c00 >> 16) & 0xff;
            uint32_t r1 = c10 & 0xff, g1 = (c10 >> 8) & 0xff, b1 = (c10 >> 16) & 0xff;
            uint32_t r2 = c01 & 0xff, g2 = (c01 >> 8) & 0xff, b2 = (c01 >> 16) & 0xff;
            uint32_t r3 = c11 & 0xff, g3 = (c11 >> 8) & 0xff, b3 = (c11 >> 16) & 0xff;
            float top_r = r0 + (r1 - r0) * tx, top_g = g0 + (g1 - g0) * tx, top_b = b0 + (b1 - b0) * tx;
            float bot_r = r2 + (r3 - r2) * tx, bot_g = g2 + (g3 - g2) * tx, bot_b = b2 + (b3 - b2) * tx;
            uint32_t r = (uint32_t)(top_r + (bot_r - top_r) * ty + 0.5f);
            uint32_t g = (uint32_t)(top_g + (bot_g - top_g) * ty + 0.5f);
            uint32_t bl = (uint32_t)(top_b + (bot_b - top_b) * ty + 0.5f);
            out[x] = r | (g << 8) | (bl << 16) | 0xff000000u;
        }
    }
}

void rbx3d_end(void) {
    if (!dst || !pix) return;
    int H = dst->height, st = dst->stride;
    if (scale == 1) {
        for (int y = 0; y < rh && y < H; y++)
            memcpy(dst->pixels + y * st, pix + y * rw, (size_t)rw * 4);
        return;
    }
    upscale_smooth();
}
