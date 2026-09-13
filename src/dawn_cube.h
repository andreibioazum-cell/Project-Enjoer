/* C ABI for the C++ Dawn cube renderer. */
#ifndef ENJOER_DAWN_CUBE_H
#define ENJOER_DAWN_CUBE_H

#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* native_window is an ANativeWindow* on Android and NULL in the HTTP preview.
 * With ENJOER_USE_DAWN enabled it becomes the WebGPU surface target. */
int cube_renderer_init(void *native_window, int width, int height);
void cube_renderer_resize(int width, int height);
void cube_renderer_render(Buffer *preview_target, float rotation, float pitch);
void cube_renderer_shutdown(void);
const char *cube_renderer_backend(void);

#ifdef __cplusplus
}
#endif
#endif
