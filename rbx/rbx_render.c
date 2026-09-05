/* Софтверный 3D: перспективные кубы с z-буфером, отсечением и туманом. */
#include "rbx_render_internal.h"
#include <math.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <pthread.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define NEAR_Z 0.08f
#define FAR_Z RBX_FAR_Z

typedef RbxVertex V3;

static Buffer *dst;
static uint32_t *pix;
/* В экранных координатах линейна 1/z, а не сама глубина z. */
static float *zbuf, *ray_length;
static int ray_w, ray_h;
static float ray_foc;
static int rw, rh, scale;
static size_t cap;
typedef struct { int lo, hi; uint32_t weight; } Sample;
static Sample *xsample;
static uint32_t *horizontal_rows;
static int horizontal_cap;
static int sample_cap, sample_w, sample_rw;
static float camx, camy, camz;
static float yaw_s, yaw_c, pitch_s, pitch_c;
static float foc, view_x, view_y, side_x, side_y;
static uint32_t fog_rgb;
static float fog_a, fog_b;
/* Строки матрицы «мир → камера» и их модули: AABB-отсечение без sqrt. */
static float mrx, mry, mrz, mux, muy, muz, mfx, mfy, mfz;
static float arx, ary, arz, aux, auy, auz, afx, afy, afz;
/* Граница тумана в мировых единицах: дальше пиксель неотличим от неба. */
static float cull_far = RBX_FAR_Z;
/* Уровень тумана пикселя: (int)(distance*fog_gain+fog_offset). */
static float fog_gain, fog_offset;
/* Целочисленный масштаб: веса и смещения повторяются с периодом scale. */
static int up_scale;
static signed char up_off[8];
static uint32_t up_w[8];

typedef struct { float x, y, iz, u, v; } ScreenV;
typedef struct {
    uint32_t color;
    const uint32_t *palette;
    const unsigned char *texels;
    float plane;
    int fog;
} Paint;

/* Кадр рисуется полосами экрана: треугольники копятся в список, подготовка
 * (преобразование, отсечение, проекция, палитра) делается один раз в главном
 * потоке, а растеризация распределяется по потокам. Полосы не пересекаются,
 * порядок треугольников внутри полосы сохраняется, поэтому изображение
 * побитово совпадает с однопоточным. */
enum { RBX_MAX_PARTS=4, RBX_TRI_START=2048, RBX_TRI_LIMIT=32768 };
typedef struct { ScreenV a,b,c; Paint paint; int y0,y1; } Tri;
static Tri *tri_queue;
static int tri_count,tri_cap,tri_failed;
static int thread_parts;          /* сколько полос одновременно, 0 = не задано */
static int band_parts=1;          /* общее число полос текущей работы */
static int sky_horizon;           /* строка горизонта: над ней работы меньше */

static pthread_t pool_threads[RBX_MAX_PARTS];
static pthread_mutex_t pool_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pool_start_cv=PTHREAD_COND_INITIALIZER;
static pthread_cond_t pool_done_cv=PTHREAD_COND_INITIALIZER;
static int pool_workers,pool_serial,pool_finished,pool_quit,pool_failed;
static int pool_parts;
static void (*pool_fn)(int,void *);
static void *pool_ctx;

/* Сколько треугольников принял рендер в текущем кадре: метрика отсечения. */
static int frame_tris;
int rbx3d_frame_triangles(void) { return frame_tris; }
int rbx3d_threads(void) { return thread_parts; }
void rbx3d_set_threads(int parts) {
    if (parts<1) parts=1;
    if (parts>RBX_MAX_PARTS) parts=RBX_MAX_PARTS;
    thread_parts=parts;
}
/* Полос больше, чем ядер: растеризация упирается в память и ветвления,
 * поэтому небольшой переподпиской ядра загружаются ровнее. */
static int detect_parts(void) {
    long cores=sysconf(_SC_NPROCESSORS_ONLN);
    if (cores<2) return 1;
    return RBX_MAX_PARTS;
}
/* Мелким кадрам синхронизация потоков дороже выигрыша. */
static int parts_for(int64_t work,int64_t minimum) {
    int parts=thread_parts>0?thread_parts:1;
    if (parts>RBX_MAX_PARTS) parts=RBX_MAX_PARTS;
    if (work<minimum) parts=1;
    return parts;
}
static int worker_seen[RBX_MAX_PARTS];  /* серийный номер, с которого поток ждёт */
static void *pool_worker(void *arg) {
    int index=(int)(intptr_t)arg;
    int seen=worker_seen[index];      /* задан создателем: не повторять старое */
    pthread_mutex_lock(&pool_mutex);
    for (;;) {
        while (pool_serial==seen&&!pool_quit) pthread_cond_wait(&pool_start_cv,&pool_mutex);
        if (pool_quit) break;
        seen=pool_serial;
        pthread_mutex_unlock(&pool_mutex);
        for (int part=index+1;part<pool_parts;part+=pool_workers+1) pool_fn(part,pool_ctx);
        pthread_mutex_lock(&pool_mutex);
        pool_finished++;
        pthread_cond_signal(&pool_done_cv);
    }
    pthread_mutex_unlock(&pool_mutex);
    return NULL;
}
static void pool_create(int workers) {
    while (pool_workers<workers) {
        pthread_t thread;
        worker_seen[pool_workers]=pool_serial;   /* поток ждёт следующую серию */
        if (pthread_create(&thread,NULL,pool_worker,(void *)(intptr_t)pool_workers)!=0) {
            pool_failed=1;
            break;
        }
        pool_threads[pool_workers]=thread;
        pool_workers++;
    }
}
/* part 0 выполняет вызывающий поток, остальные — рабочие. */
static void rbx_jobs(int parts,void (*fn)(int,void *),void *ctx) {
    if (parts<1) parts=1;
    if (parts==1) { fn(0,ctx); return; }
    if (!pool_workers&&!pool_failed) pool_create(parts-1);
    if (!pool_workers) { for (int part=0;part<parts;part++) fn(part,ctx); return; }
    pool_fn=fn;pool_ctx=ctx;pool_parts=parts;
    pthread_mutex_lock(&pool_mutex);
    pool_finished=0;
    pool_serial++;
    pthread_cond_broadcast(&pool_start_cv);
    pthread_mutex_unlock(&pool_mutex);
    for (int part=0;part<parts;part+=pool_workers+1) fn(part,ctx);
    pthread_mutex_lock(&pool_mutex);
    while (pool_finished<pool_workers) pthread_cond_wait(&pool_done_cv,&pool_mutex);
    pthread_mutex_unlock(&pool_mutex);
}
void rbx3d_shutdown(void) {
    if (!pool_workers) return;
    pthread_mutex_lock(&pool_mutex);
    pool_quit=1;
    pthread_cond_broadcast(&pool_start_cv);
    pthread_mutex_unlock(&pool_mutex);
    for (int i=0;i<pool_workers;i++) pthread_join(pool_threads[i],NULL);
    pool_workers=0;
}
/* Равные полосы для работ с одинаковой стоимостью строки; total — число строк
 * того буфера, который пишем (кадр, исходник апскейла или целевой буфер). */
static void equal_bounds(int total,int parts,int *lo,int *hi) {
    int previous=0;
    for (int part=0;part<parts;part++) {
        lo[part]=previous;
        int y=part==parts-1?total:(int)((int64_t)total*(part+1)/parts);
        if (y<=previous) y=previous+1;
        if (y>total) y=total;
        hi[part]=y;
        previous=y;
    }
}
/* Границы полос: над горизонтом строка стоит дешевле (там в основном небо). */
static void band_bounds(int parts,int *lo,int *hi) {
    int horizon=sky_horizon<0?0:sky_horizon>rh?rh:sky_horizon;
    const float sky_weight=.3f;
    float total=horizon*sky_weight+(float)(rh-horizon),step=total/parts;
    int previous=0;
    for (int part=0;part<parts;part++) {
        lo[part]=previous;
        if (part==parts-1) { hi[part]=rh; break; }
        float target=step*(float)(part+1);
        int y=target<=horizon*sky_weight ? (int)(target/sky_weight)
                                         : horizon+(int)(target-horizon*sky_weight);
        if (y<=previous) y=previous+1;
        if (y>rh) y=rh;
        hi[part]=y;
        previous=y;
    }
}

static void fog_constants(void) {
    fog_gain = fog_b * (FOG_LEVELS - 1);
    fog_offset = .5f - fog_a * fog_gain;
    cull_far = fog_a + (fog_b > 0 ? 1.0f / fog_b : 0.0f);
    if (!isfinite(cull_far) || cull_far < 0) cull_far = RBX_FAR_Z;
}

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
        float *nr = realloc(ray_length,need*sizeof(*nr));
        if (!nr) return 0;
        ray_length=nr;
        cap = need;
    }
    if (scale>1 && ((size_t)b->width>SIZE_MAX/sizeof(Sample) || (size_t)b->width>SIZE_MAX/sizeof(uint32_t)/2)) return 0;
    if (scale>1 && b->width>horizontal_cap) {
        /* По две строки на полосу: потоки не разделяют промежуточный буфер. */
        uint32_t *rows=realloc(horizontal_rows,(size_t)b->width*2*RBX_MAX_PARTS*sizeof(*rows));
        if(!rows)return 0;
        horizontal_rows=rows;horizontal_cap=b->width;
    }
    if (scale > 1 && b->width > sample_cap) {
        Sample *ns = (Sample *)realloc(xsample, (size_t)b->width * sizeof(*xsample));
        if (!ns) return 0;
        xsample = ns;
        sample_cap = b->width;
    }
    if (!thread_parts) thread_parts=detect_parts();
    tri_count=0;
    frame_tris=0;                        /* оборванный кадр не переносится в следующий */
    camx = cx; camy = cy; camz = cz;
    yaw_s = sinf(yaw); yaw_c = cosf(yaw);
    pitch_s = sinf(pitch); pitch_c = cosf(pitch);
    /* Те же базисные векторы, что и в to_view: right, up', forward. */
    mrx = yaw_c;            mry = 0;        mrz = -yaw_s;
    mux = -yaw_s * pitch_s; muy = pitch_c;  muz = -yaw_c * pitch_s;
    mfx = yaw_s * pitch_c;  mfy = pitch_s;  mfz = yaw_c * pitch_c;
    arx = fabsf(mrx); ary = fabsf(mry); arz = fabsf(mrz);
    aux = fabsf(mux); auy = fabsf(muy); auz = fabsf(muz);
    afx = fabsf(mfx); afy = fabsf(mfy); afz = fabsf(mfz);
    float fov = fov_deg * (float)M_PI / 180.0f;
    foc = (0.5f * (float)rh) / tanf(fov * 0.5f);
    view_x = 0.5f * rw / foc;
    view_y = 0.5f * rh / foc;
    side_x = sqrtf(1.0f + view_x * view_x);
    side_y = sqrtf(1.0f + view_y * view_y);
    if (ray_w!=rw || ray_h!=rh || ray_foc!=foc) {
        for (int y=0;y<rh;y++) for (int x=0;x<rw;x++) {
            float dx=(x+.5f-rw*.5f)/foc,dy=(y+.5f-rh*.5f)/foc;
            ray_length[y*rw+x]=sqrtf(1+dx*dx+dy*dy);
        }
        ray_w=rw;ray_h=rh;ray_foc=foc;
    }
    dst = b;
    return 1;
}

void rbx3d_fog(float start,float end) {
    if (!isfinite(start+end) || start<0 || end<=start) return;
    fog_a=start;fog_b=1/(end-start);
    fog_constants();
}
static int sky_tr,sky_tg,sky_tb,sky_br,sky_bg,sky_bb;
static float sky_gradient,sky_line;
static void sky_rows(int y0,int y1) {
    float horizon=sky_line;
    for (int y=y0;y<y1;y++) {
        float k=fmaxf(0,fminf(1,1-(horizon-y)/sky_gradient));
        k=k*k*(3.0f-2.0f*k);
        int r=(int)(sky_tr+(sky_br-sky_tr)*k);
        int g=(int)(sky_tg+(sky_bg-sky_tg)*k);
        int bl=(int)(sky_tb+(sky_bb-sky_tb)*k);
        uint32_t c=(uint32_t)r|((uint32_t)g<<8)|((uint32_t)bl<<16)|0xff000000u;
        uint32_t * restrict row=pix+(size_t)y*rw;
        for (int x=0;x<rw;x++) row[x]=c;
        float * restrict zr=zbuf+(size_t)y*rw;
        for (int x=0;x<rw;x++) zr[x]=1.0f/FAR_Z;
    }
}
static void sky_job(int part,void *ctx) {
    int lo[RBX_MAX_PARTS],hi[RBX_MAX_PARTS];
    (void)ctx;
    equal_bounds(rh,band_parts,lo,hi);
    sky_rows(lo[part],hi[part]);
}
void rbx3d_sky(uint32_t top, uint32_t bot) {
    if (!dst) return;
    uint32_t t = pack(top), b = pack(bot);
    fog_rgb = b;
    fog_a = RBX_FOG_START;
    fog_b = 1.0f / (RBX_FOG_END - RBX_FOG_START);
    fog_constants();
    int tr = t & 0xff, tg = (t >> 8) & 0xff, tb = (t >> 16) & 0xff;
    int br = b & 0xff, bg = (b >> 8) & 0xff, bb = (b >> 16) & 0xff;
    float horizon = rh * .5f + foc * pitch_s / fmaxf(.05f, pitch_c);
    sky_tr = tr; sky_tg = tg; sky_tb = tb;
    sky_br = br; sky_bg = bg; sky_bb = bb;
    sky_gradient = (float)(rh * .9);
    sky_line = horizon;
    sky_horizon = (int)horizon;
    if (sky_horizon < 0) sky_horizon = 0;
    if (sky_horizon > rh) sky_horizon = rh;
    int parts = parts_for((int64_t)rw * rh, 150000);
    if (parts > rh) parts = rh;
    band_parts = parts;
    if (parts > 1) rbx_jobs(parts, sky_job, NULL);
    else sky_rows(0, rh);
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


static ScreenV mix_vertex(ScreenV a, ScreenV b, float t) {
    ScreenV r = {a.x + (b.x-a.x)*t, a.y + (b.y-a.y)*t, a.iz + (b.iz-a.iz)*t,
                 a.u + (b.u-a.u)*t, a.v + (b.v-a.v)*t};
    return r;
}
static void span(int y, ScreenV a, ScreenV b, const Paint *paint) {
    if (a.x>b.x) {ScreenV t=a;a=b;b=t;}
    int i0=(int)fmaxf(0,ceilf(a.x-.5f)),i1=(int)fminf((float)rw,ceilf(b.x-.5f));
    if (i0>=i1) return;
    float inv=b.x-a.x>1e-6f ? 1/(b.x-a.x) : 0;
    float diz=(b.iz-a.iz)*inv,offset=i0+.5f-a.x,iz=a.iz+offset*diz;
    uint32_t * restrict row=pix+y*rw;
    float * restrict zr=zbuf+y*rw;
    if (!paint->texels) {
        uint32_t color=paint->color;
        for (int x=i0;x<i1;x++,iz+=diz) if (iz>zr[x]) {zr[x]=iz;row[x]=color;}
        return;
    }
    float du=(b.u-a.u)*inv,dv=(b.v-a.v)*inv,u=a.u+offset*du,v=a.v+offset*dv;
    const float *ray=ray_length+y*rw;
    static const int offsets[6]={1364,1360,1344,1280,1024,0};
    /* Поля Paint копируются в локальные: запись в кадр не заставляет
     * компилятор перечитывать их из памяти на каждом пикселе. */
    const uint32_t * restrict palette=paint->palette;
    const unsigned char * restrict texels_base=paint->texels;
    const float plane=paint->plane;
    const int fog=paint->fog;
    const float gain=fog_gain,bias=fog_offset;
    /* Мелкий шаг LOD (8 вместо 16) — меньше блочной пикселизации вдали. */
    for (int start=i0;start<i1;start+=8) {
        int end=start+8<i1 ? start+8 : i1;
        float mid=iz+diz*(end-start-1)*.5f;
        float pixels=foc*mid*fminf(1,plane*mid);
        union {float f;uint32_t u;} bits={pixels};
        int exponent=(int)((bits.u>>23)&255)-127;
        if (exponent<0) exponent=0;
        if (exponent>5) exponent=5;
        int shift=5-exponent;
        const unsigned char *texels=texels_base+offsets[exponent];
        if (fog) {
            /* Последний ряд палитры — ровно цвет тумана для любого текселя,
             * поэтому вдали текстуру можно не читать вовсе. */
            const uint32_t haze=palette[(FOG_LEVELS-1)*PALETTE_SIZE];
            for (int x=start;x<end;x++,iz+=diz,u+=du,v+=dv) {
                if (iz<=zr[x]) continue;
                float depth=1/iz;
                int level=(int)(depth*ray[x]*gain+bias);
                if (level>=FOG_LEVELS-1) {row[x]=haze;zr[x]=iz;continue;}
                if (level<0) level=0;
                /* floor + power-of-two wrap also works for negative/world UV. */
                float fu=u*depth,fv=v*depth;
                int iu=(int)fu,iv=(int)fv;
                iu-=fu<iu;iv-=fv<iv; /* bounded floor without generic libm overhead */
                int tx=(iu&31)>>shift,ty=(iv&31)>>shift;
                row[x]=palette[texels[(ty<<exponent)+tx]+level*PALETTE_SIZE];zr[x]=iz;
            }
        } else {
            for (int x=start;x<end;x++,iz+=diz,u+=du,v+=dv) {
                if (iz<=zr[x]) continue;
                float depth=1/iz;
                float fu=u*depth,fv=v*depth;
                int iu=(int)fu,iv=(int)fv;
                iu-=fu<iu;iv-=fv<iv;
                int tx=(iu&31)>>shift,ty=(iv&31)>>shift;
                row[x]=palette[texels[(ty<<exponent)+tx]];zr[x]=iz;
            }
        }
    }
}
static void fill_tri(ScreenV a, ScreenV b, ScreenV c, const Paint *paint, int lo, int hi) {
    if (a.y > b.y) { ScreenV t = a; a = b; b = t; }
    if (a.y > c.y) { ScreenV t = a; a = c; c = t; }
    if (b.y > c.y) { ScreenV t = b; b = c; c = t; }
    if (c.y - a.y < 1e-6f) return;
    int ys = (int)fmaxf(lo, ceilf(a.y - .5f));
    int ye = (int)fminf((float)hi, ceilf(c.y - .5f));
    /* Обратные высоты сторон считаем один раз на треугольник, не на строку. */
    float inv_ac = 1.0f / (c.y - a.y);
    float inv_ab = b.y - a.y > 1e-6f ? 1.0f / (b.y - a.y) : 0;
    float inv_bc = c.y - b.y > 1e-6f ? 1.0f / (c.y - b.y) : 0;
    for (int y = ys; y < ye; y++) {
        float fy = y + .5f;
        ScreenV left = mix_vertex(a, c, (fy-a.y)*inv_ac);
        int lower = fy < b.y;
        ScreenV lo = lower ? a : b, hi = lower ? b : c;
        if (hi.y - lo.y < 1e-6f) continue;
        ScreenV right = mix_vertex(lo, hi, (fy-lo.y)*(lower ? inv_ab : inv_bc));
        span(y, left, right, paint);
    }
}

/* Список треугольников кадра: растеризация идёт полосами в нескольких потоках. */
static void draw_triangles(int part) {
    int lo[RBX_MAX_PARTS],hi[RBX_MAX_PARTS];
    band_bounds(band_parts,lo,hi);      /* границы одинаковы у всех потоков */
    int top=lo[part],bottom=hi[part];
    for (int i=0;i<tri_count;i++) {
        const Tri *t=&tri_queue[i];
        if (t->y1<=top||t->y0>=bottom) continue;
        fill_tri(t->a,t->b,t->c,&t->paint,top,bottom);
    }
}
static void draw_job(int part,void *ctx) { (void)ctx;draw_triangles(part); }
static void flush_triangles(void) {
    if (!tri_count) return;
    int parts=parts_for((int64_t)rw*rh,100000);
    if (parts>rh) parts=rh;
    band_parts=parts;
    if (parts>1) rbx_jobs(parts,draw_job,NULL);
    else draw_triangles(0);
    tri_count=0;
}
static void queue_tri(ScreenV a,ScreenV b,ScreenV c,const Paint *paint) {
    if (tri_failed) { fill_tri(a,b,c,paint,0,rh); return; }
    float ymin=fminf(a.y,fminf(b.y,c.y)),ymax=fmaxf(a.y,fmaxf(b.y,c.y));
    int y0=(int)fmaxf(0,ceilf(ymin-.5f)),y1=(int)fminf((float)rh,ceilf(ymax-.5f));
    if (y0>=y1) return;                                  /* вне экрана по Y */
    if (tri_count==tri_cap) {
        if (tri_cap>=RBX_TRI_LIMIT) { flush_triangles(); }  /* порядок сохранён */
        else {
            int capacity=tri_cap?tri_cap*2:RBX_TRI_START;
            Tri *grown=realloc(tri_queue,(size_t)capacity*sizeof(*grown));
            if (!grown) { tri_failed=1;fill_tri(a,b,c,paint,0,rh);return; }
            tri_queue=grown;tri_cap=capacity;
        }
    }
    tri_queue[tri_count++]=(Tri){a,b,c,*paint,y0,y1};
}

void rbx3d_polygon(const RbxVertex *w, int n, float nx, float ny, float nz,
                   uint32_t color, RbxMaterial *material) {
    if (!dst || n < 3 || n > 8) return;
    frame_tris+=n-2;                   /* считаем всё, что прислал мир */
    float plane=nx*(camx-w[0].x)+ny*(camy-w[0].y)+nz*(camz-w[0].z);
    if (plane<=0) return;
    V3 buffers[2][16], *in = buffers[0], *out = buffers[1];
    float wx = 0, wy = 0, wz = 0, max_distance2 = 0;
    for (int i = 0; i < n; i++) {
        to_view(w[i].x, w[i].y, w[i].z, &in[i]);
        in[i].u = w[i].u; in[i].v = w[i].v;
        if (material) {
            float dx=w[i].x-camx,dy=w[i].y-camy,dz=w[i].z-camz;
            max_distance2=fmaxf(max_distance2,dx*dx+dy*dy+dz*dz);
        } else { wx += w[i].x; wy += w[i].y; wz += w[i].z; }
    }
    /* Одна классификация по шести плоскостям вместо шести проходов отсечения:
     * полностью видимая грань (обычный случай) не копирует вершины. */
    float dmin[6];
    int inside = 1;
    for (int p = 0; p < 6; p++) {
        float lo = plane_distance(in[0], p), hi = lo;
        for (int i = 1; i < n; i++) {
            float d = plane_distance(in[i], p);
            if (d < lo) lo = d;
            if (d > hi) hi = d;
        }
        dmin[p] = lo;
        if (hi < 0) return;              /* целиком за плоскостью */
        if (lo < 0) inside = 0;
    }
    if (!inside) {
        for (int p = 0; p < 6; p++) {
            if (dmin[p] >= 0) continue;  /* эта плоскость ничего не режет */
            n = clip_plane(in, n, out, p);
            if (n < 3) return;
            V3 *swap = in; in = out; out = swap;
        }
    }
    float shade = .52f + .5f * fmaxf(0, nx*.32f + ny*.88f + nz*.35f);
    Paint paint = {0};
    if (material) {
        int face=ny>.5f ? 0 : ny<-.5f ? 1 : nz>.5f ? 2 : nz<-.5f ? 3 : nx>.5f ? 4 : 5;
        paint.palette=rbx_material_shades(material,face,fog_rgb);
        paint.texels=material->mip;paint.plane=plane;
        paint.fog=max_distance2>fog_a*fog_a;
    } else {
        wx = wx/n-camx; wy = wy/n-camy; wz = wz/n-camz;
        paint.color=shade_fog(pack(color),sqrtf(wx*wx + wy*wy + wz*wz),shade);
    }
    ScreenV v[16];
    for (int i = 0; i < n; i++) {
        if (!project_v(in[i], &v[i].x, &v[i].y)) return;
        v[i].iz = 1 / in[i].z;
        v[i].u = in[i].u * TEXTURE_SIZE * v[i].iz;
        v[i].v = in[i].v * TEXTURE_SIZE * v[i].iz;
    }
    if (n>4) {
        /* Редкий случай: сначала рисуем уже накопленное, чтобы не менять порядок. */
        flush_triangles();
        for (int i=1;i<n-1;i++) fill_tri(v[0],v[i],v[i+1],&paint,0,rh);
    } else {
        for (int i=1;i<n-1;i++) queue_tri(v[0],v[i],v[i+1],&paint);
    }
}

int rbx3d_visible(float x, float y, float z, float hx, float hy, float hz) {
    if (!dst || !isfinite(x+y+z+hx+hy+hz)) return 0;
    V3 center;
    to_view(x, y, z, &center);
    float radius = sqrtf(hx*hx + hy*hy + hz*hz);
    float far=fog_a+1/fog_b;
    if (sqrtf(center.x*center.x+center.y*center.y+center.z*center.z)-radius>far) return 0;
    return center.z + radius >= NEAR_Z && center.z - radius <= FAR_Z &&
           fabsf(center.x) - center.z*view_x <= radius*side_x &&
           fabsf(center.y) - center.z*view_y <= radius*side_y;
}

/* Быстрое отсечение ориентированного по осям параллелепипеда: центр
 * переводится в систему камеры один раз, радиусы — без sqrt. */
int rbx3d_box_visible(float cx, float cy, float cz, float hx, float hy, float hz) {
    if (!dst || !isfinite(cx+cy+cz+hx+hy+hz)) return 0;
    float dx=cx-camx, dy=cy-camy, dz=cz-camz;
    float radius=hx+hy+hz;                    /* L1 ≥ L2: отсечение остаётся безопасным */
    float limit=cull_far+radius;
    if (dx*dx+dy*dy+dz*dz > limit*limit) return 0; /* дальше тумана пиксель неотличим от неба */
    float vx=mrx*dx+mry*dy+mrz*dz;
    float vy=mux*dx+muy*dy+muz*dz;
    float vz=mfx*dx+mfy*dy+mfz*dz;
    float ex=arx*hx+ary*hy+arz*hz;
    float ey=aux*hx+auy*hy+auz*hz;
    float ez=afx*hx+afy*hy+afz*hz;
    if (vz+ez < NEAR_Z || vz-ez > FAR_Z) return 0;
    float side=vz*view_x, up=vz*view_y;
    if (vx+side+ex+ez*view_x < 0) return 0;
    if (ez*view_x-vx+side+ex < 0) return 0;
    if (vy+up+ey+ez*view_y < 0) return 0;
    if (ez*view_y-vy+up+ey < 0) return 0;
    return 1;
}

/* Отсечение квада воксельной грани до постройки вершин: задняя сторона
 * отсекается одним сравнением координаты камеры с плоскостью грани. */
int rbx3d_quad_visible(int sx, int sy, int sz, int u, int v, int face) {
    if (!dst || face < 0 || face >= 6 || u <= 0 || v <= 0) return 0;
    float x0=sx*.5f, y0=sy*.5f, z0=sz*.5f, hx, hy, hz;
    switch (face) {
        case 0: if (camy <= y0) return 0; hx=u*.25f; hy=.02f;  hz=v*.25f; break;
        case 1: if (camy >= y0) return 0; hx=u*.25f; hy=.02f;  hz=v*.25f; break;
        case 2: if (camz <= z0) return 0; hx=u*.25f; hy=v*.25f; hz=.02f;  break;
        case 3: if (camz >= z0) return 0; hx=u*.25f; hy=v*.25f; hz=.02f;  break;
        case 4: if (camx <= x0) return 0; hx=.02f;  hy=v*.25f; hz=u*.25f; break;
        default: if (camx >= x0) return 0; hx=.02f; hy=v*.25f; hz=u*.25f; break;
    }
    return rbx3d_box_visible(x0+hx, y0+hy, z0+hz, hx, hy, hz);
}

void rbx3d_segment(float x,float y,float z,float x2,float y2,float z2,uint32_t color) {
    if (!dst || !isfinite(x+y+z+x2+y2+z2)) return;
    flush_triangles();   /* обводка рисуется поверх уже отправленных граней */
    V3 a,b;to_view(x,y,z,&a);to_view(x2,y2,z2,&b);
    for (int p=0;p<6;p++) {
        float da=plane_distance(a,p),db=plane_distance(b,p);
        if (da<0 && db<0) return;
        if ((da<0)!=(db<0)) {
            V3 v=lerp3(a,b,da/(da-db));
            if(p==0)v.z=NEAR_Z;
            if (da<0) a=v;else b=v;
        }
    }
    float ax,ay,bx,by;
    if (!project_v(a,&ax,&ay) || !project_v(b,&bx,&by)) return;
    int steps=(int)ceilf(fmaxf(fabsf(bx-ax),fabsf(by-ay)));if(steps<1)steps=1;
    /* Увеличенный bias чтобы обводка не пропадала наполовину из-за z-fighting. */
    for (int i=0;i<=steps;i++) {
        float t=(float)i/steps,iz=(1-t)/a.z+t/b.z;
        int px=(int)floorf(ax+(bx-ax)*t),py=(int)floorf(ay+(by-ay)*t);
        if (px>=0 && py>=0 && px<rw && py<rh && iz+iz*.010f>=zbuf[py*rw+px])
            pix[py*rw+px]=pack(color);
    }
}

/* Взвешенная сумма вместо беззнакового (b-a): каналы не переполняются.
 * R/B считаются вместе, G отдельно; веса 0..256 сохраняют диапазон RGB. */
static uint32_t mix_rgb(uint32_t a, uint32_t b, uint32_t t) {
    uint32_t inv = 256 - t;
    uint32_t rb = (((a & 0x00ff00ffu) * inv + (b & 0x00ff00ffu) * t + 0x00800080u) >> 8) & 0x00ff00ffu;
    uint32_t g = (((a & 0x0000ff00u) * inv + (b & 0x0000ff00u) * t + 0x00008000u) >> 8) & 0x0000ff00u;
    return rb | g | 0xff000000u;
}

/* Четыре пикселя одним вектором: SSE2 на x86-64, NEON на arm64, без
 * интринзиков конкретной платформы. Арифметика побитово совпадает с mix_rgb:
 * те же 16-битные каналы, то же округление Q8, альфа всегда 255. */
#if defined(__GNUC__) && (defined(__SSE2__) || defined(__ARM_NEON) || defined(__aarch64__))
#define RBX_VECTOR_MIX 1
typedef uint32_t RbxV4 __attribute__((vector_size(16)));
typedef uint16_t RbxW8 __attribute__((vector_size(16)));
typedef int32_t RbxI4 __attribute__((vector_size(16)));
static inline RbxV4 vload(const uint32_t *p) { RbxV4 v; __builtin_memcpy(&v, p, sizeof v); return v; }
static inline void vstore(uint32_t *p, RbxV4 v) { __builtin_memcpy(p, &v, sizeof v); }
static inline RbxW8 vsplat16(uint32_t x) {
    RbxW8 v = {0,0,0,0,0,0,0,0};
    uint16_t *lane = (uint16_t *)&v;
    for (int i = 0; i < 8; i++) lane[i] = (uint16_t)x;
    return v;
}
static inline RbxV4 vmix(RbxV4 a, RbxV4 b, uint32_t t) {
    const RbxV4 mask = {0x00ff00ffu,0x00ff00ffu,0x00ff00ffu,0x00ff00ffu};
    const RbxV4 eight = {8,8,8,8};
    const RbxW8 bias = {0x0080,0x0080,0x0080,0x0080,0x0080,0x0080,0x0080,0x0080};
    RbxW8 w = vsplat16(t), inv = vsplat16(256 - t);
    RbxW8 rb = ((RbxW8)(a & mask) * inv + (RbxW8)(b & mask) * w + bias) >> 8;
    RbxW8 ga = ((RbxW8)((a >> eight) & mask) * inv + (RbxW8)((b >> eight) & mask) * w + bias) >> 8;
    /* После >>8 каждая дорожка ≤ 255, поэтому маска больше не нужна. */
    return (RbxV4)rb | ((RbxV4)ga << eight);
}
#endif

static Sample sample_at(int pos, int source, int target) {
    float f = ((float)pos + 0.5f) * source / target - 0.5f;
    if (f < 0) f = 0;
    if (f > source - 1) f = (float)(source - 1);
    int lo = (int)f;
    Sample s = {lo, lo + 1 < source ? lo + 1 : lo, (uint32_t)((f - lo) * 256 + 0.5f)};
    return s;
}

/* При целом масштабе смещение и вес сэмпла зависят только от позиции внутри
 * периода, поэтому таблица X строится один раз, а строка раскладывается
 * проходом по исходным пикселям вместо чтения 12-байтовой таблицы на столбец.
 * Периодичность проверяется; при любом расхождении остаётся общий путь,
 * так что побитовый результат прежний. */
static void upscale_prepare(void) {
    int W = dst->width, H = dst->height;
    if (sample_w == W && sample_rw == rw) return;
    for (int x = 0; x < W; x++) xsample[x] = sample_at(x, rw, W);
    sample_w = W; sample_rw = rw;
    up_scale = scale > 1 && scale <= 8 && rw*scale == W && rh*scale == H ? scale : 0;
    if (!up_scale) return;
    for (int k = 0; k < up_scale; k++) {
        Sample s = sample_at(up_scale + k, rw, W); /* i = 1, внутренняя позиция */
        up_off[k] = (signed char)(s.lo - 1);
        up_w[k] = s.weight;
    }
    for (int p = 0; p < 3; p++) {
        int i = p == 0 ? 1 : p == 1 ? rw/2 : rw-2;
        int j = p == 0 ? 1 : p == 1 ? rh/2 : rh-2;
        if (i < 1 || i > rw-2 || j < 1 || j > rh-2) { up_scale = 0; return; }
        for (int k = 0; k < up_scale; k++) {
            Sample sx = sample_at(i*up_scale+k, rw, W);
            Sample sy = sample_at(j*up_scale+k, rh, H);
            if (sx.lo != i+up_off[k] || sx.hi != sx.lo+1 || sx.weight != up_w[k] ||
                sy.lo != j+up_off[k] || sy.hi != sy.lo+1 || sy.weight != up_w[k]) { up_scale = 0; return; }
        }
    }
}

static void expand_row(uint32_t * restrict out, const uint32_t * restrict in, int W) {
#ifdef RBX_VECTOR_MIX
    /* Масштаб 2 — самый частый: два векторных прохода и перестановка. */
    if (up_scale == 2 && up_off[0] == -1 && up_off[1] == 0) {
        const uint32_t w0 = up_w[0], w1 = up_w[1];
        for (int x = 0; x < 2; x++) { Sample s = xsample[x]; out[x] = mix_rgb(in[s.lo], in[s.hi], s.weight); }
        int i = 1;
        for (; i + 4 <= rw - 2; i += 4) {
            RbxV4 a = vmix(vload(in+i-1), vload(in+i), w0);   /* чётные столбцы */
            RbxV4 b = vmix(vload(in+i), vload(in+i+1), w1);   /* нечётные столбцы */
            const RbxI4 lo = {0,4,1,5}, hi = {2,6,3,7};
            vstore(out+2*i, __builtin_shuffle(a, b, lo));
            vstore(out+2*i+4, __builtin_shuffle(a, b, hi));
        }
        for (; i < rw-1; i++) {
            out[2*i]   = mix_rgb(in[i-1], in[i], w0);
            out[2*i+1] = mix_rgb(in[i], in[i+1], w1);
        }
        for (int x = (rw-1)*2; x < W; x++) { Sample s = xsample[x]; out[x] = mix_rgb(in[s.lo], in[s.hi], s.weight); }
        return;
    }
#endif
    if (up_scale >= 2) {
        int S = up_scale;
        for (int x = 0; x < S; x++) { Sample s = xsample[x]; out[x] = mix_rgb(in[s.lo], in[s.hi], s.weight); }
        for (int i = 1; i < rw-1; i++) {
            const uint32_t *p = in + i;
            uint32_t *o = out + (size_t)i * S;
            for (int k = 0; k < S; k++) { int off = up_off[k]; o[k] = mix_rgb(p[off], p[off+1], up_w[k]); }
        }
        for (int x = (rw-1)*S; x < W; x++) { Sample s = xsample[x]; out[x] = mix_rgb(in[s.lo], in[s.hi], s.weight); }
        return;
    }
    for (int x = 0; x < W; x++) { Sample s = xsample[x]; out[x] = mix_rgb(in[s.lo], in[s.hi], s.weight); }
}

/* Промежуточные горизонтально раскрытые строки: свой буфер на каждую полосу. */
typedef struct { int cached[2]; uint32_t *rows; } Expand;
static Expand expand[RBX_MAX_PARTS];
static void upscale_rows(int y0,int y1,Expand *scratch) {
    int W = dst->width, H = dst->height, st = dst->stride;
    uint32_t *horizontal_rows_local=scratch->rows;
    int *cached=scratch->cached;
    for (int y=y0;y<y1;y++) {
        int lo, weight;
        if (up_scale >= 2) {
            int j = y / up_scale, k = y - j*up_scale;
            if (j >= 1 && j <= rh-2) { lo = j + up_off[k]; weight = (int)up_w[k]; }
            else { Sample s = sample_at(y, rh, H); lo = s.lo; weight = (int)s.weight; }
        } else { Sample s = sample_at(y, rh, H); lo = s.lo; weight = (int)s.weight; }
        int hi = lo + 1 < rh ? lo + 1 : lo;
        const uint32_t *expanded[2];
        /* Horizontal interpolation is shared by adjacent destination rows.
         * The Q8 rounding is bit-identical to the direct bilinear formula. */
        for (int i=0;i<2;i++) {
            int source=i ? hi : lo,index=source&1;
            uint32_t *row=horizontal_rows_local+(size_t)index*W;
            if (cached[index]!=source) {
                expand_row(row,pix+(size_t)source*rw,W);
                cached[index]=source;
            }
            expanded[i]=row;
        }
        uint32_t * restrict out=dst->pixels+(size_t)y*st;
        const uint32_t * restrict e0=expanded[0];
        const uint32_t * restrict e1=expanded[1];
        if (!weight) { memcpy(out,e0,(size_t)W*sizeof(*out)); continue; }
#ifdef RBX_VECTOR_MIX
        {
            uint32_t w=(uint32_t)weight;
            int x=0;
            for (;x+3<W;x+=4) vstore(out+x,vmix(vload(e0+x),vload(e1+x),w));
            for (;x<W;x++) out[x]=mix_rgb(e0[x],e1[x],w);
            continue;
        }
#endif
        /* Два пикселя считаются одним 64-битным умножением: каналы R/B и G
         * не переполняются, потому что inv+weight всегда равно 256. */
        uint32_t w=(uint32_t)weight,inv=256-w;
        int x=0;
        for (;x+1<W;x+=2) {
            uint64_t a=(uint64_t)e0[x]|((uint64_t)e0[x+1]<<32);
            uint64_t b=(uint64_t)e1[x]|((uint64_t)e1[x+1]<<32);
            uint64_t rb=(((a&0x00ff00ff00ff00ffull)*inv+(b&0x00ff00ff00ff00ffull)*w
                          +0x0080008000800080ull)>>8)&0x00ff00ff00ff00ffull;
            uint64_t g=(((a&0x0000ff000000ff00ull)*inv+(b&0x0000ff000000ff00ull)*w
                         +0x0000800000008000ull)>>8)&0x0000ff000000ff00ull;
            uint64_t r=rb|g|0xff000000ff000000ull;
            out[x]=(uint32_t)r;out[x+1]=(uint32_t)(r>>32);
        }
        if (x<W) out[x]=mix_rgb(e0[x],e1[x],w);
    }
}
static void upscale_job(int part,void *ctx) {
    int lo[RBX_MAX_PARTS],hi[RBX_MAX_PARTS];
    (void)ctx;
    equal_bounds(dst->height,band_parts,lo,hi);
    upscale_rows(lo[part],hi[part],&expand[part]);
}
/* Билинейный апскейл без дорогих float-операций на каждом пикселе.
 * Таблица X пересчитывается только при смене разрешения, строки раскладываются
 * в несколько потоков: каждая полоса пишет свои строки кадра. */
static void upscale_smooth(void) {
    int W=dst->width,H=dst->height;
    upscale_prepare();
    int parts=parts_for((int64_t)W*H,400000);
    if (parts>H) parts=H;
    band_parts=parts;
    for (int i=0;i<parts;i++) {
        expand[i].cached[0]=expand[i].cached[1]=-1;
        expand[i].rows=horizontal_rows+(size_t)i*2*W;
    }
    band_parts=parts;
    if (parts>1) rbx_jobs(parts,upscale_job,NULL);
    else upscale_rows(0,H,&expand[0]);
}

static int copy_rows_target;
static void copy_rows(int y0,int y1) {
    int st=dst->stride;
    for (int y=y0;y<y1;y++) memcpy(dst->pixels+(size_t)y*st,pix+(size_t)y*rw,(size_t)rw*4);
}
static void copy_job(int part,void *ctx) {
    int lo[RBX_MAX_PARTS],hi[RBX_MAX_PARTS];
    (void)ctx;
    equal_bounds(copy_rows_target,band_parts,lo,hi);
    if (hi[part]>copy_rows_target) hi[part]=copy_rows_target;
    if (lo[part]<hi[part]) copy_rows(lo[part],hi[part]);
}
void rbx3d_end(void) {
    if (!dst || !pix) return;
    flush_triangles();
    int H = dst->height;
    if (scale == 1) {
        copy_rows_target = H < rh ? H : rh;
        int parts = parts_for((int64_t)rw * copy_rows_target, 400000);
        if (parts > copy_rows_target) parts = copy_rows_target;
        band_parts = parts;
        if (parts > 1) rbx_jobs(parts, copy_job, NULL);
        else copy_rows(0, copy_rows_target);
        return;
    }
    upscale_smooth();
}
