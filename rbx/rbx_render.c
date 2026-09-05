/* Софтверный 3D: перспективные кубы с z-буфером, отсечением и туманом. */
#include "rbx_render_internal.h"
#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NEAR_Z 0.08f
#define FAR_Z RBX_FAR_Z

typedef RbxVertex V3;

static Buffer *dst;
static uint32_t *pix;
/* В экранных координатах линейна 1/z, а не сама глубина z. */
static float *zbuf;
static int rw, rh, scale;
static size_t cap;
typedef struct { int lo, hi; uint32_t weight; } Sample;
static Sample *xsample;
static int sample_cap, sample_w, sample_rw;
static float camx, camy, camz;
static float yaw_s, yaw_c, pitch_s, pitch_c;
static float foc, view_x, view_y, side_x, side_y;
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
    /* Знаковые каналы: вычитание из uint32_t давало переполнение и радугу. */
    int fr = fog_rgb & 0xff, fg = (fog_rgb >> 8) & 0xff, fb = (fog_rgb >> 16) & 0xff;
    ir = (int)(ir + (fr - ir) * t);
    ig = (int)(ig + (fg - ig) * t);
    ib = (int)(ib + (fb - ib) * t);
    return (uint32_t)ir | ((uint32_t)ig << 8) | ((uint32_t)ib << 16) | 0xff000000u;
}

int rbx3d_begin(Buffer *b, int sc, float cx, float cy, float cz, float yaw, float pitch, float fov_deg) {
    dst = NULL;
    if (!b || !b->pixels || b->width <= 0 || b->height <= 0 || b->stride < b->width ||
        !isfinite(cx + cy + cz + yaw + pitch + fov_deg) || fov_deg < 5 || fov_deg > 175)
        return 0;
    scale = sc < 1 ? 1 : sc;
    rw = b->width / scale;
    rh = b->height / scale;
    if (rw < 8 || rh < 8 || rw > INT_MAX / rh) return 0;
    size_t need = (size_t)rw * rh;
    if (need > SIZE_MAX / sizeof(float) || need > SIZE_MAX / sizeof(uint32_t)) return 0;
    if (need > cap) {
        uint32_t *np = (uint32_t *)realloc(pix, need * sizeof(*pix));
        if (!np) return 0;
        pix = np; /* не оставляем висячий указатель при отказе второго realloc */
        float *nz = (float *)realloc(zbuf, need * sizeof(*zbuf));
        if (!nz) return 0;
        zbuf = nz;
        cap = need;
    }
    if (scale > 1 && b->width > sample_cap) {
        Sample *ns = (Sample *)realloc(xsample, (size_t)b->width * sizeof(*xsample));
        if (!ns) return 0;
        xsample = ns;
        sample_cap = b->width;
    }
    camx = cx; camy = cy; camz = cz;
    yaw_s = sinf(yaw); yaw_c = cosf(yaw);
    pitch_s = sinf(pitch); pitch_c = cosf(pitch);
    float fov = fov_deg * (float)M_PI / 180.0f;
    foc = (0.5f * (float)rh) / tanf(fov * 0.5f);
    view_x = 0.5f * rw / foc;
    view_y = 0.5f * rh / foc;
    side_x = sqrtf(1.0f + view_x * view_x);
    side_y = sqrtf(1.0f + view_y * view_y);
    dst = b;
    return 1;
}

void rbx3d_sky(uint32_t top, uint32_t bot) {
    if (!dst) return;
    uint32_t t = pack(top), b = pack(bot);
    fog_rgb = b;
    fog_a = RBX_FOG_START;
    fog_b = 1.0f / (RBX_FOG_END - RBX_FOG_START);
    int tr = t & 0xff, tg = (t >> 8) & 0xff, tb = (t >> 16) & 0xff;
    int br = b & 0xff, bg = (b >> 8) & 0xff, bb = (b >> 16) & 0xff;
    float horizon = rh * .5f + foc * pitch_s / fmaxf(.05f, pitch_c);
    for (int y = 0; y < rh; y++) {
        float k = fmaxf(0, fminf(1, 1 - (horizon - y) / (rh * .9f)));
        k = k * k * (3.0f - 2.0f * k);
        int r = (int)(tr + (br - tr) * k);
        int g = (int)(tg + (bg - tg) * k);
        int bl = (int)(tb + (bb - tb) * k);
        uint32_t c = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)bl << 16) | 0xff000000u;
        uint32_t *row = pix + y * rw;
        for (int x = 0; x < rw; x++) row[x] = c;
        float *zr = zbuf + y * rw;
        for (int x = 0; x < rw; x++) zr[x] = 1.0f / FAR_Z;
    }
}

static int to_view(float wx, float wy, float wz, V3 *o) {
    float dx = wx - camx, dy = wy - camy, dz = wz - camz;
    float rx = dx * yaw_c - dz * yaw_s;          /* right */
    float rz = dx * yaw_s + dz * yaw_c;          /* forward */
    float ry = dy;
    o->x = rx;
    o->u = o->v = 0;
    /* Положительный pitch смотрит вверх — как камера и вектор полёта. */
    o->y = ry * pitch_c - rz * pitch_s;
    o->z = ry * pitch_s + rz * pitch_c;
    return o->z > 0.01f;
}

static int project_v(V3 v, float *sx, float *sy) {
    if (v.z < 0.04f) return 0;
    float iz = foc / v.z;
    *sx = (float)rw * 0.5f + v.x * iz;
    *sy = (float)rh * 0.5f - v.y * iz;
    return 1;
}

int rbx3d_project(float x, float y, float z, float *sx, float *sy) {
    V3 v;
    if (!dst || !sx || !sy || !isfinite(x + y + z) ||
        !to_view(x, y, z, &v) || v.z < NEAR_Z || v.z > FAR_Z) return 0;
    float px, py;
    if (!project_v(v, &px, &py)) return 0;
    *sx = px * (float)dst->width / rw;
    *sy = py * (float)dst->height / rh;
    if (*sx < -80 || *sy < -80 || *sx > screen_w + 80 || *sy > screen_h + 80) return 0;
    return 1;
}

static V3 lerp3(V3 a, V3 b, float t) {
    V3 r;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t;
    r.u = a.u + (b.u - a.u) * t;
    r.v = a.v + (b.v - a.v) * t;
    return r;
}

/* Отсекаем до проекции: даже рядом с гранью не возникают огромные
 * экранные координаты и лишние полноэкранные треугольники. */
static float plane_distance(V3 v, int plane) {
    switch (plane) {
        case 0: return v.z - NEAR_Z;
        case 1: return FAR_Z - v.z;
        case 2: return v.z * view_x + v.x;
        case 3: return v.z * view_x - v.x;
        case 4: return v.z * view_y + v.y;
        default: return v.z * view_y - v.y;
    }
}

static int clip_plane(const V3 *in, int n, V3 *out, int plane) {
    int m = 0;
    V3 a = in[n - 1];
    float da = plane_distance(a, plane);
    for (int i = 0; i < n; i++) {
        V3 b = in[i];
        float db = plane_distance(b, plane);
        if ((da >= 0) != (db >= 0)) {
            V3 v = lerp3(a, b, da / (da - db));
            if (plane == 0) v.z = NEAR_Z;
            else if (plane == 1) v.z = FAR_Z;
            out[m++] = v;
        }
        if (db >= 0) out[m++] = b;
        a = b; da = db;
    }
    return m;
}

typedef struct { float x, y, iz, u, v; } ScreenV;
typedef struct {
    uint32_t color, palette[16];
    const unsigned char *texels;
    int size;
} Paint;

static ScreenV mix_vertex(ScreenV a, ScreenV b, float t) {
    ScreenV r = {a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.iz + (b.iz-a.iz)*t,
                 a.u + (b.u-a.u)*t, a.v + (b.v-a.v)*t};
    return r;
}
static void span(int y, ScreenV a, ScreenV b, const Paint *paint) {
    if (a.x > b.x) { ScreenV t = a; a = b; b = t; }
    int i0 = (int)fmaxf(0, ceilf(a.x - .5f));
    int i1 = (int)fminf((float)rw, ceilf(b.x - .5f));
    if (i0 >= i1) return;
    float inv = b.x - a.x > 1e-6f ? 1 / (b.x - a.x) : 0;
    float diz = (b.iz - a.iz) * inv, offset = i0 + .5f - a.x;
    float iz = a.iz + offset * diz;
    uint32_t *row = pix + y * rw;
    float *zr = zbuf + y * rw;
    if (!paint->size) {
        for (int x = i0; x < i1; x++, iz += diz) {
            if (iz > zr[x]) { zr[x] = iz; row[x] = paint->color; }
        }
        return;
    }
    float du = (b.u-a.u)*inv, dv = (b.v-a.v)*inv;
    float u = a.u + offset*du, v = a.v + offset*dv;
    int size = paint->size;
    for (int x = i0; x < i1; x++, iz += diz, u += du, v += dv) {
        if (iz <= zr[x]) continue;
        float depth = 1 / iz;
        int tx = (int)(u * depth), ty = (int)(v * depth);
        if (tx < 0) tx = 0;
        if (ty < 0) ty = 0;
        if (tx >= size) tx = size - 1;
        if (ty >= size) ty = size - 1;
        row[x] = paint->palette[paint->texels[ty * size + tx]];
        zr[x] = iz;
    }
}
static void fill_tri(ScreenV a, ScreenV b, ScreenV c, const Paint *paint) {
    if (a.y > b.y) { ScreenV t = a; a = b; b = t; }
    if (a.y > c.y) { ScreenV t = a; a = c; c = t; }
    if (b.y > c.y) { ScreenV t = b; b = c; c = t; }
    if (c.y - a.y < 1e-6f) return;
    int ys = (int)fmaxf(0, ceilf(a.y - .5f));
    int ye = (int)fminf((float)rh, ceilf(c.y - .5f));
    for (int y = ys; y < ye; y++) {
        float fy = y + .5f;
        ScreenV left = mix_vertex(a, c, (fy-a.y)/(c.y-a.y));
        ScreenV lo = fy < b.y ? a : b, hi = fy < b.y ? b : c;
        if (hi.y - lo.y < 1e-6f) continue;
        ScreenV right = mix_vertex(lo, hi, (fy-lo.y)/(hi.y-lo.y));
        span(y, left, right, paint);
    }
}

/* Оцениваем сжатую сторону грани, чтобы трава у горизонта не рябила. */
static int mip_size(const V3 *v, int n) {
    if (n != 4) return 16;
    float ax, ay, bx, by, dx, dy;
    if (v[0].z < NEAR_Z || v[1].z < NEAR_Z || v[3].z < NEAR_Z) return 16;
    if (!project_v(v[0], &ax, &ay) || !project_v(v[1], &bx, &by) || !project_v(v[3], &dx, &dy)) return 16;
    bx -= ax; by -= ay; dx -= ax; dy -= ay;
    float longest = fmaxf(sqrtf(bx*bx+by*by), sqrtf(dx*dx+dy*dy));
    float pixels = longest > 1e-6f ? fabsf(bx*dy-by*dx) / longest : 0;
    int size = 16;
    while (size > 1 && size > pixels) size /= 2;
    return size;
}

void rbx3d_polygon(const RbxVertex *w, int n, float nx, float ny, float nz,
                   uint32_t color, const RbxMaterial *material) {
    if (!dst || n < 3 || n > 8) return;
    if (nx*(camx-w[0].x) + ny*(camy-w[0].y) + nz*(camz-w[0].z) <= 0) return;
    V3 buffers[2][16], *in = buffers[0], *out = buffers[1];
    float wx = 0, wy = 0, wz = 0;
    for (int i = 0; i < n; i++) {
        to_view(w[i].x, w[i].y, w[i].z, &in[i]);
        in[i].u = w[i].u; in[i].v = w[i].v;
        wx += w[i].x; wy += w[i].y; wz += w[i].z;
    }
    wx = wx/n-camx; wy = wy/n-camy; wz = wz/n-camz;
    float distance = sqrtf(wx*wx + wy*wy + wz*wz);
    int size = material ? mip_size(in, n) : 0;
    for (int plane = 0; plane < 6; plane++) {
        n = clip_plane(in, n, out, plane);
        if (n < 3) return;
        V3 *swap = in; in = out; out = swap;
    }
    float shade = .52f + .5f * fmaxf(0, nx*.32f + ny*.88f + nz*.35f);
    Paint paint = {0};
    if (material && distance < RBX_FOG_END) {
        int offset = 0;
        for (int s = 16; s > size; s /= 2) offset += s*s;
        paint.texels = material->mip + offset;
        for (int i = 0; i < 16; i++) paint.palette[i] = shade_fog(pack(material->palette[i]), distance, shade);
        if (size > 1) paint.size = size;
        else paint.color = paint.palette[paint.texels[0]];
    } else paint.color = shade_fog(pack(color), distance, shade);
    ScreenV v[16];
    for (int i = 0; i < n; i++) {
        if (!project_v(in[i], &v[i].x, &v[i].y)) return;
        v[i].iz = 1 / in[i].z;
        v[i].u = in[i].u * size * v[i].iz;
        v[i].v = in[i].v * size * v[i].iz;
    }
    for (int i = 1; i < n-1; i++) fill_tri(v[0], v[i], v[i+1], &paint);
}

int rbx3d_visible(float x, float y, float z, float hx, float hy, float hz) {
    if (!dst || !isfinite(x+y+z+hx+hy+hz)) return 0;
    V3 center;
    to_view(x, y, z, &center);
    float radius = sqrtf(hx*hx + hy*hy + hz*hz);
    return center.z + radius >= NEAR_Z && center.z - radius <= FAR_Z &&
           fabsf(center.x) - center.z*view_x <= radius*side_x &&
           fabsf(center.y) - center.z*view_y <= radius*side_y;
}

/* Взвешенная сумма вместо беззнакового (b-a): каналы не переполняются.
 * R/B считаются вместе, G отдельно; веса 0..256 сохраняют диапазон RGB. */
static uint32_t mix_rgb(uint32_t a, uint32_t b, uint32_t t) {
    uint32_t inv = 256 - t;
    uint32_t rb = (((a & 0x00ff00ffu) * inv + (b & 0x00ff00ffu) * t + 0x00800080u) >> 8) & 0x00ff00ffu;
    uint32_t g = (((a & 0x0000ff00u) * inv + (b & 0x0000ff00u) * t + 0x00008000u) >> 8) & 0x0000ff00u;
    return rb | g | 0xff000000u;
}

static Sample sample_at(int pos, int source, int target) {
    float f = ((float)pos + 0.5f) * source / target - 0.5f;
    if (f < 0) f = 0;
    if (f > source - 1) f = (float)(source - 1);
    int lo = (int)f;
    Sample s = {lo, lo + 1 < source ? lo + 1 : lo, (uint32_t)((f - lo) * 256 + 0.5f)};
    return s;
}

/* Билинейный апскейл без дорогих float-операций на каждом пикселе.
 * Таблица X пересчитывается только при смене разрешения. */
static void upscale_smooth(void) {
    int W = dst->width, H = dst->height, st = dst->stride;
    if (sample_w != W || sample_rw != rw) {
        for (int x = 0; x < W; x++) xsample[x] = sample_at(x, rw, W);
        sample_w = W; sample_rw = rw;
    }
    for (int y = 0; y < H; y++) {
        Sample sy = sample_at(y, rh, H);
        const uint32_t *row0 = pix + sy.lo * rw, *row1 = pix + sy.hi * rw;
        uint32_t *out = dst->pixels + y * st;
        for (int x = 0; x < W; x++) {
            Sample sx = xsample[x];
            uint32_t top = mix_rgb(row0[sx.lo], row0[sx.hi], sx.weight);
            uint32_t bot = mix_rgb(row1[sx.lo], row1[sx.hi], sx.weight);
            out[x] = mix_rgb(top, bot, sy.weight);
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
