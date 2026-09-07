/* Platformium HUD: score, coins, lives, timer, level toast and the
 * clear / game-over panels. */
#include "platformium_internal.h"
#include <stdio.h>

void pm_text_center(const char *s, float x, float y, uint32_t color, float scale) {
    text_scaled(s, x - text_width(s) * scale * .5f, y, color, scale);
}

static void text_shadow(const char *s, float x, float y, uint32_t color, float scale) {
    text_scaled(s, x + scale * 4, y + scale * 4, 0x88000000u, scale);
    text_scaled(s, x, y, color, scale);
}

static void text_shadow_center(const char *s, float x, float y, uint32_t color, float scale) {
    pm_text_center(s, x + scale * 4, y + scale * 4, 0x88000000u, scale);
    pm_text_center(s, x, y, color, scale);
}

static void coin_icon(float x, float y, float r) {
    circle(x, y, r, 0xffe9b23au);
    circle(x, y, r * .7f, 0xffffd25eu);
}

static void heart(float x, float y, float r, int full) {
    uint32_t c = full ? 0xffff5f7au : 0x55ffffffu;
    circle(x - r * .45f, y - r * .2f, r * .55f, c);
    circle(x + r * .45f, y - r * .2f, r * .55f, c);
    float pts = r * 1.05f;
    rect(x - r * .92f, y, r * 1.84f, pts * .5f, c);
    rect(x - r * .6f, y + pts * .45f, r * 1.2f, pts * .35f, c);
    rect(x - r * .3f, y + pts * .75f, r * .6f, pts * .3f, c);
}

void pm_hud_draw(void) {
    PmLevelDef *level = pm_level_live();
    if (!level) return;
    float u = fminf(screen_w / 960.0f, screen_h / 540.0f);
    if (u <= 0) return;
    char label[64];

    /* level name under the router's menu button */
    snprintf(label, sizeof(label), "%s", level->name);
    text_shadow(label, 56 * u, 14 * u, 0xffffffffu, .42f * u);

    /* coins, centered */
    snprintf(label, sizeof(label), "%d/%d", pm_coins, level->coins_total);
    float cw = text_width(label) * .48f * u;
    coin_icon(screen_w * .5f - cw * .5f - 16 * u, 26 * u, 10 * u);
    text_shadow(label, screen_w * .5f - cw * .5f, 14 * u, 0xffffd25eu, .48f * u);

    /* score on the right */
    snprintf(label, sizeof(label), "Score %d", pm_score);
    text_shadow(label, screen_w - 18 * u - text_width(label) * .46f * u, 14 * u, 0xffffffffu, .46f * u);

    /* lives + timer under the score */
    for (int i = 0; i < 3; i++)
        heart(screen_w - (110 - i * 30) * u, 48 * u, 9 * u, i < pm_lives);
    int t = pm_timer > 0 ? (int)pm_timer : 0;
    snprintf(label, sizeof(label), "%d:%02d", t / 60, t % 60);
    uint32_t tc = t <= 30 && ((int)(pm_time * 2) & 1) ? 0xffff6b6bu : 0xffffffffu;
    text_shadow(label, screen_w - 18 * u - text_width(label) * .40f * u, 60 * u, tc, .40f * u);

    pm_input_draw();

    /* level toast at the start of a run */
    if (pm_state_time < 2.4f && pm_state == PM_STATE_PLAY) {
        float fade = pm_state_time < .4f ? pm_state_time / .4f
                   : pm_state_time > 1.9f ? (2.4f - pm_state_time) / .5f : 1;
        if (fade < 0) fade = 0;
        if (fade > 1) fade = 1;
        char toast[80];
        snprintf(toast, sizeof(toast), "%s", level->name);
        uint32_t c = 0xff000000u | (uint32_t)(200 * fade) << 24;
        pm_text_center(toast, screen_w * .5f + 2 * u, screen_h * .30f + 2 * u, 0x00000000u | (uint32_t)(110 * fade) << 24, .9f * u);
        pm_text_center(toast, screen_w * .5f, screen_h * .30f, c, .9f * u);
    }
}

static void panel(float *x, float *y, float *w, float *h) {
    float u = fminf(screen_w / 960.0f, screen_h / 540.0f);
    *w = fminf(560 * u, screen_w * .86f);
    *h = 250 * u;
    *x = screen_w * .5f - *w * .5f;
    *y = screen_h * .5f - *h * .5f;
}

void pm_hud_overlay_draw(void) {
    if (pm_state != PM_STATE_CLEAR && pm_state != PM_STATE_GAMEOVER) return;
    PmLevelDef *level = pm_level_live();
    float u = fminf(screen_w / 960.0f, screen_h / 540.0f);
    if (u <= 0 || !level) return;
    float x, y, w, h;
    panel(&x, &y, &w, &h);
    rect(0, 0, (float)screen_w, (float)screen_h, 0x90000000u);
    roundrect(x, y, w, h, 14 * u, 0xf2232a35u);
    roundrect(x + 5 * u, y + 5 * u, w - 10 * u, 8 * u, 4 * u,
              pm_state == PM_STATE_CLEAR ? 0xff2ee6a8u : 0xffff5f7au);
    char line[96];
    if (pm_state == PM_STATE_CLEAR) {
        text_shadow_center("Level clear!", x + w * .5f, y + 34 * u, 0xff2ee6a8u, .8f * u);
        snprintf(line, sizeof(line), "Coins  %d / %d", pm_coins, level->coins_total);
        text_shadow_center(line, x + w * .5f, y + 96 * u, 0xffffd25eu, .52f * u);
        snprintf(line, sizeof(line), "Time bonus  +%d", (int)(pm_timer > 0 ? pm_timer : 0) * 5);
        text_shadow_center(line, x + w * .5f, y + 128 * u, 0xffffffffu, .52f * u);
        snprintf(line, sizeof(line), "Score  %d", pm_score);
        text_shadow_center(line, x + w * .5f, y + 160 * u, 0xffffffffu, .52f * u);
        int last = pm_level_def_count() - 1;
        snprintf(line, sizeof(line), pm_current_level() >= last ?
                 "Tap for the menu" : "Tap for the next level");
        if ((int)(pm_time * 1.6f) & 1)
            text_shadow_center(line, x + w * .5f, y + h - 44 * u, 0xaad0d8e0u, .46f * u);
    } else {
        text_shadow_center("Game over", x + w * .5f, y + 40 * u, 0xffff5f7au, .85f * u);
        snprintf(line, sizeof(line), "Score  %d", pm_score);
        text_shadow_center(line, x + w * .5f, y + 110 * u, 0xffffffffu, .55f * u);
        if ((int)(pm_time * 1.6f) & 1)
            text_shadow_center("Tap to try again", x + w * .5f, y + h - 44 * u, 0xaad0d8e0u, .46f * u);
    }
}
