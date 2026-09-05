/* rbx/rbx_hud.c — 2D-интерфейс поверх 3D: верхняя панель, джойстик,
 * кнопка прыжка, имена над головами, плашка «зашли». */
#include "rbx_internal.h"
#include <stdio.h>

static void text_c(const char *s, float cx, float cy, float sc, uint32_t col) {
    int w = text_width(s);
    text_scaled(s, cx - (w * sc) * 0.5f, cy, col, sc);
}

void rbx_hud_draw(void) {
    float W = (float)screen_w;
    float u = W / 420.0f;
    if (u < 0.7f) u = 0.7f;

    /* верхняя панель как у Roblox */
    rect(0, 0, W, 50.0f * u, 0xE8232327u);
    roundrect(10 * u, 9 * u, 32 * u, 32 * u, 7 * u, 0xFFE2231Au);
    roundrect(17 * u, 16 * u, 18 * u, 18 * u, 4 * u, 0xFFFFFFFFu);
    text_scaled("Obby Park", 50 * u, 14 * u, 0xFFFFFFFFu, 0.62f * u);

    char buf[40];
    snprintf(buf, sizeof(buf), "монеты  %d/%d", rbx_world_caught(), rbx_world_coin_count());
    int tw = text_width(buf);
    text_scaled(buf, W - tw * 0.55f * u - 14 * u, 16 * u, 0xFFFFF59Du, 0.55f * u);

    /* джойстик */
    float joy_cx, joy_cy, joy_r, jx, jy;
    rbx_input_joy_geom(&joy_cx, &joy_cy, &joy_r);
    rbx_input_joy(&jx, &jy);
    ring(joy_cx, joy_cy, joy_r, 4.0f, 0x66FFFFFFu);
    circle(joy_cx, joy_cy, joy_r, 0x33000000u);
    circle(joy_cx + jx * (joy_r * 0.48f), joy_cy + jy * (joy_r * 0.48f), joy_r * 0.38f, 0xCCFFFFFFu);

    /* прыжок */
    float jmp_cx, jmp_cy, jmp_r;
    rbx_input_jump_geom(&jmp_cx, &jmp_cy, &jmp_r);
    circle(jmp_cx, jmp_cy, jmp_r, 0xCC43A047u);
    ring(jmp_cx, jmp_cy, jmp_r, 3.0f, 0xAAFFFFFFu);
    text_c("прыжок", jmp_cx, jmp_cy - 8 * u, 0.42f * u, 0xFFFFFFFFu);

    /* имена над головами */
    float px, py, pz, sx, sy;
    rbx_player_pos(&px, &py, &pz, NULL, NULL);
    if (rbx3d_project(px, py + 5.4f, pz, &sx, &sy))
        text_c("Ты", sx, sy - 10, 0.45f * u, 0xFFFFFFFFu);
    static const char *names[MAX_BOT] = { "Mila", "Rob", "Alex" };
    const RbxBot *bots = rbx_world_bots();
    for (int i = 0; i < MAX_BOT; i++) {
        if (rbx3d_project(bots[i].x, bots[i].y + 5.4f, bots[i].z, &sx, &sy))
            text_c(names[i], sx, sy - 8, 0.40f * u, 0xFFE0E0E0u);
    }

    /* короткая плашка «зашли» — мир уже 3D под ней */
    if (rbx_join_t < 1.6) {
        float a = rbx_join_t < 0.9 ? 1.0f : (float)((1.6 - rbx_join_t) / 0.7);
        if (a < 0) a = 0;
        uint32_t al = (uint32_t)(a * 200 + 20);
        float bw = 260 * u, bh = 54 * u;
        roundrect(W * 0.5f - bw * 0.5f, 64 * u, bw, bh, 12 * u, (al << 24) | 0x001A1A1Eu);
        text_c("Зашли в Obby Park", W * 0.5f, 78 * u, 0.55f * u, 0xFFFFFFFFu);
    }
}
