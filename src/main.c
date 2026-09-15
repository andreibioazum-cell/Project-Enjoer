/* Android native activity entry point. Vulkan owns the window surface; C only
 * forwards lifecycle and input events to the small cube game layer. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include <android_native_app_glue.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_activity.h>
#include <time.h>
#include "engine.h"
#include "ds_files.h"

static int active;
static int focused = 1;
static int initialized;
static uint64_t previous_ns;

typedef struct { float x, y; int action, id; } TouchCall;

static uint64_t monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return 0;
    return (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
}

static void protected_init(void *window) { game_init(window); }
static void protected_update(void *unused) { (void)unused; game_update(); }
static void protected_draw(void *unused) { (void)unused; game_draw(NULL); }
static void protected_touch(void *value) {
    TouchCall *touch = (TouchCall *)value;
    game_touch(touch->x, touch->y, touch->action, touch->id);
}

static void handle_command(struct android_app *app, int32_t command) {
    if (!app) return;
    switch (command) {
    case APP_CMD_INIT_WINDOW:
        if (!app->window) return;
        screen_w = ANativeWindow_getWidth(app->window);
        screen_h = ANativeWindow_getHeight(app->window);
        if (screen_w < 1 || screen_h < 1) return;
        game_shutdown();
        app_clear_error();
        if (!app_call(protected_init, app->window, "cube init")) return;
        initialized = 1;
        active = 1;
        previous_ns = 0;
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONTENT_RECT_CHANGED:
    case APP_CMD_CONFIG_CHANGED:
        if (app->window) {
            screen_w = ANativeWindow_getWidth(app->window);
            screen_h = ANativeWindow_getHeight(app->window);
            if (initialized) game_resize(screen_w, screen_h);
        }
        break;
    case APP_CMD_TERM_WINDOW:
        game_cancel_input();
        game_shutdown();
        initialized = 0;
        active = 0;
        break;
    case APP_CMD_GAINED_FOCUS:
        focused = 1;
        previous_ns = 0;
        break;
    case APP_CMD_LOST_FOCUS:
        focused = 0;
        game_cancel_input();
        previous_ns = 0;
        break;
    default:
        break;
    }
}

static int32_t handle_input(struct android_app *app, AInputEvent *event) {
    (void)app;
    if (!event || !active) return 0;
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_MOTION) {
        const size_t count = AMotionEvent_getPointerCount(event);
        const int raw = AMotionEvent_getAction(event);
        int action = raw & AMOTION_EVENT_ACTION_MASK;
        if (action == AMOTION_EVENT_ACTION_CANCEL) {
            game_cancel_input();
            return 1;
        }
        if (!count) return 0;
        size_t index = (size_t)((raw & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
        if (index >= count) index = 0;
        if (action == AMOTION_EVENT_ACTION_POINTER_DOWN) action = AMOTION_EVENT_ACTION_DOWN;
        else if (action == AMOTION_EVENT_ACTION_POINTER_UP) action = AMOTION_EVENT_ACTION_UP;
        if (action == AMOTION_EVENT_ACTION_MOVE) {
            for (size_t i = 0; i < count; ++i) {
                TouchCall call = {AMotionEvent_getX(event, i), AMotionEvent_getY(event, i),
                                  action, AMotionEvent_getPointerId(event, i)};
                if (!app_call(protected_touch, &call, "touch")) return 1;
            }
        } else if (action == AMOTION_EVENT_ACTION_DOWN || action == AMOTION_EVENT_ACTION_UP) {
            TouchCall call = {AMotionEvent_getX(event, index), AMotionEvent_getY(event, index),
                              action, AMotionEvent_getPointerId(event, index)};
            app_call(protected_touch, &call, "touch");
        }
        return 1;
    }
    if (AInputEvent_getType(event) == AINPUT_EVENT_TYPE_KEY) {
        const int32_t key_action = AKeyEvent_getAction(event);
        const int32_t key = AKeyEvent_getKeyCode(event);
        const char *mapped = NULL;
        switch (key) {
        case AKEYCODE_W: mapped = "w"; break;
        case AKEYCODE_A: mapped = "a"; break;
        case AKEYCODE_S: mapped = "s"; break;
        case AKEYCODE_D: mapped = "d"; break;
        case AKEYCODE_R: mapped = "r"; break;
        case AKEYCODE_SPACE: mapped = "space"; break;
        case AKEYCODE_DPAD_LEFT: mapped = "ArrowLeft"; break;
        case AKEYCODE_DPAD_RIGHT: mapped = "ArrowRight"; break;
        case AKEYCODE_DPAD_UP: mapped = "ArrowUp"; break;
        case AKEYCODE_DPAD_DOWN: mapped = "ArrowDown"; break;
        default: break;
        }
        if (mapped && (key_action == AKEY_EVENT_ACTION_DOWN ||
                       key_action == AKEY_EVENT_ACTION_UP))
            game_key(mapped, key_action == AKEY_EVENT_ACTION_DOWN);
        return 1;
    }
    return 0;
}

void android_main(struct android_app *app) {
    if (!app) return;
    app->onAppCmd = handle_command;
    app->onInputEvent = handle_input;
    app_set_activity((void *)app->activity);
    app_set_java_vm((void *)app->activity->vm);
    /* A game is data in the APK, not code: assets/game/game.manifest and the
     * .ds files next to it are the whole project, read through the asset
     * manager so the same folder layout works on a device and on disk. */
    ds_files_set_asset_manager(app->activity->assetManager);
    app_log("Enjoer: native C game + C++ Vulkan cube");

    for (;;) {
        struct android_poll_source *source = NULL;
        int ident;
        while ((ident = ALooper_pollOnce(active && focused ? 0 : 100, NULL, NULL,
                                         (void **)&source)) >= 0) {
            if (source && source->process) source->process(app, source);
            if (app->destroyRequested) {
                game_shutdown();
                return;
            }
        }
        if (!active || !initialized || !focused || app->destroyRequested) continue;

        const uint64_t now = monotonic_ns();
        dt = previous_ns ? (double)(now - previous_ns) / 1000000000.0 : 0.0;
        previous_ns = now;
        if (!app_call(protected_update, NULL, "cube update") ||
            !app_call(protected_draw, NULL, "cube draw")) {
            active = 0;
            app_log_error("%s", app_error());
        }
    }
}
