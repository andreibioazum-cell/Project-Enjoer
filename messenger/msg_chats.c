/* Экран «Список чатов»: шапка с поиском, строки чатов с аватарками,
 * счётчики непрочитанных, FAB-кнопка. Скролл с инерцией и фильтром поиска. */
#include "msg.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HEADER_H   200.0f   /* в дизайн-единицах (ширина 1080) */
#define ROW_H      148.0f
#define FAB_D      132.0f
#define MARGIN      40.0f

typedef struct {
    double off;            /* смещение списка (0 = верх) */
    double vel;
    int dragging;
    float down_x, down_y;
    int moved;
    int press_row;         /* строка под пальцем */
    int press_fab;
    int search_active;
    char search[128];
    int visible[MSG_MAX_CHATS];
    int visible_n;
    double fab_scale;      /* анимация FAB */
} ChatsState;

static ChatsState cs;

void chats_reset(void) {
    memset(&cs, 0, sizeof(cs));
    cs.press_row = -1;
    cs.fab_scale = 1.0;
}

static float row_top(float u) { return HEADER_H * u; }

/* Нижняя папка для кодовой точки (латиница + кириллица). */
static unsigned cp_lower(unsigned cp) {
    if (cp >= 'A' && cp <= 'Z') return cp + 32;
    if (cp >= 0x0410 && cp <= 0x042F) return cp + 0x20;  /* А-Я → а-я */
    if (cp == 0x0401) return 0x0451;                       /* Ё → ё */
    return cp;
}

static unsigned utf8_take(const char **p) {
    const unsigned char *s = (const unsigned char *)*p;
    unsigned cp;
    if (*s < 0x80) cp = *s++;
    else if ((*s & 0xE0) == 0xC0) { cp = ((s[0] & 0x1F) << 6) | (s[1] & 0x3F); s += 2; }
    else if ((*s & 0xF0) == 0xE0) { cp = ((s[0] & 0x0F) << 12) | ((s[1] & 0x3F) << 6) | (s[2] & 0x3F); s += 3; }
    else { cp = ((s[0] & 0x07) << 18) | ((s[1] & 0x3F) << 12) | ((s[2] & 0x3F) << 6) | (s[3] & 0x3F); s += 4; }
    *p = (const char *)s;
    return cp;
}

/* Регистронезависимый поиск подстроки без аллокаций (строчный пул рантайма
 * трогать нельзя — он очищается только на рестарте, покадровые вызовы утекают). */
static int ci_contains(const char *hay, const char *needle) {
    if (!needle || !*needle) return 1;
    if (!hay) return 0;
    for (const char *h = hay; *h; ) {
        const char *hh = h, *nn = needle;
        int ok = 1;
        while (*hh && *nn) {
            unsigned a = cp_lower(utf8_take(&hh));
            unsigned b = cp_lower(utf8_take(&nn));
            if (a != b) { ok = 0; break; }
        }
        if (ok && !*nn) return 1;
        utf8_take(&h);
    }
    return 0;
}

static void rebuild_filter(void) {
    cs.visible_n = 0;
    for (int i = 0; i < msg_chat_count && cs.visible_n < MSG_MAX_CHATS; i++) {
        if (cs.search_active && cs.search[0] && !ci_contains(msg_chats[i].name, cs.search))
            continue;
        cs.visible[cs.visible_n++] = i;
    }
}

static int row_at(float y, float u) {
    float rel = y - (row_top(u) - (float)cs.off);
    if (rel < 0) return -1;
    int r = (int)(rel / (ROW_H * u));
    return r < cs.visible_n ? r : -1;
}

void chats_update(void) {
    rebuild_filter();
    /* инерция скролла */
    if (!cs.dragging && fabs(cs.vel) > 1.0) {
        cs.off -= cs.vel * dt;
        cs.vel *= pow(0.02, dt); /* затухание */
    } else if (!cs.dragging) {
        cs.vel = 0;
    }
    double content = (double)cs.visible_n * ROW_H * msg_u();
    double view = screen_h - HEADER_H * msg_u();
    double max_off = content - view;
    if (max_off < 0) max_off = 0;
    /* мягкий возврат при перетягивании */
    if (cs.off < -80 * msg_u()) cs.off = -80 * msg_u();
    if (cs.off > max_off + 80 * msg_u()) cs.off = max_off + 80 * msg_u();
    if (!cs.dragging) {
        if (cs.off < 0) cs.off = lerp(cs.off, 0, 1.0 - pow(0.0005, dt));
        if (cs.off > max_off) cs.off = lerp(cs.off, max_off, 1.0 - pow(0.0005, dt));
    }
    /* текст поиска из клавиатуры */
    if (cs.search_active) {
        snprintf(cs.search, sizeof(cs.search), "%s", keyboard_get_raw());
        if (keyboard_enter_pressed()) { cs.search_active = 0; keyboard_hide(); }
    }
    cs.fab_scale = lerp(cs.fab_scale, cs.press_fab ? 0.90 : 1.0, 1.0 - pow(0.0001, dt));
}

/* ── отрисовка ─────────────────────────────────────────────────────────── */
static void draw_header(float u, float dx) {
    float W = (float)screen_w;
    rect(dx, 0, W, HEADER_H * u, C_WHITE);
    /* заголовок */
    if (cs.search_active)
        ico_back(dx + 52 * u, 52 * u, 26 * u, C_TEXT_MAIN);
    else
        ico_menu(dx + 52 * u, 52 * u, 26 * u, C_TEXT_MAIN);
    text_scaled("Enjoer", dx + 116 * u, 52 * u - text_height("Enjoer") * 0.66f * 0.5f, C_TEXT_MAIN, 0.66f);
    if (!cs.search_active)
        ico_search(dx + W - 64 * u, 52 * u, 27 * u, C_TEXT_SEC);

    /* поисковая плашка */
    float px = dx + MARGIN * u, pw = W - MARGIN * 2 * u;
    float py = 116 * u, ph = 68 * u;
    roundrect(px, py, pw, ph, ph * 0.5f, C_SEARCH_BG);
    if (cs.search_active) {
        /* тонкая акцентная обводка сфокусированной плашки */
        ring(px + ph * 0.5f, py + ph * 0.5f, ph * 0.5f - 1.0f * u, 1.6f * u, 0x662AABEEu);
        ring(px + pw - ph * 0.5f, py + ph * 0.5f, ph * 0.5f - 1.0f * u, 1.6f * u, 0x662AABEEu);
        rect(px + ph * 0.5f, py, pw - ph, 1.6f * u, 0x662AABEEu);
        rect(px + ph * 0.5f, py + ph - 1.6f * u, pw - ph, 1.6f * u, 0x662AABEEu);
    }
    float icx = px + 42 * u, icy = py + ph * 0.5f;
    if (!cs.search_active) {
        ico_search(icx, icy, 20 * u, C_TEXT_HINT);
        text_scaled("Поиск", px + 80 * u, py + ph * 0.5f - text_height("Поиск") * 0.5f * 0.5f, C_TEXT_HINT, 0.50f);
    } else {
        ico_search(icx, icy, 20 * u, C_ACCENT);
        if (cs.search[0]) {
            char fit[160];
            msg_text_fit(cs.search, 0.50f, (int)(pw - 200 * u), fit, sizeof(fit));
            text_scaled(fit, px + 80 * u, py + ph * 0.5f - text_height(fit) * 0.5f * 0.5f, C_TEXT_MAIN, 0.50f);
            /* мигающий курсор */
            if (((int)(msg_now() * 2)) % 2 == 0) {
                int tw = msg_text_w(fit, 0.50f);
                rect(px + 80 * u + tw + 4 * u, py + 16 * u, 3 * u, ph - 32 * u, C_ACCENT);
            }
            ico_close(px + pw - 44 * u, icy, 17 * u, C_TEXT_HINT);
        } else {
            text_scaled("Поиск", px + 80 * u, py + ph * 0.5f - text_height("Поиск") * 0.5f * 0.5f, C_TEXT_HINT, 0.50f);
        }
    }
    rect(dx, HEADER_H * u - 2 * u, W, 2 * u, C_DIVIDER);
}

static void draw_row(float u, float dx, int ci, float y) {
    MsgChat *c = &msg_chats[ci];
    float W = (float)screen_w;
    float cx_av = dx + (MARGIN + 52) * u;
    float cy_av = y + ROW_H * 0.5f * u;
    float tx = dx + (MARGIN + 52 + 52 + 28) * u;   /* левый край текста */
    float right = dx + W - MARGIN * u;

    if (cs.press_row >= 0 && cs.visible[cs.press_row] == ci && cs.dragging == 0)
        rect(dx, y, W, ROW_H * u, C_PRESS);

    /* время / бейдж справа вверху */
    char ts[32];
    double last_t = 0;
    int typing = (c->reply_phase == 2);
    if (c->count > 0) {
        last_t = c->msgs[c->count - 1].t;
    }
    msg_fmt_time(last_t, ts, sizeof(ts));
    int badge = c->unread > 0;
    float time_right = right;
    if (badge) {
        char cnt[16];
        snprintf(cnt, sizeof(cnt), "%d", c->unread > 99 ? 99 : c->unread);
        int cw = msg_text_w(cnt, 0.36f);
        float bw = (float)cw + 34 * u;
        if (bw < 44 * u) bw = 44 * u;
        float bx = right - bw, by = y + 26 * u, bh = 44 * u;
        roundrect(bx, by, bw, bh, bh * 0.5f, c->muted ? 0xFFC7CBD1u : C_GREEN);
        msg_draw_text_c(cnt, bx + bw * 0.5f, by + bh * 0.5f + 1 * u, 0.36f, C_WHITE);
        time_right = bx - 14 * u;
    } else if (c->muted) {
        ico_mute(right - 22 * u, y + 48 * u, 20 * u, 0xFFC7CBD1u);
        time_right = right - 54 * u;
    }
    if (last_t > 0) {
        char fit[40];
        msg_text_fit(ts, 0.38f, (int)(time_right - tx), fit, sizeof(fit));
        int tw = msg_text_w(fit, 0.38f);
        text_scaled(fit, time_right - tw, y + 34 * u, typing ? C_ACCENT : C_TEXT_HINT, 0.38f);
    }

    /* имя */
    char name_fit[80];
    msg_text_fit(c->name, 0.55f, (int)(time_right - 20 * u - tx), name_fit, sizeof(name_fit));
    text_scaled(name_fit, tx, y + 26 * u, C_TEXT_MAIN, 0.55f);

    /* превью последнего сообщения */
    char line[192];
    if (typing) {
        snprintf(line, sizeof(line), "печатает…");
    } else if (c->count == 0) {
        line[0] = '\0';
    } else {
        MsgMessage *m = &c->msgs[c->count - 1];
        if (c->group) {
            /* в группах показываем автора до двоеточия */
            const char *colon = strchr(m->text, ':');
            if (colon && !m->out && colon - m->text < 24 && colon - m->text > 1) {
                char author[28];
                int an = (int)(colon - m->text);
                memcpy(author, m->text, (size_t)an);
                author[an] = '\0';
                char rest[160];
                msg_text_fit(colon + 2, 0.47f, (int)(right - tx - 90 * u), rest, sizeof(rest));
                snprintf(line, sizeof(line), "%s: %s", author, rest);
            } else {
                msg_text_fit(m->text, 0.47f, (int)(right - tx - 90 * u), line, sizeof(line));
            }
        } else if (m->out && !c->saved) {
            char rest[160];
            msg_text_fit(m->text, 0.47f, (int)(right - tx - 90 * u), rest, sizeof(rest));
            snprintf(line, sizeof(line), "Вы: %s", rest);
        } else {
            msg_text_fit(m->text, 0.47f, (int)(right - tx - 90 * u), line, sizeof(line));
        }
    }
    if (typing)
        text_scaled(line, tx, y + 88 * u, C_ACCENT, 0.47f);
    else if (line[0])
        text_scaled(line, tx, y + 88 * u, C_TEXT_SEC, 0.47f);

    /* разделитель */
    rect(tx, y + ROW_H * u - 1.5f * u, W - (MARGIN + 132) * u, 1.5f * u, C_DIVIDER);

    /* аватарка поверх текста строки */
    msg_avatar(cx_av, cy_av, 52 * u, c->color, c->name, c->saved);
    if (c->online && !c->saved && !c->group) {
        circle(cx_av + 36 * u, cy_av + 36 * u, 16 * u, C_WHITE);
        circle(cx_av + 36 * u, cy_av + 36 * u, 11 * u, C_ONLINE);
    }
}

void chats_draw(float dx) {
    float u = msg_u(), W = (float)screen_w, H = (float)screen_h;
    rect(dx, 0, W, H, C_WHITE);
    rebuild_filter();

    float top = row_top(u) - (float)cs.off;
    for (int r = 0; r < cs.visible_n; r++) {
        float y = top + r * ROW_H * u;
        if (y > H) break;
        if (y + ROW_H * u < HEADER_H * u) continue;
        draw_row(u, dx, cs.visible[r], y);
    }
    if (cs.visible_n == 0) {
        msg_draw_text_c(cs.search[0] ? "Ничего не найдено" : "Нет чатов",
                        W * 0.5f + dx, H * 0.45f, 0.55f, C_TEXT_HINT);
    }

    draw_header(u, dx);

    /* FAB «новое сообщение» */
    float fs = (float)cs.fab_scale;
    float fx = dx + W - (MARGIN + FAB_D * 0.5f) * u;
    float fy = H - (MARGIN + FAB_D * 0.5f) * u;
    if (fs != 1.0f) {
        circle(fx, fy, FAB_D * 0.5f * u * fs + 4 * u, 0x22000000u);
    } else {
        circle(fx, fy + 3 * u, FAB_D * 0.5f * u + 3 * u, 0x2A000000u);
    }
    circle(fx, fy, FAB_D * 0.5f * u * fs, cs.press_fab ? C_ACCENT_DK : C_ACCENT);
    ico_pencil(fx, fy, 34 * u * fs, C_WHITE);
}

/* ── касания ───────────────────────────────────────────────────────────── */
static float fab_dist(float x, float y) {
    float u = msg_u();
    float fx = (float)screen_w - (MARGIN + FAB_D * 0.5f) * u;
    float fy = (float)screen_h - (MARGIN + FAB_D * 0.5f) * u;
    return sqrtf((x - fx) * (x - fx) + (y - fy) * (y - fy));
}

int chats_touch(float x, float y, int action) {
    float u = msg_u();
    (void)u;
    if (action == 0) { /* DOWN */
        cs.down_x = x; cs.down_y = y; cs.moved = 0; cs.vel = 0;
        cs.dragging = 0;
        cs.press_row = row_at(y, u);
        cs.press_fab = fab_dist(x, y) < FAB_D * 0.5f * u + 10 * u;
        /* шапка: иконка поиска */
        return 1;
    }
    if (action == 2) { /* MOVE */
        if (!cs.dragging && fabsf(y - cs.down_y) > 12 * u && fabsf(y - cs.down_y) > fabsf(x - cs.down_x)) {
            cs.dragging = 1;
            cs.press_row = -1;
            cs.press_fab = 0;
        }
        if (cs.dragging) {
            float step = cs.down_y - y;
            cs.off += step;
            cs.vel = cs.vel * 0.6 + (double)step / (dt > 0 ? dt : 0.016) * 0.4;
            cs.down_y = y;
            cs.down_x = x;
            cs.moved = 1;
        }
        return 1;
    }
    if (action == 1) { /* UP */
        int was_row = cs.press_row, was_fab = cs.press_fab;
        cs.press_row = -1;
        cs.press_fab = 0;
        cs.dragging = 0;
        if (cs.moved) return 1;
        /* одиночные тапы */
        if (y < HEADER_H * u) {
            float px = MARGIN * u, pw = (float)screen_w - MARGIN * 2 * u;
            float ph = 68 * u, py = 116 * u;
            if (cs.search_active) {
                /* назад из поиска */
                if (x < 100 * u && y < 104 * u) {
                    cs.search_active = 0;
                    keyboard_hide();
                    return 1;
                }
                /* крестик очистки */
                if (x > px + pw - 80 * u && y > py && y < py + ph && cs.search[0]) {
                    keyboard_clear();
                    cs.search[0] = '\0';
                    return 1;
                }
                return 1;
            }
            if (y > py && y < py + ph && x > px && x < px + pw) {
                cs.search_active = 1;
                keyboard_clear();
                cs.search[0] = '\0';
                keyboard_show();
                return 1;
            }
            if (x > (float)screen_w - 100 * u && y < 104 * u) {
                cs.search_active = 1;
                keyboard_clear();
                cs.search[0] = '\0';
                keyboard_show();
                return 1;
            }
            return 1;
        }
        if (was_fab && fab_dist(x, y) < FAB_D * 0.5f * u + 14 * u) {
            /* «новое сообщение»: открываем первый чат как демо */
            if (msg_chat_count > 0) chat_open(cs.search_active && cs.visible_n ? cs.visible[0] : 0);
            return 1;
        }
        if (was_row >= 0 && was_row < cs.visible_n) {
            int r = row_at(y, u);
            if (r == was_row) {
                chat_open(cs.visible[r]);
                return 1;
            }
        }
        return 1;
    }
    return 1;
}
