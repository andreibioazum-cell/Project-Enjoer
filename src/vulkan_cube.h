/* C ABI for the C++ Vulkan cube renderer. */
#ifndef ENJOER_VULKAN_CUBE_H
#define ENJOER_VULKAN_CUBE_H

#include "engine.h"
#include "enjoer_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* native_window is an ANativeWindow* on Android and NULL in the HTTP preview.
 * With ENJOER_USE_VULKAN enabled it becomes the VkSurfaceKHR target.
 *
 * `frame` is the triangle batch the DimScript runtime filled during the draw
 * callback; `show_cube` keeps the 3D playground behind the game surface. */
int cube_renderer_init(void *native_window, int width, int height);
void cube_renderer_resize(int width, int height);
void cube_renderer_render(Buffer *preview_target, float rotation, float pitch,
                          const EnjoerFrame *frame, int show_cube);
void cube_renderer_shutdown(void);
const char *cube_renderer_backend(void);

#ifdef __cplusplus
}
#endif
#endif
