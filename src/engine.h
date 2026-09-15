/* Public C side of Enjoer.
 *
 * The game loop and input state deliberately stay in C. Rendering crosses one
 * small C ABI boundary into src/vulkan_cube.cpp, so the game can remain easy to
 * embed in the Android native activity while the GPU backend is C++. */
#ifndef ENJOER_ENGINE_H
#define ENJOER_ENGINE_H

#include <stdint.h>

#include "ds_manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t *pixels;
    int width;
    int height;
    int stride;
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
void app_set_asset_manager(void *manager);
void app_quit(void);

/* C game layer. The native window is opaque here; only Vulkan owns it.
 *
 * A game is either a folder of .ds files interpreted at runtime (the normal
 * case, see games/) or the ahead-of-time compiled example in src/generated.
 * game_set_game_dir selects the folder on the host; on Android the folder is
 * always the APK's assets/game. */
void game_set_game_dir(const char *dir);
const char *game_title(void);
int game_is_interpreted(void);
const DsGameManifest *game_manifest(void);
/* The last DimScript error, or "" when the script is healthy.  The preview
 * shows it instead of a black screen, which is the whole point of having a
 * host build at all. */
const char *game_script_error(void);
int game_script_failed(void);
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
