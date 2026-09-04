/* Утилиты интерфейса мессенджера: аватарки, иконки (всё рисуется базовыми
 * примитивами рендера — линии/круги/треугольники, никаких картинок),
 * текстовые помощники и форматирование времени. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "msg.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── время ─────────────────────────────────────────────────────────────── */
static double mono_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0.0;
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
double msg_now(void) { return mono_now(); }
double msg_wall(void) { return (double)time(NULL); }

float msg_u(void) {
    float u = (float)screen_w / 1080.0f;
    return u > 0.05f ? u : 0.05f;
}

/* ── палитра аватарок в духе Telegram ──────────────────────────────────── */
static const uint32_t AV_COLORS[8] = {
    0xFFEF5350u, 0xFFFFA241u, 0xFFAB67D6u, 0xFF59C96Fu,
    0xFF2FC1F2u, 0xFF5F74D2u, 0xFFF06292u, 0xFF36B5A6u,
};

/* ── текст ─────────────────────────────────────────────────────────────── */
int msg_text_w(const char *s, float scale) {
    return (int)(text_width(s) * scale + 0.5f);
}
float msg_line_h(float scale) { return text_line_height() * scale; }

static int utf8_next(const char *s, int i) {
    unsigned char c = (unsigned char)s[i];
    if (!c) return i;
    if (c < 0x80) return i + 1;
    if ((c & 0xE0) == 0xC0) return s[i+1] ? i + 2 : i + 1;
    if ((c & 0xF0) == 0xE0) return (s[i+1] && s[i+2]) ? i + 3 : i + 1;
    return (s[i+1] && s[i+2] && s[i+3]) ? i + 4 : i + 1;
}

/* Центрирует строку по «чернильному» боксу, чтобы хвостики букв не
 * смещали подпись (тот же приём, что использовал движок игры). */
void msg_draw_text_c(const char *s, float cx, float cy, float scale, uint32_t c) {
    int w = text_width(s), h = text_height(s), top = text_ink_top(s);
    float x = cx - (float)w * scale * 0.5f;
    float y = cy - ((float)h * 0.5f + (float)top) * scale;
    text_scaled(s, x, y, c, scale);
}

/* Обрезает строку под максимальную ширину, добавляя многоточие. */
void msg_text_fit(const char *s, float scale, int maxw, char *out, int outsz) {
    if (!s || !out || outsz <= 0) return;
    out[0] = '\0';
    if (maxw <= 4) return;
    if (msg_text_w(s, scale) <= maxw) {
        snprintf(out, (size_t)outsz, "%s", s);
        return;
    }
    int dots_w = msg_text_w("…", scale);
    int limit = maxw - dots_w;
    char tmp[MSG_TEXT_MAX];
    int i = 0, last = 0;
    while (s[i]) {
        int next = utf8_next(s, i);
        int n = next < (int)sizeof(tmp) - 1 ? next : (int)sizeof(tmp) - 1;
        memcpy(tmp, s, (size_t)n);
        tmp[n] = '\0';
        if (msg_text_w(tmp, scale) > limit) break;
        last = next;
        i = next;
    }
    int n = last < outsz - 4 ? last : outsz - 4;
    memcpy(out, s, (size_t)n);
    memcpy(out + n, "…", 3);
    out[n + 3] = '\0';
}

/* ── форматирование времени ────────────────────────────────────────────── */
static const char *WD_SHORT[7] = { "Вс", "Пн", "Вт", "Ср", "Чт", "Пт", "Сб" };
static const char *MON_GEN[12] = {
    "января", "февраля", "марта", "апреля", "мая", "июня",
    "июля", "августа", "сентября", "октября", "ноября", "декабря",
};

static void tm_of(double t, struct tm *out) {
    time_t tt = (time_t)t;
    localtime_r(&tt, out);
}

int msg_same_day(double a, double b) {
    struct tm ta, tb;
    tm_of(a, &ta); tm_of(b, &tb);
    return ta.tm_year == tb.tm_year && ta.tm_yday == tb.tm_yday;
}

void msg_fmt_hm(double t, char *out, int n) {
    struct tm tmv;
    tm_of(t, &tmv);
    snprintf(out, (size_t)n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

void msg_fmt_time(double t, char *out, int n) {
    double now = msg_wall();
    if (msg_same_day(t, now)) { msg_fmt_hm(t, out, n); return; }
    struct tm tt, tn;
    tm_of(t, &tt); tm_of(now, &tn);
    time_t day_t = (time_t)t - ((time_t)t % 86400);
    (void)day_t;
    double yest = now - 86400.0;
    if (msg_same_day(t, yest) && tn.tm_yday - 1 == tt.tm_yday) {
        snprintf(out, (size_t)n, "Вчера");
        return;
    }
    double diff = now - t;
    if (diff < 6.5 * 86400.0) {
        snprintf(out, (size_t)n, "%s", WD_SHORT[tt.tm_wday]);
        return;
    }
    snprintf(out, (size_t)n, "%d %s", tt.tm_mday, MON_GEN[tt.tm_mon]);
}

void msg_fmt_day(double t, char *out, int n) {
    double now = msg_wall();
    if (msg_same_day(t, now)) { snprintf(out, (size_t)n, "Сегодня"); return; }
    if (msg_same_day(t, now - 86400.0)) { snprintf(out, (size_t)n, "Вчера"); return; }
    struct tm tt;
    tm_of(t, &tt);
    snprintf(out, (size_t)n, "%d %s", tt.tm_mday, MON_GEN[tt.tm_mon]);
}

/* ── геометрия ─────────────────────────────────────────────────────────── */
void msg_poly(const float *xy, int n, uint32_t c) {
    if (!xy || n < 3) return;
    for (int i = 1; i + 1 < n; i++)
        tri(xy[0], xy[1], xy[2*i], xy[2*i+1], xy[2*(i+1)], xy[2*(i+1)+1], c);
}

void msg_arc(float cx, float cy, float r, float a0, float a1, float th, uint32_t c) {
    if (a1 <= a0 || r <= 0) return;
    int segs = (int)((a1 - a0) / (2.0 * M_PI) * 40.0) + 2;
    if (segs > 64) segs = 64;
    float px = cx + r * cosf(a0), py = cy + r * sinf(a0);
    for (int i = 1; i <= segs; i++) {
        float a = a0 + (a1 - a0) * (float)i / (float)segs;
        float nx = cx + r * cosf(a), ny = cy + r * sinf(a);
        line(px, py, nx, ny, th, c);
        px = nx; py = ny;
    }
}

/* Собирает до двух первых букв слова (латиницу и кириллицу переводит в
 * верхний регистр), результат — валидный UTF-8. */
static int grab_initials(const char *name, char *out) {
    int letters = 0, oi = 0;
    const char *p = name ? name : "";
    int at_word_start = 1;
    while (*p && letters < 2) {
        if (*p == ' ') { at_word_start = 1; p++; continue; }
        unsigned char ch = (unsigned char)*p;
        int len = 1;
        if ((ch & 0xE0) == 0xC0) len = 2;
        else if ((ch & 0xF0) == 0xE0) len = 3;
        else if ((ch & 0xF8) == 0xF0) len = 4;
        if (at_word_start) {
            char cp[4];
            int ok = 1;
            for (int k = 0; k < len; k++) { if (!p[k]) { ok = 0; break; } cp[k] = p[k]; }
            if (ok) {
                /* upper: ASCII a-z */
                if (len == 1 && cp[0] >= 'a' && cp[0] <= 'z') cp[0] = (char)(cp[0] - 32);
                /* upper: кириллица а-п (D0 B0..D0 BF) → А-П (D0 90..D0 AF) */
                else if (len == 2 && (unsigned char)cp[0] == 0xD0 &&
                         (unsigned char)cp[1] >= 0xB0 && (unsigned char)cp[1] <= 0xBF)
                    cp[1] = (char)((unsigned char)cp[1] - 0x20);
                /* upper: кириллица р-я (D1 80..D1 8F) → Р-Я (D0 A0..D0 AF) */
                else if (len == 2 && (unsigned char)cp[0] == 0xD1 &&
                         (unsigned char)cp[1] >= 0x80 && (unsigned char)cp[1] <= 0x8F) {
                    cp[0] = (char)0xD0;
                    cp[1] = (char)((unsigned char)cp[1] - 0x20);
                }
                for (int k = 0; k < len; k++) out[oi++] = cp[k];
                letters++;
            }
            at_word_start = 0;
        }
        p += len;
        while (*p && *p != ' ') p = name + utf8_next(name, (int)(p - name));
    }
    if (letters == 0) { out[0] = '?'; oi = 1; }
    out[oi] = '\0';
    return letters;
}

/* ── аватарка: цветной круг с инициалами (или закладкой для Избранного) ─ */
void msg_avatar(float cx, float cy, float r, int color, const char *name, int saved) {
    circle(cx, cy, r, AV_COLORS[((color % 8) + 8) % 8]);
    if (saved) {
        /* Закладка: прямоугольник с вырезом снизу (веер из треугольников). */
        float w = r * 0.52f, h = r * 0.98f, top = cy - h * 0.5f, bot = cy + h * 0.5f;
        float pts[10] = {
            cx - w, top,  cx + w, top,  cx + w, bot,
            cx, bot - w * 0.9f,  cx - w, bot,
        };
        msg_poly(pts, 5, C_WHITE);
        return;
    }
    char ini[12];
    grab_initials(name, ini);
    float scale = (r * 0.80f) / 32.0f;
    msg_draw_text_c(ini, cx, cy + r * 0.02f, scale, C_WHITE);
}

/* ── иконки (размер s ≈ радиус описанной окружности) ───────────────────── */
void ico_menu(float cx, float cy, float s, uint32_t c) {
    float t = s * 0.20f, w = s * 0.95f;
    line(cx - w, cy - s * 0.62f, cx + w, cy - s * 0.62f, t, c);
    line(cx - w, cy,              cx + w, cy,              t, c);
    line(cx - w, cy + s * 0.62f, cx + w, cy + s * 0.62f, t, c);
}

void ico_search(float cx, float cy, float s, uint32_t c) {
    float t = s * 0.20f;
    ring(cx - s * 0.14f, cy - s * 0.14f, s * 0.60f, t, c);
    line(cx + s * 0.30f, cy + s * 0.30f, cx + s * 0.78f, cy + s * 0.78f, t, c);
}

void ico_back(float cx, float cy, float s, uint32_t c) {
    float t = s * 0.21f;
    line(cx + s * 0.80f, cy, cx - s * 0.72f, cy, t, c);
    line(cx - s * 0.72f, cy, cx - s * 0.06f, cy - s * 0.62f, t, c);
    line(cx - s * 0.72f, cy, cx - s * 0.06f, cy + s * 0.62f, t, c);
}

void ico_pencil(float cx, float cy, float s, uint32_t c) {
    /* Карандаш под 45°: тело-капсула и носик-треугольник. */
    float ax = cx - s * 0.62f, ay = cy + s * 0.62f;
    float bx = cx + s * 0.38f, by = cy - s * 0.38f;
    line(ax, ay, bx, by, s * 0.46f, c);
    float px = -0.7071f, py = 0.7071f;             /* перпендикуляр к телу */
    float hw = s * 0.23f;
    float pts[6] = {
        bx + px * hw, by + py * hw,
        cx + s * 0.86f, cy - s * 0.86f,
        bx - px * hw, by - py * hw,
    };
    msg_poly(pts, 3, c);
}

void ico_send(float cx, float cy, float s, uint32_t c) {
    /* Бумажный самолётик: треугольник с вырезом на хвосте. */
    float pts[8] = {
        cx + s * 0.90f, cy,
        cx - s * 0.78f, cy - s * 0.66f,
        cx - s * 0.34f, cy,
        cx - s * 0.78f, cy + s * 0.66f,
    };
    msg_poly(pts, 4, c);
}

void ico_mic(float cx, float cy, float s, uint32_t c) {
    float t = s * 0.16f;
    line(cx, cy - s * 0.66f, cx, cy - s * 0.06f, s * 0.46f, c);
    msg_arc(cx, cy - s * 0.02f, s * 0.52f, (float)M_PI * 0.08f, (float)M_PI * 0.92f, t, c);
    line(cx, cy + s * 0.50f, cx, cy + s * 0.80f, t, c);
    line(cx - s * 0.34f, cy + s * 0.82f, cx + s * 0.34f, cy + s * 0.82f, t, c);
}

void ico_smile(float cx, float cy, float s, uint32_t c) {
    float t = s * 0.15f;
    ring(cx, cy, s * 0.82f, t, c);
    circle(cx - s * 0.32f, cy - s * 0.28f, s * 0.09f, c);
    circle(cx + s * 0.32f, cy - s * 0.28f, s * 0.09f, c);
    msg_arc(cx, cy + s * 0.02f, s * 0.44f, (float)M_PI * 0.18f, (float)M_PI * 0.82f, t * 0.9f, c);
}

void ico_clip(float cx, float cy, float s, uint32_t c) {
    float t = s * 0.15f;
    line(cx - s * 0.34f, cy - s * 0.46f, cx - s * 0.34f, cy + s * 0.34f, t, c);
    msg_arc(cx, cy + s * 0.34f, s * 0.34f, 0, (float)M_PI, t, c);
    line(cx + s * 0.34f, cy + s * 0.34f, cx + s * 0.34f, cy - s * 0.56f, t, c);
    msg_arc(cx + s * 0.10f, cy - s * 0.56f, s * 0.24f, (float)M_PI, (float)M_PI * 2.0f, t, c);
    line(cx - s * 0.14f, cy - s * 0.56f, cx - s * 0.14f, cy + s * 0.14f, t, c);
    msg_arc(cx, cy + s * 0.14f, s * 0.14f, 0, (float)M_PI, t, c);
    line(cx + s * 0.14f, cy + s * 0.14f, cx + s * 0.14f, cy - s * 0.22f, t, c);
}

void ico_check(float cx, float cy, float s, uint32_t c) {
    float t = s * 0.20f;
    line(cx - s * 0.62f, cy + s * 0.02f, cx - s * 0.18f, cy + s * 0.46f, t, c);
    line(cx - s * 0.18f, cy + s * 0.46f, cx + s * 0.66f, cy - s * 0.42f, t, c);
}

void ico_checks(float cx, float cy, float s, uint32_t c) {
    float t = s * 0.17f;
    float dx = s * 0.30f;
    ico_check(cx - dx * 0.5f, cy, s * 0.9f, c);
    line(cx - s * 0.18f + dx, cy + s * 0.42f, cx + s * 0.60f, cy - s * 0.40f, t, c);
}

void ico_clock(float cx, float cy, float s, uint32_t c) {
    float t = s * 0.15f;
    ring(cx, cy, s * 0.62f, t, c);
    line(cx, cy, cx, cy - s * 0.34f, t, c);
    line(cx, cy, cx + s * 0.26f, cy + s * 0.10f, t, c);
}

void ico_close(float cx, float cy, float s, uint32_t c) {
    float t = s * 0.20f, d = s * 0.58f;
    line(cx - d, cy - d, cx + d, cy + d, t, c);
    line(cx + d, cy - d, cx - d, cy + d, t, c);
}

void ico_mute(float cx, float cy, float s, uint32_t c) {
    float t = s * 0.14f;
    msg_arc(cx, cy - s * 0.12f, s * 0.42f, (float)M_PI, (float)M_PI * 2.0f, t, c);
    line(cx - s * 0.42f, cy - s * 0.12f, cx - s * 0.56f, cy + s * 0.34f, t, c);
    line(cx + s * 0.42f, cy - s * 0.12f, cx + s * 0.56f, cy + s * 0.34f, t, c);
    line(cx - s * 0.62f, cy + s * 0.34f, cx + s * 0.62f, cy + s * 0.34f, t, c);
    circle(cx, cy + s * 0.58f, s * 0.10f, c);
    line(cx - s * 0.72f, cy - s * 0.72f, cx + s * 0.72f, cy + s * 0.72f, t, c);
}
