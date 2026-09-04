/* Экран «Список чатов»: шапка с поиском, строки чатов с аватарками,
 * счётчики непрочитанных. Внизу — две кнопки: УРОВНИ слева и СКИНЫ справа.
 * Скролл с инерцией и фильтром поиска. Два старых класса (Дима и Новости) убраны. */
#include "msg.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HEADER_H   200.0f   /* в дизайн-единицах (ширина 1080) */
#define ROW_H      148.0f
#define MARGIN      40.0f
#define BTM_H      168.0f   /* высота нижней панели с двумя кнопками */
#define BTN_H       96.0f

typedef struct {
    double off;            /* смещение списка (0 = верх) */
    double vel;
    int dragging;
    float down_x, down_y;
    int moved;
    int press_row;         /* строка под пальцем */
    int press_levels;
    int press_skins;
    int search_active;
    char search[128];
    int visible[MSG_MAX_CHATS];
    int visible_n;
} ChatsState;

static ChatsState cs;

void chats_reset(void) {
    memset(&cs, 0, sizeof(cs));
    cs.press_row = -1;
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
    double view = screen_h - HEADER_H * msg_u() - BTM_H * msg_u();
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

static void draw_bottom_bar(float u, float dx) {
    float W = (float)screen_w, H = (float)screen_h;
    float bar_top = H - BTM_H * u;
    // фон панели
    rect(dx, bar_top, W, BTM_H * u, C_WHITE);
    rect(dx, bar_top, W, 2 * u, C_DIVIDER);
    // размеры кнопок
    float gap = MARGIN * u;
    float btn_w = (W - gap * 3) * 0.5f;
    float btn_h = BTN_H * u;
    float by = bar_top + (BTM_H * u - btn_h) * 0.5f;
    float left_x = dx + gap;
    float right_x = dx + gap * 2 + btn_w;

    // тень-подложка когда зажато чуть меньше
    float lpress = cs.press_levels ? 1 : 0;
    float rpress = cs.press_skins ? 1 : 0;

    // левая кнопка "УРОВНИ" — светлая, с иконкой уровней
    {
        float sc = lpress ? 0.97f : 1.0f;
        float w = btn_w * sc;
        float h = btn_h * sc;
        float x = left_x + (btn_w - w)*0.5f;
        float y = by + (btn_h - h)*0.5f;
        uint32_t bg = lpress ? 0xFFE9E9E9u : C_SEARCH_BG;
        // лёгкая тень
        roundrect(x, y + 3*u, w, h, h*0.5f, 0x1A000000u);
        roundrect(x, y, w, h, h*0.5f, bg);
        if (lpress) {
            // обводка при нажатии
            ring(x + w*0.5f, y + h*0.5f, h*0.5f - 1*u, 1.4f*u, 0x22000000u);
        }
        // иконка уровней: 3 горизонтальные полоски слева
        float icx = x + 36*u;
        float icy = y + h*0.5f;
        float t = 5*u;
        line(icx - 14*u, icy - 14*u, icx + 14*u, icy - 14*u, t, C_TEXT_SEC);
        line(icx - 14*u, icy,        icx + 14*u, icy,        t, C_TEXT_SEC);
        line(icx - 14*u, icy + 14*u, icx + 14*u, icy + 14*u, t, C_TEXT_SEC);
        // текст
        const char *lab = "УРОВНИ";
        int tw = msg_text_w(lab, 0.48f);
        float tx = x + w*0.5f + 10*u - tw*0.5f*0.48f; // чуть правее иконки
        // центрируем с учётом иконки: вычисляем смещение
        float content_w = 28*u + tw*0.48f;
        float start = x + (w - content_w)*0.5f;
        // перерисуем иконку и текст выровненно
        // иконка уже, теперь текст
        text_scaled(lab, start + 28*u, y + h*0.5f - text_height(lab)*0.48f*0.5f - text_ink_top(lab)*0.48f, C_TEXT_MAIN, 0.48f);
    }
    // правая кнопка "СКИНЫ" — акцентная синяя
    {
        float sc = rpress ? 0.97f : 1.0f;
        float w = btn_w * sc;
        float h = btn_h * sc;
        float x = right_x + (btn_w - w)*0.5f;
        float y = by + (btn_h - h)*0.5f;
        uint32_t bg = rpress ? C_ACCENT_DK : C_ACCENT;
        roundrect(x, y + 4*u, w, h, h*0.5f, 0x30000000u);
        roundrect(x, y, w, h, h*0.5f, bg);
        // иконка палитры: кружок + маленькая точка
        float icx = x + 38*u;
        float icy = y + h*0.5f;
        // палитра упрощённо: кружок + блик
        circle(icx, icy, 16*u, C_WHITE);
        circle(icx + 5*u, icy - 5*u, 4*u, bg);
        circle(icx - 6*u, icy - 2*u, 3*u, 0xFFFFD54Fu);
        circle(icx + 2*u, icy + 7*u, 2.5f*u, 0xFFEF5350u);
        const char *lab = "СКИНЫ";
        int tw = msg_text_w(lab, 0.50f);
        float content_w = 32*u + tw*0.50f;
        float start = x + (w - content_w)*0.5f;
        text_scaled(lab, start + 32*u, y + h*0.5f - text_height(lab)*0.50f*0.5f - text_ink_top(lab)*0.50f, C_WHITE, 0.50f);
    }
}

void chats_draw(float dx) {
    float u = msg_u(), W = (float)screen_w, H = (float)screen_h;
    rect(dx, 0, W, H, C_WHITE);
    rebuild_filter();

    float top = row_top(u) - (float)cs.off;
    float bottom_limit = H - BTM_H * u;
    for (int r = 0; r < cs.visible_n; r++) {
        float y = top + r * ROW_H * u;
        if (y > bottom_limit) break;
        if (y + ROW_H * u < HEADER_H * u) continue;
        draw_row(u, dx, cs.visible[r], y);
    }
    if (cs.visible_n == 0) {
        msg_draw_text_c(cs.search[0] ? "Ничего не найдено" : "Нет чатов",
                        W * 0.5f + dx, H * 0.45f - 40*u, 0.55f, C_TEXT_HINT);
    }

    draw_header(u, dx);
    draw_bottom_bar(u, dx);
}

/* ── касания ───────────────────────────────────────────────────────────── */
static int hit_left_btn(float x, float y, float u) {
    float W = (float)screen_w, H = (float)screen_h;
    float bar_top = H - BTM_H * u;
    if (y < bar_top) return 0;
    float gap = MARGIN * u;
    float btn_w = (W - gap * 3) * 0.5f;
    float btn_h = BTN_H * u;
    float by = bar_top + (BTM_H * u - btn_h) * 0.5f;
    float left_x = gap;
    return x >= left_x && x <= left_x + btn_w && y >= by && y <= by + btn_h;
}
static int hit_right_btn(float x, float y, float u) {
    float W = (float)screen_w, H = (float)screen_h;
    float bar_top = H - BTM_H * u;
    if (y < bar_top) return 0;
    float gap = MARGIN * u;
    float btn_w = (W - gap * 3) * 0.5f;
    float btn_h = BTN_H * u;
    float by = bar_top + (BTM_H * u - btn_h) * 0.5f;
    float right_x = gap * 2 + btn_w;
    return x >= right_x && x <= right_x + btn_w && y >= by && y <= by + btn_h;
}

int chats_touch(float x, float y, int action) {
    float u = msg_u();
    (void)u;
    if (action == 0) { /* DOWN */
        cs.down_x = x; cs.down_y = y; cs.moved = 0; cs.vel = 0;
        cs.dragging = 0;
        // проверка нижних кнопок в приоритете
        if (hit_left_btn(x, y, u)) { cs.press_levels = 1; cs.press_row = -1; return 1; }
        if (hit_right_btn(x, y, u)) { cs.press_skins = 1; cs.press_row = -1; return 1; }
        // проверка бара низа — не скроллить если нажали бар
        float H = (float)screen_h;
        if (y > H - BTM_H * u) { cs.press_row = -1; return 1; }
        cs.press_row = row_at(y, u);
        return 1;
    }
    if (action == 2) { /* MOVE */
        if (cs.press_levels || cs.press_skins) {
            // если палец ушёл с кнопки — снять нажатие
            if (cs.press_levels && !hit_left_btn(x, y, u)) cs.press_levels = 0;
            if (cs.press_skins && !hit_right_btn(x, y, u)) cs.press_skins = 0;
            return 1;
        }
        if (!cs.dragging && fabsf(y - cs.down_y) > 12 * u && fabsf(y - cs.down_y) > fabsf(x - cs.down_x)) {
            cs.dragging = 1;
            cs.press_row = -1;
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
        int was_levels = cs.press_levels, was_skins = cs.press_skins, was_row = cs.press_row;
        cs.press_levels = 0; cs.press_skins = 0;
        cs.press_row = -1;
        cs.dragging = 0;
        if (was_levels && hit_left_btn(x, y, u)) {
            levels_open();
            return 1;
        }
        if (was_skins && hit_right_btn(x, y, u)) {
            skins_open();
            return 1;
        }
        if (was_levels || was_skins) return 1;
        // тап на нижней панели вне кнопок — игнор
        float H = (float)screen_h;
        if (y > H - BTM_H * u) return 1;
        if (cs.moved) return 1;
        /* одиночные тапы */
        if (y < HEADER_H * u) {
            float px = MARGIN * u, pw = (float)screen_w - MARGIN * 2 * u;
            float ph = 68 * u, py = 116 * u;
            if (cs.search_active) {
                if (x < 100 * u && y < 104 * u) {
                    cs.search_active = 0;
                    keyboard_hide();
                    return 1;
                }
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
