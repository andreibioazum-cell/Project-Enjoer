/* rbx/rbx_hud.c — HUD первого лица: монеты, чёрный стик полёта,
 * прицел и подсказка свайпа. Справа нет кнопки, мешающей обзору. */
#include "rbx_internal.h"
#include <math.h>
#include <stdio.h>

static void text_c(const char *s, float cx, float cy, float sc, uint32_t col) {
    int w = text_width(s);
    text_scaled(s, cx - (w * sc) * 0.5f, cy, col, sc);
}

void rbx_hud_draw(void) {
    float W = (float)screen_w, H = (float)screen_h;
    float u = fminf(W / 420.0f, H / 600.0f);

    /* верхняя панель */
    rect(0, 0, W, 50.0f * u, 0xE8232327u);
    roundrect(10 * u, 9 * u, 32 * u, 32 * u, 7 * u, 0xFFE2231Au);
    roundrect(17 * u, 16 * u, 18 * u, 18 * u, 4 * u, 0xFFFFFFFFu);
    text_scaled("Obby Park", 50 * u, 14 * u, 0xFFFFFFFFu, 0.62f * u);

    char buf[40];
    snprintf(buf, sizeof(buf), "монеты  %d/%d", rbx_world_caught(), rbx_world_coin_count());
    int tw = text_width(buf);
    text_scaled(buf, W - tw * 0.55f * u - 14 * u, 16 * u, 0xFFFFF59Du, 0.55f * u);

    /* Чёрная база и чёрная ручка, тонкие серые края для читаемости. */
    float joy_cx, joy_cy, joy_r, jx, jy;
    rbx_input_joy_geom(&joy_cx, &joy_cy, &joy_r);
    rbx_input_joy(&jx, &jy);
    circle(joy_cx, joy_cy, joy_r, 0xD6000000u);
    ring(joy_cx, joy_cy, joy_r, 2.0f * u, 0xBB3A3A3Au);
    ring(joy_cx, joy_cy, joy_r * 0.65f, 1.0f * u, 0x663C3C3Cu);
    float knob_x = joy_cx + jx * joy_r * 0.52f;
    float knob_y = joy_cy + jy * joy_r * 0.52f;
    circle(knob_x, knob_y, joy_r * 0.38f, 0xFF000000u);
    ring(knob_x, knob_y, joy_r * 0.38f, 2.0f * u, 0xFF444444u);
    text_c("Полёт", joy_cx, joy_cy + joy_r + 6 * u, 0.36f * u, 0xFFFFFFFFu);

    /* Это только подпись: вся правая половина экрана принимает свайп. */
    float look_x = W - 84 * u;
    roundrect(look_x - 64 * u, joy_cy - 20 * u, 128 * u, 48 * u, 10 * u, 0xC01A1A1Eu);
    text_c("Обзор", look_x, joy_cy - 14 * u, 0.42f * u, 0xFFFFFFFFu);
    text_c("свайп справа", look_x, joy_cy + 7 * u, 0.30f * u, 0xFFBDBDBDu);

    /* Небольшая точка показывает направление взгляда/полёта. */
    circle(W * 0.5f, H * 0.5f, 3.4f * u, 0xAA000000u);
    circle(W * 0.5f, H * 0.5f, 1.6f * u, 0xEEFFFFFFu);

    /* В первом лице своё имя и свою модель не рисуем. */
    float sx, sy;
    static const char *names[MAX_BOT] = { "Mila", "Rob", "Alex" };
    const RbxBot *bots = rbx_world_bots();
    for (int i = 0; i < MAX_BOT; i++) {
        if (rbx3d_project(bots[i].x, bots[i].y + 5.4f, bots[i].z, &sx, &sy))
            text_c(names[i], sx, sy - 8, 0.40f * u, 0xFFE0E0E0u);
    }

    if (rbx_join_t < 4.0) {
        float bw = 308 * u, bh = 62 * u;
        roundrect(W * 0.5f - bw * 0.5f, 64 * u, bw, bh, 12 * u, 0xE61A1A1Eu);
        text_c("Первое лицо · полёт", W * 0.5f, 73 * u, 0.51f * u, 0xFFFFFFFFu);
        text_c("Лети туда, куда смотришь", W * 0.5f, 101 * u, 0.33f * u, 0xFFBDBDBDu);
    }
}
