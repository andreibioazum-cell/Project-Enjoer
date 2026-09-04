/* Enjoer Messenger — главный цикл под Android (native activity).
 * Раньше здесь жил мост к сгенерированному из DimScript коду игры; теперь
 * хуки init/update/draw/touch/reset реализованы на чистом C в messenger/.
 * Механизм защищённых вызовов оставлен: если рантайм словит ошибку, вместо
 * падения выводится экран с текстом ошибки и консолью. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <android_native_app_glue.h>
#include "runtime.h"
#include "messenger/msg.h"
#include <stdio.h>
#include <time.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_activity.h>

static int init_done = 0;
static int app_active = 0;
static AAssetManager *app_assets = NULL;
static uint64_t prev_frame_ns = 0;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void protected_init(void *userdata) { init((AAssetManager *)userdata); }
static void protected_reset(void *userdata) { (void)userdata; reset(); }
static void protected_update(void *userdata) { (void)userdata; update(); }
static void protected_draw(void *userdata) { draw((Buffer *)userdata); }
typedef struct { float x; float y; int action; int id; } TouchCall;
static void protected_touch(void *userdata) {
    TouchCall *call = (TouchCall *)userdata;
    touch(call->x, call->y, call->action, call->id);
}

static void handle_cmd(struct android_app *app, int32_t command) {
    if (!app) return;
    switch (command) {
        case APP_CMD_INIT_WINDOW:
            if (!app->window) { init_done = 0; return; }
            screen_w = ANativeWindow_getWidth(app->window);
            screen_h = ANativeWindow_getHeight(app->window);
            if (screen_w <= 0 || screen_h <= 0) { init_done = 0; return; }
            app_assets = app->activity ? app->activity->assetManager : NULL;
            ANativeWindow_setBuffersGeometry(app->window, 0, 0, WINDOW_FORMAT_RGBA_8888);
            ds_set_activity(app->activity);
            if (!ds_graphics_init(app_assets)) { init_done = 0; return; }
            /* Звуки лежат в тех же assets (sounds/...), вывод — AudioTrack. */
            ds_sound_init(app_assets);
            ds_sound_resume();
            init_done = 1;
            app_active = 0;
            ds_clear_runtime_error();
            ds_string_pool_reset();
            if (!ds_call_protected(protected_reset, NULL, "reset")) { init_done = 0; return; }
            ds_clear_runtime_error();
            if (!ds_call_protected(protected_init, app_assets, "init")) { init_done = 0; return; }
            app_active = 1;
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED:
        case APP_CMD_CONFIG_CHANGED:
            /* adjustResize меняет поверхность, пока открыта клавиатура. */
            if (app->window) {
                ANativeWindow_setBuffersGeometry(app->window, 0, 0, WINDOW_FORMAT_RGBA_8888);
                int w = ANativeWindow_getWidth(app->window);
                int h = ANativeWindow_getHeight(app->window);
                if (w > 0 && h > 0) { screen_w = w; screen_h = h; }
            }
            break;
        case APP_CMD_TERM_WINDOW:
            init_done = 0;
            app_active = 0;
            keyboard_hide();
            ds_graphics_shutdown();
            ds_sound_shutdown();
            break;
        case APP_CMD_GAINED_FOCUS: ds_sound_resume(); break;
        case APP_CMD_LOST_FOCUS: ds_sound_pause(); break;
        default: break;
    }
}

static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    (void)app;
    if (!event) return 0;
    int32_t type = AInputEvent_getType(event);
    if (type == AINPUT_EVENT_TYPE_MOTION) {
        if (!app_active) return 0;
        TouchCall call;
        size_t count, index, i;
        int raw, action;
        count = AMotionEvent_getPointerCount(event);
        if (count == 0) return 0;
        raw = AMotionEvent_getAction(event);
        action = raw & AMOTION_EVENT_ACTION_MASK;
        if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) action = AMOTION_EVENT_ACTION_DOWN;
        else if (action == AMOTION_EVENT_ACTION_POINTER_UP) action = AMOTION_EVENT_ACTION_UP;
        index = (size_t)((raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (index >= count) index = 0;
        i = (action == AMOTION_EVENT_ACTION_MOVE) ? 0 : index;
        count = (action == AMOTION_EVENT_ACTION_MOVE) ? count : index + 1;
        for (; i < count; i++) {
            call.x = AMotionEvent_getX(event, i);
            call.y = AMotionEvent_getY(event, i);
            call.action = action;
            call.id = AMotionEvent_getPointerId(event, i);
            if (!ds_call_protected(protected_touch, &call, "touch")) break;
        }
        return 1;
    } else if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t action = AKeyEvent_getAction(event);
        int32_t key = AKeyEvent_getKeyCode(event);
        int32_t meta = AKeyEvent_getMetaState(event);
        if (key == AKEYCODE_BACK && action == AKEY_EVENT_ACTION_DOWN &&
            (keyboard_visible() || keyboard_uses_editor())) {
            keyboard_hide();
            return 1;
        }
        if (keyboard_visible() &&
            (action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_MULTIPLE)) {
            if (keyboard_handle_key(key, action, meta)) return 1;
        }
        if (key == AKEYCODE_BACK && action == AKEY_EVENT_ACTION_UP) return 0;
        /* Когда текст ведёт системный EditText, клавиши должны дойти до него. */
        if (keyboard_uses_editor()) return 0;
        return 1;
    }
    return 0;
}

void android_main(struct android_app *app) {
    Buffer frame = {0};
    if (!app) return;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;
    ds_sound_set_java_vm((void *)app->activity->vm);
    /* Данные мессенджера (чаты/сообщения) пишем во внутреннее хранилище. */
    msg_set_data_path(app->activity->internalDataPath);
    ds_set_activity(app->activity);
    ds_log("Enjoer Messenger: Android, чистый C (рендер и шрифт из DimScript)");
    for (;;) {
        struct android_poll_source *source = NULL;
        int ident;
        while ((ident = ALooper_pollOnce(app_active ? 0 : 10, NULL, NULL, (void **)&source)) >= 0) {
            if (source && source->process) source->process(app, source);
            if (app->destroyRequested) {
                init_done = 0;
                app_active = 0;
                keyboard_hide();
                ds_graphics_shutdown();
                ds_sound_shutdown();
                return;
            }
        }
        if (!app->window || !init_done || app->destroyRequested) continue;

        uint64_t now = monotonic_ns();
        if (app_active) {
            dt = prev_frame_ns ? (double)(now - prev_frame_ns) / 1000000000.0 : 0.0;
            if (dt < 0.0) dt = 0.0;
            if (dt > 0.1) dt = 0.1;
            if (!ds_call_protected(protected_update, NULL, "update")) app_active = 0;
        }
        prev_frame_ns = now;

        ANativeWindow_Buffer native_buffer;
        if (ANativeWindow_lock(app->window, &native_buffer, NULL) == 0) {
            int frame_valid;
            frame.pixels = (uint32_t *)native_buffer.bits;
            frame.width = native_buffer.width;
            frame.height = native_buffer.height;
            frame.stride = native_buffer.stride;
            frame_valid = frame.pixels && frame.width > 0 && frame.height > 0 &&
                          frame.stride >= frame.width &&
                          native_buffer.format == WINDOW_FORMAT_RGBA_8888;
            if (frame_valid && ds_graphics_begin_frame(&frame)) {
                int draw_failed = 0;
                if (app_active) {
                    if (!ds_call_protected(protected_draw, &frame, "draw")) {
                        draw_failed = 1;
                        app_active = 0;
                    }
                }
                if (!app_active) {
                    if (draw_failed || ds_script_has_error())
                        ds_graphics_error_screen(ds_runtime_error_message());
                    ds_graphics_cancel_frame();
                } else {
                    ds_graphics_end_frame();
                }
            }
            ANativeWindow_unlockAndPost(app->window);
        }
    }
}

#include "graphics.c"
#include "sound.c"
#include "messenger/msg_ui.c"
#include "messenger/msg_data.c"
#include "messenger/msg_chats.c"
#include "messenger/msg_chat.c"
#include "messenger/msg_levels.c"
#include "messenger/msg_skins.c"
#include "messenger/msg_app.c"
