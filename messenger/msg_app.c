/* Точки входа приложения: те же хуки, что раньше отдавались сгенерированному
 * из DimScript коду (init/update/draw/touch/reset), теперь реализованы на
 * чистом C. Управляет экранами и анимацией перехода список чатов ↔ разговор. */
#include "msg.h"
#include <math.h>
#include <string.h>

#define TRANS_T 0.22  /* секунды на слайд-переход */

typedef enum { MODE_CHATS, MODE_OPENING, MODE_CHAT, MODE_CLOSING } Mode;

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
    chats_reset();
    chat_reset();
    mode = MODE_CHATS;
    trans = 0;
    ds_log("Enjoer Messenger: чистый C, рендер и шрифт DimScript");
}

void reset(void) {
    msg_data_init();
    chats_reset();
    chat_reset();
    mode = MODE_CHATS;
    trans = 0;
}

void update(void) {
    msg_data_update();
    chats_update();
    chat_update();

    /* открыли чат из списка — запускаем слайд вперёд */
    if (mode == MODE_CHATS && chat_is_open()) {
        mode = MODE_OPENING;
        trans = 0;
    }
    if (mode == MODE_CHAT && chat_wants_close()) {
        mode = MODE_CLOSING;
        trans = 0;
    }
    if (mode == MODE_OPENING || mode == MODE_CLOSING) {
        trans += dt / TRANS_T;
        if (trans >= 1.0) {
            trans = 0;
            if (mode == MODE_OPENING) mode = MODE_CHAT;
            else {
                chat_finish_close();
                mode = MODE_CHATS;
            }
        }
    }
}

void draw(Buffer *buffer) {
    (void)buffer;
    float W = (float)screen_w;

    if (mode == MODE_CHATS) {
        chats_draw(0);
        return;
    }
    if (mode == MODE_OPENING) {
        float e = ease_out((float)trans);
        chats_draw(-W * 0.22f * e);
        chat_draw(W * (1.0f - e));
        return;
    }
    if (mode == MODE_CLOSING) {
        float e = ease_out((float)trans);
        chats_draw(-W * 0.22f * (1.0f - e));
        chat_draw(W * e);
        return;
    }
    chat_draw(0);
}

void touch(float x, float y, int action, int pointer_id) {
    if (pointer_id != 0) return;
    if (mode == MODE_CHATS) { chats_touch(x, y, action); return; }
    if (mode == MODE_CHAT) { chat_touch(x, y, action); return; }
    /* во время перехода касания игнорируем */
}
