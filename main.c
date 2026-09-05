/* Enjoer — главный цикл под Android (native activity).
 * Хуки init/update/draw/touch/reset — 3D-плейс в rbx/.
 * Ошибки движка показываются вместо аварийного завершения. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <android_native_app_glue.h>
#include "engine.h"
#include "rbx/rbx.h"
#include <stdio.h>
#include <time.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_activity.h>

static int init_done = 0;
static int app_active = 0;
static int app_focused = 1;
static AAssetManager *app_assets = NULL;
static uint64_t prev_frame_ns = 0;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void protected_init(void *userdata) { game_init((AAssetManager *)userdata); }
static void protected_update(void *userdata) { (void)userdata; game_update(); }
static void protected_draw(void *userdata) { game_draw((Buffer *)userdata); }
typedef struct { float x; float y; int action; int id; } TouchCall;
static void protected_touch(void *userdata) {
    TouchCall *call = (TouchCall *)userdata;
    game_touch(call->x, call->y, call->action, call->id);
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
            if (!gfx_init(app_assets)) { init_done = 0; return; }
            /* Звуки лежат в тех же assets (sounds/...), вывод — AudioTrack. */
            audio_init(app_assets);
            audio_resume();
            init_done = 1;
            app_active = 0;
            app_clear_error();
            /* game_init() сам строит мир: не генерируем все чанки дважды. */
            if (!app_call(protected_init, app_assets, "init")) return;
            app_active = 1;
            break;
        case APP_CMD_WINDOW_RESIZED:
        case APP_CMD_CONTENT_RECT_CHANGED:
        case APP_CMD_CONFIG_CHANGED:
            rbx_cancel_input();
            /* Пересчитываем поверхность после изменения размеров. */
            if (app->window) {
                ANativeWindow_setBuffersGeometry(app->window, 0, 0, WINDOW_FORMAT_RGBA_8888);
                int w = ANativeWindow_getWidth(app->window);
                int h = ANativeWindow_getHeight(app->window);
                if (w > 0 && h > 0) { screen_w = w; screen_h = h; }
            }
            break;
        case APP_CMD_TERM_WINDOW:
            rbx_cancel_input();
            game_save();
            init_done = 0;
            app_active = 0;
            /* Потоки растеризации гасятся до выгрузки окна. */
            rbx3d_shutdown();
            gfx_shutdown();
            audio_shutdown();
            break;
        case APP_CMD_GAINED_FOCUS: app_focused=1;prev_frame_ns=0;audio_resume();break;
        case APP_CMD_LOST_FOCUS:
            app_focused=0;
            rbx_cancel_input();
            game_save();
            prev_frame_ns = 0;
            audio_pause();
            break;
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
        raw = AMotionEvent_getAction(event);
        action = raw & AMOTION_EVENT_ACTION_MASK;
        if (action == AMOTION_EVENT_ACTION_CANCEL) { rbx_cancel_input(); return 1; }
        if (count == 0) return 0;
        if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) action = AMOTION_EVENT_ACTION_DOWN;
        else if (action == AMOTION_EVENT_ACTION_POINTER_UP) action = AMOTION_EVENT_ACTION_UP;
        if (action != AMOTION_EVENT_ACTION_DOWN && action != AMOTION_EVENT_ACTION_UP &&
            action != AMOTION_EVENT_ACTION_MOVE) return 0;
        index = (size_t)((raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (index >= count) index = 0;
        i = (action == AMOTION_EVENT_ACTION_MOVE) ? 0 : index;
        count = (action == AMOTION_EVENT_ACTION_MOVE) ? count : index + 1;
        for (; i < count; i++) {
            call.x = AMotionEvent_getX(event, i);
            call.y = AMotionEvent_getY(event, i);
            call.action = action;
            call.id = AMotionEvent_getPointerId(event, i);
            if (!app_call(protected_touch, &call, "touch")) break;
        }
        return 1;
    } else if (type == AINPUT_EVENT_TYPE_KEY) {
        int32_t action = AKeyEvent_getAction(event);
        int32_t key = AKeyEvent_getKeyCode(event);
        if (key == AKEYCODE_BACK) return 0;
        const char *game_key = NULL;
        switch (key) {
            case AKEYCODE_W: game_key = "w"; break;
            case AKEYCODE_A: game_key = "a"; break;
            case AKEYCODE_S: game_key = "s"; break;
            case AKEYCODE_D: game_key = "d"; break;
            case AKEYCODE_E: game_key = "break"; break;
            case AKEYCODE_R: game_key = "place"; break;
            case AKEYCODE_1: game_key = "1"; break;
            case AKEYCODE_2: game_key = "2"; break;
            case AKEYCODE_3: game_key = "3"; break;
            case AKEYCODE_4: game_key = "4"; break;
            case AKEYCODE_5: game_key = "5"; break;
            case AKEYCODE_6: game_key = "6"; break;
            case AKEYCODE_7: game_key = "7"; break;
            case AKEYCODE_F: game_key = "f"; break;
            case AKEYCODE_DPAD_LEFT: game_key = "ArrowLeft"; break;
            case AKEYCODE_DPAD_RIGHT: game_key = "ArrowRight"; break;
            case AKEYCODE_DPAD_UP: game_key = "ArrowUp"; break;
            case AKEYCODE_DPAD_DOWN: game_key = "ArrowDown"; break;
            case AKEYCODE_SPACE: game_key = "space"; break;
            case AKEYCODE_SHIFT_LEFT:
            case AKEYCODE_SHIFT_RIGHT: game_key = "Shift"; break;
            default: break;
        }
        if (game_key && app_active && (action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_UP))
            rbx_key(game_key, action == AKEY_EVENT_ACTION_DOWN);
        return 1;
    }
    return 0;
}

void android_main(struct android_app *app) {
    Buffer frame = {0};
    if (!app) return;
    app->onAppCmd = handle_cmd;
    app->onInputEvent = handle_input;
    audio_set_java_vm((void *)app->activity->vm);
    app_set_storage(app->activity->internalDataPath);
    app_log("Enjoer: Android, 3D-плейс на чистом C");
    for (;;) {
        struct android_poll_source *source = NULL;
        int ident;
        while ((ident = ALooper_pollOnce(app_active && app_focused ? 0 : 100, NULL, NULL, (void **)&source)) >= 0) {
            if (source && source->process) source->process(app, source);
            if (app->destroyRequested) {
                game_save();
                init_done = 0;
                app_active = 0;
                gfx_shutdown();
                audio_shutdown();
                return;
            }
        }
        if (!app->window || !init_done || app->destroyRequested || !app_focused) continue;

        uint64_t now = monotonic_ns();
        if (app_active) {
            dt = prev_frame_ns ? (double)(now - prev_frame_ns) / 1000000000.0 : 0.0;
            if (dt < 0.0) dt = 0.0;
            if (!app_call(protected_update, NULL, "update")) app_active = 0;
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
            if (frame_valid && gfx_begin_frame(&frame)) {
                int draw_failed = 0;
                if (app_active) {
                    if (!app_call(protected_draw, &frame, "draw")) {
                        draw_failed = 1;
                        app_active = 0;
                    }
                }
                if (!app_active) {
                    if (draw_failed || app_failed())
                        gfx_error_screen(app_error());
                    gfx_cancel_frame();
                } else {
                    gfx_end_frame();
                }
            }
            ANativeWindow_unlockAndPost(app->window);
        }
    }
}

