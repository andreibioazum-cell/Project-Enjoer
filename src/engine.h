/* Public C side of Enjoer.
 *
 * The game loop and input state deliberately stay in C. Rendering crosses one
 * small C ABI boundary into src/vulkan_cube.cpp, so the game can remain easy to
 * embed in the Android native activity while the GPU backend is C++. */
#ifndef ENJOER_ENGINE_H
#define ENJOER_ENGINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* How the pixels of a Buffer are packed in memory. An ANativeWindow can hand
 * back a 16-bit RGB565 buffer on devices without a 32-bit window surface, so
 * the software renderer has to know the layout. */
#define ENJOER_BUFFER_FORMAT_RGBA8888 0
#define ENJOER_BUFFER_FORMAT_RGB565 1

typedef struct {
    uint32_t *pixels;
    int width;
    int height;
    int stride;
    int format; /* ENJOER_BUFFER_FORMAT_* */
} Buffer;

extern int screen_w;
extern int screen_h;
extern double dt;

typedef void (*AppCallback)(void *);
int app_call(AppCallback callback, void *arg, const char *label);
void app_fail(const char *format, ...);
const char *app_error(void);
int app_failed(void);
void app_clear_error(void);
void app_log(const char *format, ...);
void app_log_error(const char *format, ...);
void app_set_activity(void *activity);
void app_set_java_vm(void *vm);
void app_quit(void);

/* C game layer. The native window is opaque here; only Vulkan owns it. */
void game_init(void *native_window);
void game_resize(int width, int height);
void game_update(void);
void game_draw(Buffer *preview_target);
void game_touch(float x, float y, int action, int pointer_id);
void game_key(const char *name, int down);
void game_cancel_input(void);
void game_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
