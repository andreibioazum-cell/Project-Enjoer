/* Точки входа приложения: хуки init/update/draw/touch/reset на чистом C.
 * Управляет переходами: список чатов ↔ чат / уровни / скины.
 * Скины с текстурами зомби Азума видны ТОЛЬКО внутри экрана скинов. */
#include "msg.h"
#include <math.h>
#include <string.h>

#define TRANS_T 0.22  /* секунды на слайд-переход */

typedef enum {
    MODE_CHATS,
    MODE_OPENING_CHAT, MODE_CHAT, MODE_CLOSING_CHAT,
    MODE_OPENING_LEVELS, MODE_LEVELS, MODE_CLOSING_LEVELS,
    MODE_OPENING_SKINS,  MODE_SKINS,  MODE_CLOSING_SKINS
} Mode;

static Mode mode = MODE_CHATS;
static double trans = 0; /* 0..1 */

static float ease_out(float t) {
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
}

void init(AAssetManager *assets) {
    (void)assets;
    msg_data_init();
    snd_load("send.wav");
    snd_load("notify.wav");
    png_load("legacy_zombie_azum.png");
    png_load("legacy_zombie_azum_punch.png");
    chats_reset();
    chat_reset();
    levels_reset();
    skins_reset();
    mode = MODE_CHATS;
    trans = 0;
    ds_log("Enjoer Messenger: чистый C, рендер и шрифт DimScript — убраны 2 класса, добавлены Уровни/Скины");
}

void reset(void) {
    msg_data_init();
    chats_reset();
    chat_reset();
    levels_reset();
    skins_reset();
    mode = MODE_CHATS;
    trans = 0;
}

void update(void) {
    msg_data_update();
    chats_update();
    chat_update();
    levels_update();
    skins_update();

    /* открытия из списка чатов */
    if (mode == MODE_CHATS) {
        if (chat_is_open()) { mode = MODE_OPENING_CHAT; trans = 0; }
        else if (levels_is_open()) { mode = MODE_OPENING_LEVELS; trans = 0; }
        else if (skins_is_open()) { mode = MODE_OPENING_SKINS; trans = 0; }
    }
    /* закрытия */
    if (mode == MODE_CHAT && chat_wants_close()) { mode = MODE_CLOSING_CHAT; trans = 0; }
    if (mode == MODE_LEVELS && levels_wants_close()) { mode = MODE_CLOSING_LEVELS; trans = 0; }
    if (mode == MODE_SKINS && skins_wants_close()) { mode = MODE_CLOSING_SKINS; trans = 0; }

    if (mode == MODE_OPENING_CHAT || mode == MODE_CLOSING_CHAT ||
        mode == MODE_OPENING_LEVELS || mode == MODE_CLOSING_LEVELS ||
        mode == MODE_OPENING_SKINS || mode == MODE_CLOSING_SKINS) {
        trans += dt / TRANS_T;
        if (trans >= 1.0) {
            trans = 0;
            if (mode == MODE_OPENING_CHAT) mode = MODE_CHAT;
            else if (mode == MODE_OPENING_LEVELS) mode = MODE_LEVELS;
            else if (mode == MODE_OPENING_SKINS) mode = MODE_SKINS;
            else if (mode == MODE_CLOSING_CHAT) { chat_finish_close(); mode = MODE_CHATS; }
            else if (mode == MODE_CLOSING_LEVELS) { levels_finish_close(); mode = MODE_CHATS; }
            else if (mode == MODE_CLOSING_SKINS) { skins_finish_close(); mode = MODE_CHATS; }
        }
    }
}

void draw(Buffer *buffer) {
    (void)buffer;
    float W = (float)screen_w;

    if (mode == MODE_CHATS) { chats_draw(0); return; }

    if (mode == MODE_OPENING_CHAT) {
        float e = ease_out((float)trans);
        chats_draw(-W * 0.22f * e);
        chat_draw(W * (1.0f - e));
        return;
    }
    if (mode == MODE_CLOSING_CHAT) {
        float e = ease_out((float)trans);
        chats_draw(-W * 0.22f * (1.0f - e));
        chat_draw(W * e);
        return;
    }
    if (mode == MODE_OPENING_LEVELS) {
        float e = ease_out((float)trans);
        chats_draw(-W * 0.22f * e);
        levels_draw(W * (1.0f - e));
        return;
    }
    if (mode == MODE_CLOSING_LEVELS) {
        float e = ease_out((float)trans);
        chats_draw(-W * 0.22f * (1.0f - e));
        levels_draw(W * e);
        return;
    }
    if (mode == MODE_OPENING_SKINS) {
        float e = ease_out((float)trans);
        chats_draw(-W * 0.22f * e);
        skins_draw(W * (1.0f - e));
        return;
    }
    if (mode == MODE_CLOSING_SKINS) {
        float e = ease_out((float)trans);
        chats_draw(-W * 0.22f * (1.0f - e));
        skins_draw(W * e);
        return;
    }
    if (mode == MODE_CHAT) { chat_draw(0); return; }
    if (mode == MODE_LEVELS) { levels_draw(0); return; }
    if (mode == MODE_SKINS) { skins_draw(0); return; }
    chats_draw(0);
}

void touch(float x, float y, int action, int pointer_id) {
    if (pointer_id != 0) return;
    if (mode == MODE_CHATS) { chats_touch(x, y, action); return; }
    if (mode == MODE_CHAT) { chat_touch(x, y, action); return; }
    if (mode == MODE_LEVELS) { levels_touch(x, y, action); return; }
    if (mode == MODE_SKINS) { skins_touch(x, y, action); return; }
    /* во время перехода касания игнорируем */
}
