/* Экран «Уровни»: простой список уровней. Открывается левой кнопкой из списка чатов. */
#include "msg.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

#define LVL_HDR 132.0f
#define LVL_CARD_H 168.0f
#define LVL_MARGIN 40.0f

static int is_open = 0;
static int want_close = 0;
static double off = 0, vel = 0;
static int dragging = 0, moved = 0;
static float down_x = 0, down_y = 0;
static int press_back = 0;
static int press_card = -1;

typedef struct { const char *title; const char *desc; int stars; int locked; } LevelInfo;
static LevelInfo levels[] = {
    {"Уровень 1", "Обучение — знакомство с мессенеджером", 3, 0},
    {"Уровень 2", "Чаты и пузыри", 3, 0},
    {"Уровень 3", "Скины: зомби Азум", 2, 0},
    {"Уровень 4", "Босс: спам-атака", 1, 0},
    {"Уровень 5", "Секретный уровень", 0, 1},
};

void levels_reset(void) {
    is_open = 0; want_close = 0; off = 0; vel = 0;
    dragging = 0; moved = 0; press_back = 0; press_card = -1;
}
int levels_is_open(void) { return is_open; }
int levels_wants_close(void) { return want_close; }
void levels_finish_close(void) { is_open = 0; want_close = 0; }
void levels_open(void) { is_open = 1; want_close = 0; off = 0; vel = 0; }

void levels_update(void) {
    if (!is_open) return;
    float u = msg_u();
    int n = (int)(sizeof(levels)/sizeof(levels[0]));
    double view = screen_h - LVL_HDR * u;
    double content = n * (LVL_CARD_H + 20) * u + 40*u;
    double max_off = content - view;
    if (max_off < 0) max_off = 0;
    if (!dragging && fabs(vel) > 1.0) { off += vel * dt; vel *= pow(0.02, dt); }
    else if (!dragging) vel = 0;
    if (off < -80*u) off = -80*u;
    if (off > max_off + 80*u) off = max_off + 80*u;
    if (!dragging) {
        if (off < 0) off = lerp(off, 0, 1.0 - pow(0.0005, dt));
        if (off > max_off) off = lerp(off, max_off, 1.0 - pow(0.0005, dt));
    }
}

void levels_draw(float dx) {
    if (!is_open) return;
    float u = msg_u(), W = (float)screen_w, H = (float)screen_h;
    rect(dx, 0, W, H, 0xFFF5F7FB);
    // header
    rect(dx, 0, W, LVL_HDR * u, C_WHITE);
    ico_back(dx + 56*u, LVL_HDR*0.5f*u, 26*u, C_TEXT_MAIN);
    text_scaled("Уровни", dx + 122*u, LVL_HDR*0.5f*u - text_height("Уровни")*0.62f*0.5f - text_ink_top("Уровни")*0.62f, C_TEXT_MAIN, 0.62f);
    rect(dx, LVL_HDR*u - 2*u, W, 2*u, C_DIVIDER);

    int n = (int)(sizeof(levels)/sizeof(levels[0]));
    float top = LVL_HDR*u + 30*u - (float)off;
    for (int i = 0; i < n; i++) {
        float y = top + i * (LVL_CARD_H + 20)*u;
        if (y > H + 20*u || y + LVL_CARD_H*u < LVL_HDR*u) continue;
        float x = dx + LVL_MARGIN*u;
        float w = W - LVL_MARGIN*2*u;
        float h = LVL_CARD_H*u;
        // shadow
        roundrect(x, y+4*u, w, h, 22*u, 0x14000000u);
        uint32_t bg = levels[i].locked ? 0xFFE8ECF1u : C_WHITE;
        if (press_card == i) bg = 0xFFF0F3F8;
        roundrect(x, y, w, h, 22*u, bg);
        // номер уровня кружок слева
        float cx = x + 56*u, cy = y + h*0.5f;
        if (levels[i].locked) {
            circle(cx, cy, 42*u, 0xFFC7CBD1u);
            // замок: корпус
            float lx = cx, ly = cy;
            roundrect(lx - 18*u, ly - 2*u, 36*u, 28*u, 6*u, C_WHITE);
            circle(lx, ly - 14*u, 16*u, C_WHITE);
            circle(lx, ly - 14*u, 10*u, 0xFFC7CBD1u);
        } else {
            circle(cx, cy, 42*u, C_ACCENT);
            char num[4]; snprintf(num, sizeof(num), "%d", i+1);
            msg_draw_text_c(num, cx, cy+2*u, 0.58f, C_WHITE);
        }
        // текст
        float tx = x + 118*u;
        text_scaled(levels[i].title, tx, y + 34*u, levels[i].locked ? C_TEXT_HINT : C_TEXT_MAIN, 0.54f);
        text_scaled(levels[i].desc, tx, y + 78*u, C_TEXT_SEC, 0.40f);
        // звёзды
        if (!levels[i].locked) {
            float sx = x + w - 40*u;
            for (int s = 0; s < 3; s++) {
                uint32_t col = s < levels[i].stars ? 0xFFFFC107u : 0xFFE0E0E0u;
                // звезда упрощённо как круг с точкой
                circle(sx - s*34*u, y + 36*u, 12*u, col);
                if (s < levels[i].stars) circle(sx - s*34*u, y + 36*u, 4*u, C_WHITE);
            }
        } else {
            text_scaled("закрыто", x + w - msg_text_w("закрыто",0.38f)*0.38f - 36*u, y + 30*u, C_TEXT_HINT, 0.38f);
        }
        // прогресс бар внизу карточки
        if (!levels[i].locked) {
            float bar_x = tx, bar_w = w - (tx - x) - 36*u, bar_h = 10*u, bar_y = y + h - 36*u;
            roundrect(bar_x, bar_y, bar_w, bar_h, bar_h*0.5f, 0xFFE9ECEF);
            float fill = 0.0f;
            if (i==0) fill=1.0f; else if (i==1) fill=0.75f; else if (i==2) fill=0.45f; else if (i==3) fill=0.15f;
            roundrect(bar_x, bar_y, bar_w*fill, bar_h, bar_h*0.5f, C_ACCENT);
        }
    }
}

int levels_touch(float x, float y, int action) {
    if (!is_open) return 0;
    float u = msg_u();
    float W = (float)screen_w;
    if (action == 0) {
        down_x = x; down_y = y; moved = 0; vel = 0; dragging = 0;
        press_back = (x < 110*u && y < LVL_HDR*u);
        if (!press_back) {
            int n = (int)(sizeof(levels)/sizeof(levels[0]));
            float top = LVL_HDR*u + 30*u - (float)off;
            press_card = -1;
            for (int i=0;i<n;i++) {
                float yy = top + i*(LVL_CARD_H+20)*u;
                float xx = LVL_MARGIN*u;
                float ww = W - LVL_MARGIN*2*u;
                if (x >= xx && x <= xx+ww && y >= yy && y <= yy+LVL_CARD_H*u) { press_card = i; break; }
            }
        }
        return 1;
    }
    if (action == 2) {
        if (!dragging && fabsf(y - down_y) > 14*u && fabsf(y-down_y) > fabsf(x-down_x)) {
            dragging = 1; press_back = 0; press_card = -1;
        }
        if (dragging) {
            float dy = y - down_y;
            off -= dy;
            vel = vel*0.6 + (double)(-dy)/(dt>0?dt:0.016)*0.4;
            down_y = y; down_x = x; moved = 1;
        }
        return 1;
    }
    if (action == 1) {
        int was_back = press_back, was_card = press_card;
        press_back = 0; press_card = -1; dragging = 0;
        if (moved) return 1;
        if (was_back && x < 110*u && y < LVL_HDR*u) { want_close = 1; return 1; }
        if (was_card >=0) {
            // тап по уровню — пока просто звук
            // если не заблокирован
            if (!levels[was_card].locked) snd_play("send.wav");
            return 1;
        }
        return 1;
    }
    return 1;
}
