/* Экран разговора: обои, пузыри сообщений с хвостиками и галочками,
 * разделители дат, индикатор «печатает», поле ввода и прокрутка с инерцией. */
#include "msg.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define CH_HDR   132.0f
#define CH_INPUT 148.0f

#define TS   0.52f            /* масштаб текста сообщений */
#define TSC  0.34f            /* масштаб времени в пузыре */
#define TSA  0.42f            /* масштаб автора в группе */

typedef struct { int start, len; float w; } Line;

typedef struct {
    float y, w, h;           /* координата в пространстве контента (сверху) */
    int out;
    int nlines;
    Line lines[64];
    char rendered[MSG_TEXT_MAX];
    float reserve;
    char date[24];           /* непустая строка → рисовать разделитель над пузырём */
    int is_typing;
    int state;
    char hm[8];
    char author[28];
    int author_color;
} LayMsg;

typedef struct {
    int ci;
    double off;              /* 0 = прижаты к низу; больше — прокручено вверх */
    double vel;
    int dragging, moved, snap;
    float down_x, down_y;
    int focus;
    char draft[MSG_DRAFT_MAX];
    int press_send, press_field, press_back, press_down;
    /* кэш раскладки */
    long gen;
    int lw, lh;
    LayMsg *items;
    int n, cap;
    float content_h;
} ChatState;

static ChatState st;
static int st_open = 0;
static int st_want_close = 0;

void chat_reset(void) {
    free(st.items);
    memset(&st, 0, sizeof(st));
    st_open = 0;
    st_want_close = 0;
}

int chat_is_open(void) { return st_open; }
int chat_current(void) { return st.ci; }
int chat_wants_close(void) { return st_want_close; }
void chat_finish_close(void) {
    st_open = 0;
    st_want_close = 0;
    st.focus = 0;
    keyboard_hide();
}

void chat_open(int ci) {
    if (ci < 0 || ci >= msg_chat_count) return;
    st.ci = ci;
    st_open = 1;
    st.off = 0;
    st.vel = 0;
    st.focus = 0;
    st.snap = 0;
    st.draft[0] = '\0';
    st.gen = -1;
    keyboard_clear();
    if (msg_chats[ci].unread) {
        msg_chats[ci].unread = 0;
        msg_data_save();
    }
}

void chat_close_request(void) {
    keyboard_hide();
    st.focus = 0;
}

static float view_top(float u) { (void)u; return CH_HDR * u; }
static float view_bottom(float u) { return (float)screen_h - CH_INPUT * u; }

static uint32_t author_pal(const char *s) {
    unsigned h = 5381;
    for (const char *p = s; *p; p++) h = h * 33 + (unsigned char)*p;
    return (uint32_t)(h % 8);
}

/* ── перенос слов ──────────────────────────────────────────────────────── */
static int measure_range(const char *text, int start, int len, float scale) {
    static char tmp[MSG_TEXT_MAX];
    if (len <= 0) return 0;
    if (len >= (int)sizeof(tmp)) len = (int)sizeof(tmp) - 1;
    memcpy(tmp, text + start, (size_t)len);
    tmp[len] = '\0';
    return msg_text_w(tmp, scale);
}

static void wrap_message(const char *text, float scale, float limit, float reserve,
                         Line *lines, int *nlines) {
    int n = 0;
    int i = 0, L = (int)strlen(text);
    int line_start = 0, line_end = 0;
    float line_w = 0;

    while (i <= L && n < 63) {
        /* конец слова: идём до пробела */
        int w_start = i;
        while (i < L && text[i] != ' ') i++;
        int w_end = i;
        int wlen = w_end - w_start;
        float ww = wlen ? (float)measure_range(text, w_start, wlen, scale) : 0;
        float space_w = wlen ? (float)msg_text_w(" ", scale) : 0;

        if (line_end == line_start) {
            /* первое слово в строке */
            line_start = w_start;
            line_end = w_end;
            line_w = ww;
        } else {
            if (line_w + space_w + ww <= limit) {
                line_end = w_end;
                line_w += space_w + ww;
            } else {
                lines[n].start = line_start;
                lines[n].len = line_end - line_start;
                lines[n].w = line_w;
                n++;
                line_start = w_start;
                line_end = w_end;
                line_w = ww;
            }
        }
        if (i >= L) break;
        i++; /* пропустить пробел */
    }
    if (line_end > line_start || (L == 0 && n == 0)) {
        if (line_end >= line_start) {
            lines[n].start = line_start;
            lines[n].len = line_end - line_start;
            lines[n].w = line_w;
            n++;
        }
    }
    if (n == 0) {
        lines[0].start = 0; lines[0].len = 0; lines[0].w = 0; n = 1;
    }
    /* резерв под время в последней строке: при необходимости переносим слова */
    for (int guard = 0; guard < 64; guard++) {
        Line *last = &lines[n - 1];
        if (last->w + reserve <= limit) break;
        /* ищем последний пробел в последней строке */
        int cut = -1;
        for (int k = last->start + last->len - 1; k > last->start; k--)
            if (text[k] == ' ') { cut = k; break; }
        if (cut < 0) break; /* одно длинное слово — оставляем как есть */
        int new_len = cut - last->start;
        int tail_start = cut + 1;
        int tail_len = (last->start + last->len) - tail_start;
        last->len = new_len;
        last->w = (float)measure_range(text, last->start, new_len, scale);
        if (n < 63) {
            lines[n].start = tail_start;
            lines[n].len = tail_len;
            lines[n].w = (float)measure_range(text, tail_start, tail_len, scale);
            n++;
        }
    }
    *nlines = n;
}

/* ── раскладка всего разговора ─────────────────────────────────────────── */
static void rebuild_layout(float u) {
    MsgChat *c = &msg_chats[st.ci];
    float W = (float)screen_w;
    float max_bw = W * 0.80f;
    if (max_bw > 830 * u) max_bw = 830 * u;
    float pad_h = 26 * u, pad_v = 18 * u;
    float line_h = msg_line_h(TS);
    float limit = max_bw - pad_h * 2;

    if (st.n == 0 && st.cap == 0) {
        st.cap = 64;
        st.items = (LayMsg *)calloc((size_t)st.cap, sizeof(LayMsg));
    }
    while (st.cap < c->count + 2) {
        st.cap *= 2;
        st.items = (LayMsg *)realloc(st.items, (size_t)st.cap * sizeof(LayMsg));
    }
    st.n = 0;
    float y = 24 * u;
    double prev_day_t = 0;

    for (int i = 0; i < c->count; i++) {
        MsgMessage *m = &c->msgs[i];
        if (st.n >= st.cap) break;
        LayMsg *it = &st.items[st.n];
        memset(it, 0, sizeof(*it));
        it->out = m->out;
        it->state = m->state;
        msg_fmt_hm(m->t, it->hm, sizeof(it->hm));

        /* разделитель даты */
        if (prev_day_t == 0 || !msg_same_day(prev_day_t, m->t)) {
            msg_fmt_day(m->t, it->date, sizeof(it->date));
            y += 26 * u;
            y += 52 * u; /* высота плашки с запасом */
        }
        prev_day_t = m->t;

        /* автор в группах: имя до двоеточия, цвет по хэшу имени */
        if (c->group && !m->out) {
            const char *colon = strchr(m->text, ':');
            if (colon && colon - m->text >= 2 && colon - m->text < 24) {
                int an = (int)(colon - m->text);
                memcpy(it->author, m->text, (size_t)an);
                it->author[an] = '\0';
                it->author_color = (int)author_pal(it->author);
            }
        }

        int time_w = msg_text_w(it->hm, TSC);
        float checks_w = m->out ? 40 * u : 0;
        it->reserve = (float)time_w + checks_w + 34 * u;

        const char *body = m->text;
        if (it->author[0]) {
            body = strchr(m->text, ':');
            body = body ? body + 2 : m->text;
        }
        wrap_message(body, TS, limit, it->reserve, it->lines, &it->nlines);

        float bw_text = 0;
        for (int k = 0; k < it->nlines; k++)
            if (it->lines[k].w > bw_text) bw_text = it->lines[k].w;
        float last_w = it->lines[it->nlines - 1].w;
        float bw = bw_text;
        if (last_w + it->reserve > bw && last_w + it->reserve <= limit) bw = last_w + it->reserve;
        if (bw < it->reserve + 30 * u) bw = it->reserve + 30 * u;
        it->w = bw + pad_h * 2;
        if (it->w > max_bw) it->w = max_bw;
        float author_h = it->author[0] ? msg_line_h(TSA) + 6 * u : 0;
        it->h = pad_v * 2 + (float)it->nlines * line_h + author_h;

        /* собрать текст с переносами в одну строку (автор рисуется отдельно) */
        int p = 0;
        for (int k = 0; k < it->nlines && p < (int)sizeof(it->rendered) - 2; k++) {
            Line *ln = &it->lines[k];
            int room = (int)sizeof(it->rendered) - 2 - p;
            int cp = ln->len < room ? ln->len : room;
            if (k) it->rendered[p++] = '\n';
            memcpy(it->rendered + p, body + ln->start, (size_t)cp);
            p += cp;
        }
        it->rendered[p] = '\0';

        it->y = y;
        y += it->h + 10 * u;
        st.n++;
    }

    /* индикатор «печатает…» */
    if (c->reply_phase == 2 && st.n < st.cap) {
        LayMsg *it = &st.items[st.n];
        memset(it, 0, sizeof(*it));
        it->is_typing = 1;
        it->w = 130 * u;
        it->h = pad_v * 2 + line_h * 0.9f;
        if (prev_day_t == 0 || !msg_same_day(prev_day_t, msg_wall())) {
            msg_fmt_day(msg_wall(), it->date, sizeof(it->date));
            y += 26 * u + 52 * u;
        }
        it->y = y;
        y += it->h + 10 * u;
        st.n++;
    }

    st.content_h = y + 16 * u;
    st.gen = c->gen;
    st.lw = screen_w;
    st.lh = screen_h;
}

static void ensure_layout(float u) {
    MsgChat *c = &msg_chats[st.ci];
    if (st.gen != c->gen || st.lw != screen_w || st.lh != screen_h || st.n == 0)
        rebuild_layout(u);
}

/* ── обновление ────────────────────────────────────────────────────────── */
void chat_update(void) {
    if (!st_open) return;
    float u = msg_u();
    MsgChat *c = &msg_chats[st.ci];
    ensure_layout(u);

    double view_h = (double)view_bottom(u) - view_top(u);
    double max_off = st.content_h - view_h;
    if (max_off < 0) max_off = 0;

    if (!st.dragging) {
        if (fabs(st.vel) > 1.0) {
            st.off += st.vel * dt;
            st.vel *= pow(0.015, dt);
        } else {
            st.vel = 0;
        }
        if (st.snap) {
            st.off = lerp(st.off, 0, 1.0 - pow(0.00001, dt));
            if (st.off < 1.0) { st.off = 0; st.snap = 0; }
        }
        if (st.off < -60 * u) st.off = -60 * u;
        if (st.off > max_off + 60 * u) st.off = max_off + 60 * u;
        if (st.off < 0) st.off = lerp(st.off, 0, 1.0 - pow(0.0005, dt));
        if (st.off > max_off) st.off = lerp(st.off, max_off, 1.0 - pow(0.0005, dt));
    }

    /* поле ввода */
    if (st.focus) {
        snprintf(st.draft, sizeof(st.draft), "%s", keyboard_get_raw());
        if (keyboard_enter_pressed()) {
            char send_buf[MSG_DRAFT_MAX];
            snprintf(send_buf, sizeof(send_buf), "%s", st.draft);
            char *trimmed = (char *)str_trim(send_buf);
            if (trimmed[0]) {
                msg_send(st.ci, trimmed);
                keyboard_clear();
                st.draft[0] = '\0';
                st.snap = 1;
            }
        }
    }
    (void)c;
}

/* ── обои ──────────────────────────────────────────────────────────────── */
static void draw_wallpaper(float u, float top, float bottom, float dx) {
    float W = (float)screen_w;
    float h = bottom - top;
    int bands = 22;
    for (int i = 0; i < bands; i++) {
        float t = (float)i / (float)(bands - 1);
        uint32_t c0 = C_WALL_TOP, c1 = C_WALL_BOT;
        int r = (int)lerp((float)(c0 >> 16 & 0xff), (float)(c1 >> 16 & 0xff), t);
        int g = (int)lerp((float)(c0 >> 8 & 0xff), (float)(c1 >> 8 & 0xff), t);
        int b = (int)lerp((float)(c0 & 0xff), (float)(c1 & 0xff), t);
        uint32_t cc = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
        float y0 = top + h * (float)i / (float)bands;
        rect(dx, y0, W, h / bands + 1.0f, cc);
    }
    /* лёгкий узор: колечки и точки, медленно плывущие вместе с прокруткой */
    float pat = 210 * u;
    float shift = (float)fmod(st.off * 0.5, pat);
    uint32_t dc = 0x30FFFFFFu;
    int cols = (int)(W / pat) + 2, rows = (int)(h / pat) + 3;
    for (int ry = -1; ry < rows; ry++) {
        for (int cx = 0; cx < cols; cx++) {
            unsigned hsh = (unsigned)(ry * 37 + cx * 91) * 2654435761u;
            float jx = (float)(hsh % 100) / 100.0f * pat * 0.5f;
            float jy = (float)(hsh / 100 % 100) / 100.0f * pat * 0.5f;
            float x = dx + cx * pat + jx, y = top + ry * pat + jy + shift;
            if (y < top - 40 * u || y > bottom + 40 * u) continue;
            if ((hsh >> 16) % 3 == 0) ring(x, y, 14 * u, 3 * u, dc);
            else if ((hsh >> 16) % 3 == 1) circle(x, y, 5 * u, dc);
            else {
                float s = 10 * u;
                line(x - s, y, x + s, y, 2.5f * u, dc);
                line(x, y - s, x, y + s, 2.5f * u, dc);
            }
        }
    }
}

/* ── пузыри ────────────────────────────────────────────────────────────── */
static void draw_bubble(float u, float x, float y, LayMsg *it) {
    uint32_t bg = it->out ? C_BUBBLE_OUT : C_BUBBLE_IN;
    float r = 18 * u;
    roundrect(x, y, it->w, it->h, r, bg);
    /* хвостик: маленький треугольник у нижнего внешнего угла, носик наружу */
    float bx = it->out ? x + it->w : x;
    float dir = it->out ? 1.0f : -1.0f;
    tri(bx + dir * 2 * u, y + it->h - 26 * u,
        bx + dir * 2 * u, y + it->h + 1 * u,
        bx + dir * 17 * u, y + it->h + 6 * u, bg);

    float pad_h = 26 * u, pad_v = 18 * u;
    float line_h = msg_line_h(TS);
    float ty = y + pad_v;
    if (it->author[0]) {
        float ah = msg_line_h(TSA);
        /* цвет автора из палитры аватарок */
        static const uint32_t pal[8] = {
            0xFFC94F4Cu, 0xFFD98425u, 0xFF8E55C2u, 0xFF3FA653u,
            0xFF2492C9u, 0xFF4C5DB8u, 0xFFCC4E7Eu, 0xFF2C9587u,
        };
        text_scaled(it->author, x + pad_h, ty, pal[it->author_color % 8], TSA);
        ty += ah + 6 * u;
    }
    if (it->is_typing) {
        /* три прыгающие точки */
        float cx0 = x + pad_h + 12 * u, cy = y + it->h * 0.5f;
        for (int i = 0; i < 3; i++) {
            float ph = (float)msg_now() * 5.0f - (float)i * 0.7f;
            float dy = -fabsf(sinf(ph)) * 7 * u;
            float a = 0.5f + 0.5f * fabsf(sinf(ph));
            uint32_t cc = 0xFF707579u;
            cc = (uint32_t)((int)(a * 255) << 24) | (cc & 0xFFFFFF);
            circle(cx0 + i * 26 * u, cy + dy, 7 * u, cc);
        }
        return;
    }
    text_scaled(it->rendered, x + pad_h, ty, C_TEXT_MAIN, TS);

    /* время + галочки в конце последней строки */
    float last_cy = ty + (float)(it->nlines - 1) * line_h + line_h * 0.5f;
    float xr = x + it->w - pad_h;
    int time_w = msg_text_w(it->hm, TSC);
    if (it->out) {
        float cw = 36 * u;
        if (it->state == 2) ico_checks(xr - cw * 0.5f, last_cy, 15 * u, C_TIME_OUT);
        else if (it->state == 1) ico_check(xr - cw * 0.5f, last_cy, 13 * u, C_TIME_OUT);
        else ico_clock(xr - cw * 0.5f, last_cy, 12 * u, C_TIME_IN);
        text_scaled(it->hm, xr - cw - 8 * u - time_w, last_cy - text_height(it->hm) * TSC * 0.5f - text_ink_top(it->hm) * TSC, C_TIME_OUT, TSC);
    } else {
        text_scaled(it->hm, xr - time_w, last_cy - text_height(it->hm) * TSC * 0.5f - text_ink_top(it->hm) * TSC, C_TIME_IN, TSC);
    }
}

void chat_draw(float dx) {
    if (!st_open) return;
    float u = msg_u(), W = (float)screen_w, H = (float)screen_h;
    MsgChat *c = &msg_chats[st.ci];
    ensure_layout(u);

    float vt = view_top(u), vb = view_bottom(u);
    draw_wallpaper(u, vt, vb, dx);

    /* сообщения: низ контента привязан к низу экрана с учётом прокрутки */
    float base_y = vb + (float)st.off - st.content_h;
    for (int i = 0; i < st.n; i++) {
        LayMsg *it = &st.items[i];
        float y = base_y + it->y;
        if (y > vb + 20 * u || y + it->h + 60 * u < vt) {
            continue;
        }
        /* разделитель даты */
        if (it->date[0]) {
            int dw = msg_text_w(it->date, 0.40f);
            float pw = (float)dw + 44 * u, ph = 48 * u;
            float px = dx + W * 0.5f - pw * 0.5f, py = y - 52 * u + 2 * u;
            roundrect(px, py, pw, ph, ph * 0.5f, C_DATE_PILL);
            msg_draw_text_c(it->date, dx + W * 0.5f, py + ph * 0.5f + 1 * u, 0.40f, 0xFF41505Bu);
        }
        float x = it->out ? dx + W - 32 * u - it->w : dx + 32 * u;
        draw_bubble(u, x, y, it);
    }

    /* кнопка «вниз» */
    if (st.off > 350 * u) {
        float fx = dx + W - 92 * u, fy = vb - 84 * u;
        circle(fx, fy + 3 * u, 56 * u, 0x2A000000u);
        circle(fx, fy, 56 * u, C_WHITE);
        line(fx, fy - 20 * u, fx, fy + 16 * u, 8 * u, C_ACCENT);
        line(fx - 16 * u, fy + 2 * u, fx, fy + 18 * u, 8 * u, C_ACCENT);
        line(fx + 16 * u, fy + 2 * u, fx, fy + 18 * u, 8 * u, C_ACCENT);
    }

    /* ── шапка ── */
    rect(dx, 0, W, CH_HDR * u, C_WHITE);
    ico_back(dx + 56 * u, CH_HDR * 0.5f * u, 26 * u, C_TEXT_MAIN);
    msg_avatar(dx + 148 * u, CH_HDR * 0.5f * u, 42 * u, c->color, c->name, c->saved);
    char name_fit[72];
    msg_text_fit(c->name, 0.58f, (int)(W - 220 * u - 40 * u), name_fit, sizeof(name_fit));
    text_scaled(name_fit, dx + 210 * u, 24 * u, C_TEXT_MAIN, 0.58f);
    const char *status = msg_last_seen(st.ci);
    uint32_t sc_col = (c->online || c->reply_phase == 2 || c->saved || c->group ||
                       strcmp(c->name, "Новости Enjoer") == 0) ? C_ACCENT : C_TEXT_HINT;
    text_scaled(status, dx + 210 * u, 78 * u, sc_col, 0.40f);
    rect(dx, CH_HDR * u - 2 * u, W, 2 * u, C_DIVIDER);

    /* ── поле ввода ── */
    float ib_top = H - CH_INPUT * u;
    rect(dx, ib_top, W, CH_INPUT * u, C_WHITE);
    rect(dx, ib_top, W, 2 * u, C_DIVIDER);
    float fld_x = dx + 28 * u;
    float fld_w = W - (28 + 116 + 28) * u;
    float fld_h = 96 * u;
    float fld_cy = ib_top + CH_INPUT * 0.5f * u - 8 * u;
    float fld_y = fld_cy - fld_h * 0.5f;
    roundrect(fld_x, fld_y, fld_w, fld_h, fld_h * 0.5f, C_SEARCH_BG);
    if (st.focus)
        ring(fld_x + fld_w * 0.5f, fld_cy, fld_w * 0.5f, 2.0f * u, 0x332AABEEu);
    ico_smile(fld_x + 52 * u, fld_cy, 26 * u, st.focus ? C_ACCENT : C_TEXT_HINT);
    float text_x = fld_x + 100 * u;
    float clip_x = fld_x + fld_w - 56 * u;
    if (st.draft[0]) {
        char fit[MSG_DRAFT_MAX];
        msg_text_fit(st.draft, 0.52f, (int)(clip_x - text_x - 60 * u), fit, sizeof(fit));
        text_scaled(fit, text_x, fld_cy - text_height(fit) * 0.52f * 0.5f - text_ink_top(fit) * 0.52f, C_TEXT_MAIN, 0.52f);
        if (((int)(msg_now() * 2)) % 2 == 0 && st.focus) {
            int tw = msg_text_w(fit, 0.52f);
            rect(text_x + tw + 4 * u, fld_y + 22 * u, 3 * u, fld_h - 44 * u, C_ACCENT);
        }
    } else {
        text_scaled("Сообщение", text_x, fld_cy - text_height("Сообщение") * 0.52f * 0.5f - text_ink_top("Сообщение") * 0.52f, C_TEXT_HINT, 0.52f);
    }
    ico_clip(clip_x, fld_cy, 25 * u, C_TEXT_HINT);

    /* кнопка отправки / микрофон */
    float sb_cx = dx + W - 28 * u - 56 * u, sb_cy = fld_cy;
    float sb_r = 56 * u * (st.press_send ? 0.92f : 1.0f);
    if (st.draft[0]) {
        circle(sb_cx, sb_cy + 2 * u, sb_r + 2 * u, 0x2A000000u);
        circle(sb_cx, sb_cy, sb_r, st.press_send ? C_ACCENT_DK : C_ACCENT);
        ico_send(sb_cx - 3 * u, sb_cy, 26 * u, C_WHITE);
    } else {
        ico_mic(sb_cx, sb_cy, 30 * u, C_TEXT_HINT);
    }
}

/* ── касания ───────────────────────────────────────────────────────────── */
int chat_touch(float x, float y, int action) {
    if (!st_open) return 0;
    float u = msg_u();
    float W = (float)screen_w, H = (float)screen_h;
    float vt = view_top(u), vb = view_bottom(u);

    if (action == 0) {
        st.down_x = x; st.down_y = y; st.moved = 0; st.vel = 0;
        st.dragging = 0;
        float fld_cy = H - CH_INPUT * 0.5f * u - 8 * u;
        st.press_send = st.draft[0] && fabsf(x - (W - 28 * u - 56 * u)) < 60 * u &&
                        fabsf(y - fld_cy) < 60 * u;
        st.press_back = x < 104 * u && y < CH_HDR * u;
        st.press_down = st.off > 350 * u && fabsf(x - (W - 92 * u)) < 60 * u &&
                        fabsf(y - (vb - 84 * u)) < 60 * u;
        st.press_field = 0;
        if (!st.press_send && !st.press_back && !st.press_down && y > H - CH_INPUT * u) {
            if (fabsf(x - (W - 28 * u - 56 * u)) >= 60 * u) st.press_field = 1;
        }
        return 1;
    }
    if (action == 2) {
        if (!st.dragging && fabsf(y - st.down_y) > 12 * u &&
            fabsf(y - st.down_y) > fabsf(x - st.down_x) && y > vt && y < vb) {
            st.dragging = 1;
            st.snap = 0;
            st.press_send = st.press_field = st.press_back = st.press_down = 0;
        }
        if (st.dragging) {
            float dy = y - st.down_y;
            st.off += dy;
            st.vel = st.vel * 0.6 + (double)dy / (dt > 0 ? dt : 0.016) * 0.4;
            st.down_y = y;
            st.down_x = x;
            st.moved = 1;
        }
        return 1;
    }
    if (action == 1) {
        int send_p = st.press_send, back_p = st.press_back, down_p = st.press_down, fld_p = st.press_field;
        st.press_send = st.press_back = st.press_down = st.press_field = 0;
        st.dragging = 0;
        if (st.moved) return 1;
        if (back_p && x < 104 * u && y < CH_HDR * u) {
            if (st.focus) { st.focus = 0; keyboard_hide(); }
            else { st_want_close = 1; } /* обработчик экранов запустит анимацию */
            return 1;
        }
        if (down_p) { st.snap = 1; return 1; }
        if (send_p) {
            char *trimmed = (char *)str_trim(st.draft);
            if (trimmed[0]) {
                msg_send(st.ci, trimmed);
                keyboard_clear();
                st.draft[0] = '\0';
                st.snap = 1;
            }
            return 1;
        }
        if (fld_p && y > H - CH_INPUT * u) {
            if (!st.focus) {
                st.focus = 1;
                /* «висящий» Enter от прошлого ввода не должен сразу отправлять */
                keyboard_enter_pressed();
                keyboard_show();
            }
            return 1;
        }
        /* тап по сообщению/обоям скрывает клавиатуру */
        if (st.focus && y > vt && y < vb) {
            st.focus = 0;
            keyboard_hide();
        }
        return 1;
    }
    return 1;
}
