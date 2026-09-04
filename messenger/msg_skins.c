/* Экран «Скины»: показывает скины только когда зашёл сюда.
 * Тут отображаются две текстуры зомби Азума: legacy_zombie_azum.png и legacy_zombie_azum_punch.png
 * Остальные слоты пока закрыты. */
#include "msg.h"
#include <string.h>
#include <math.h>
#include <stdio.h>

#define SK_HDR 132.0f
#define SK_MARGIN 28.0f

static int is_open = 0;
static int want_close = 0;
static double off = 0, vel = 0;
static int dragging = 0, moved = 0;
static float down_x = 0, down_y = 0;
static int press_back = 0;
static int selected = 0; // 0 = idle, 1 = punch, -1 = none
static float sel_anim = 0;

void skins_reset(void) {
    is_open = 0; want_close = 0; off = 0; vel = 0;
    dragging = 0; moved = 0; press_back = 0; selected = 0; sel_anim = 0;
}
int skins_is_open(void) { return is_open; }
int skins_wants_close(void) { return want_close; }
void skins_finish_close(void) { is_open = 0; want_close = 0; }
void skins_open(void) { is_open = 1; want_close = 0; off = 0; vel = 0; selected = 0; }

void skins_update(void) {
    if (!is_open) return;
    float u = msg_u();
    // контент: 2 больших карточки + сетка 2x2 мелких (заглушки)
    double content = 720*u + 520*u + 40*u; // две большие + два ряда мелких
    double view = screen_h - SK_HDR*u;
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
    sel_anim = lerp(sel_anim, selected >=0 ? 1.0f : 0.0f, 1.0f - pow(0.0001, dt));
}

static void draw_skin_card(float u, float dx, float x, float y, float w, float h, const char *tex_name, const char *title, const char *subtitle, int is_selected, int locked) {
    // тень
    roundrect(x, y+5*u, w, h, 26*u, 0x14000000u);
    uint32_t bg = locked ? 0xFFE9EDF2u : C_WHITE;
    if (is_selected && !locked) bg = 0xFFE8F4FFu;
    roundrect(x, y, w, h, 26*u, bg);
    if (is_selected && !locked) {
        // тонкая акцентная рамка вместо огромного круга
        roundrect(x-1*u, y-1*u, w+2*u, h+2*u, 27*u, C_ACCENT);
        roundrect(x, y, w, h, 26*u, bg);
        // галочка
        float cx = x + w - 36*u, cy = y + 36*u;
        circle(cx, cy+3*u, 22*u, 0x20000000u);
        circle(cx, cy, 22*u, C_ACCENT);
        ico_check(cx, cy, 12*u, C_WHITE);
    }
    if (locked) {
        // замок по центру
        float cx = x + w*0.5f, cy = y + h*0.42f;
        // иконка замка
        roundrect(cx - 28*u, cy - 6*u, 56*u, 44*u, 10*u, 0xFFC7CBD1u);
        circle(cx, cy - 22*u, 22*u, 0xFFC7CBD1u);
        circle(cx, cy - 22*u, 13*u, bg);
        rect(cx - 6*u, cy + 8*u, 12*u, 16*u, C_WHITE);
        circle(cx, cy + 8*u, 5*u, C_WHITE);
    } else if (tex_name) {
        // текстура скина — центрируем в карточке, правильный масштаб под 1254px
        float desired_h = h * 0.58f;
        float scale = desired_h / 1254.0f;
        float tex_w = 1254.0f * scale;
        float tx = x + w*0.5f - tex_w*0.5f;
        float ty = y + 22*u;
        // небольшой подиум
        float plat_y = ty + desired_h - 10*u;
        roundrect(x + w*0.5f - 66*u, plat_y, 132*u, 16*u, 8*u, 0x1A000000u);
        // сама текстура
        tex(tx, ty, tex_name, 0, scale);
    }
    // подпись
    if (locked) {
        msg_draw_text_c(title, x + w*0.5f, y + h - 56*u, 0.48f, C_TEXT_HINT);
        msg_draw_text_c(subtitle, x + w*0.5f, y + h - 28*u, 0.36f, C_TEXT_HINT);
    } else {
        msg_draw_text_c(title, x + w*0.5f, y + h - 56*u, 0.50f, C_TEXT_MAIN);
        text_scaled(subtitle, x + w*0.5f - msg_text_w(subtitle,0.38f)*0.38f*0.5f, y + h - 28*u, C_TEXT_SEC, 0.38f);
    }
}

void skins_draw(float dx) {
    if (!is_open) return;
    float u = msg_u(), W = (float)screen_w, H = (float)screen_h;
    rect(dx, 0, W, H, 0xFFF5F7FB);
    // header
    rect(dx, 0, W, SK_HDR*u, C_WHITE);
    ico_back(dx + 56*u, SK_HDR*0.5f*u, 26*u, C_TEXT_MAIN);
    text_scaled("Скины", dx + 122*u, SK_HDR*0.5f*u - text_height("Скины")*0.62f*0.5f - text_ink_top("Скины")*0.62f, C_TEXT_MAIN, 0.62f);
    // маленький бейдж "только здесь"
    {
        const char *badge = "только здесь";
        int tw = msg_text_w(badge, 0.34f);
        float bw = tw*0.34f + 28*u, bh = 34*u;
        float bx = dx + W - bw - 28*u, by = SK_HDR*0.5f*u - bh*0.5f;
        roundrect(bx, by, bw, bh, bh*0.5f, 0xFFE3F2FD);
        msg_draw_text_c(badge, bx + bw*0.5f, by + bh*0.5f+1*u, 0.34f, C_ACCENT);
    }
    rect(dx, SK_HDR*u -2*u, W, 2*u, C_DIVIDER);

    float top = SK_HDR*u + 28*u - (float)off;
    float margin = SK_MARGIN*u;
    float gap = 20*u;
    float cardW = (W - margin*2 - gap)*0.5f;
    float bigH = 460*u;
    float smallH = 380*u;

    // верхние две большие карточки — наши зомби скины, видны только тут
    {
        float y = top;
        // левая — idle
        float x0 = dx + margin;
        draw_skin_card(u, dx, x0, y, cardW, bigH, "legacy_zombie_azum.png", "Зомби Азум", "legacy · idle", selected==0, 0);
        // правая — punch
        float x1 = dx + margin + cardW + gap;
        draw_skin_card(u, dx, x1, y, cardW, bigH, "legacy_zombie_azum_punch.png", "Зомби Азум", "legacy · удар", selected==1, 0);
    }
    {
        float y = top + bigH + gap;
        float x0 = dx + margin;
        float x1 = dx + margin + cardW + gap;
        draw_skin_card(u, dx, x0, y, cardW, smallH, NULL, "Скоро", "закрыто", 0, 1);
        draw_skin_card(u, dx, x1, y, cardW, smallH, NULL, "Скоро", "закрыто", 0, 1);
    }
    {
        float y = top + bigH + smallH + gap*2;
        // подпись снизу
        const char *hint = "Скины отображаются только внутри этого экрана";
        msg_draw_text_c(hint, dx + W*0.5f, y + 24*u, 0.38f, C_TEXT_HINT);
        // превью выбранного скина крупно внизу?
        if (selected >=0) {
            float py = y + 64*u;
            float pw = W - margin*2;
            float ph = 240*u;
            roundrect(dx+margin, py, pw, ph, 20*u, C_WHITE);
            roundrect(dx+margin, py+4*u, pw, ph, 20*u, 0x0A000000u);
            const char *label = selected==0 ? "Выбран: Зомби Азум — обычный" : "Выбран: Зомби Азум — удар";
            text_scaled(label, dx+margin+24*u, py+20*u, C_TEXT_MAIN, 0.42f);
            // мини-текстура в углу превью — внутри карточки
            const char *skin_tex = selected==0 ? "legacy_zombie_azum.png" : "legacy_zombie_azum_punch.png";
            float sc2 = 110*u / 1254.0f;
            float tx2 = dx + W - margin - 110*u - 20*u;
            float ty2 = py + 28*u;
            tex(tx2, ty2, skin_tex, 0, sc2);
            text_scaled("Нажми на карточку чтобы выбрать", dx+margin+24*u, py+ 64*u, C_TEXT_SEC, 0.36f);
            // кнопка применить
            float btn_w = pw - 48*u, btn_h = 72*u;
            float bx = dx+margin+24*u, by = py+ ph - btn_h - 18*u;
            uint32_t bg =  C_ACCENT;
            roundrect(bx, by, btn_w, btn_h, btn_h*0.5f, bg);
            msg_draw_text_c("Применить", bx+btn_w*0.5f, by+btn_h*0.5f+1*u, 0.48f, C_WHITE);
        }
    }
}

int skins_touch(float x, float y, int action) {
    if (!is_open) return 0;
    float u = msg_u();
    float W = (float)screen_w;
    if (action == 0) {
        down_x = x; down_y = y; moved = 0; vel = 0; dragging = 0;
        press_back = (x < 110*u && y < SK_HDR*u);
        return 1;
    }
    if (action == 2) {
        if (!dragging && fabsf(y - down_y) > 14*u && fabsf(y-down_y) > fabsf(x-down_x)) {
            dragging = 1; press_back = 0;
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
        int was_back = press_back;
        press_back = 0; dragging = 0;
        if (moved) return 1;
        if (was_back && x < 110*u && y < SK_HDR*u) { want_close = 1; return 1; }
        // проверка тапа по карточкам
        float top = SK_HDR*u + 28*u - (float)off;
        float margin = SK_MARGIN*u, gap = 20*u;
        float cardW = (W - margin*2 - gap)*0.5f;
        float bigH = 460*u;
        float y0 = top;
        float x0 = margin, x1 = margin + cardW + gap;
        // левая зомби
        if (x >= x0 && x <= x0+cardW && y >= y0 && y <= y0+bigH) {
            selected = 0;
            snd_play("send.wav");
            return 1;
        }
        if (x >= x1 && x <= x1+cardW && y >= y0 && y <= y0+bigH) {
            selected = 1;
            snd_play("send.wav");
            return 1;
        }
        // проверка кнопки применить
        if (selected >=0) {
            float yb = top + bigH + 380*u + gap*2 + 64*u;
            float py = yb;
            float pw = W - margin*2;
            float ph = 240*u;
            float bx = margin+24*u, by = py+ ph - 72*u - 18*u;
            float btn_w = pw - 48*u, btn_h = 72*u;
            if (x >= bx && x <= bx+btn_w && y >= by && y <= by+btn_h) {
                snd_play("notify.wav");
                // тут можно сохранить выбранный скин
                return 1;
            }
        }
        return 1;
    }
    return 1;
}
