/* C ABI for the C++ Vulkan cube renderer. */
#ifndef ENJOER_VULKAN_CUBE_H
#define ENJOER_VULKAN_CUBE_H

#include "engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* native_window is an ANativeWindow* on Android and NULL in the HTTP preview.
 * With ENJOER_USE_VULKAN enabled it becomes the VkSurfaceKHR target. */
int cube_renderer_init(void *native_window, int width, int height);
void cube_renderer_resize(int width, int height);
void cube_renderer_render(Buffer *preview_target, float rotation, float pitch);
void cube_renderer_shutdown(void);
const char *cube_renderer_backend(void);

/* 1 when the cube is drawn by the CPU rasterizer instead of Vulkan. */
int cube_renderer_software_active(void);

/* Draw one frame with the CPU rasterizer: the cube is rasterized at a reduced
 * resolution and stretched over the whole target. This is the path used on
 * devices without a working Vulkan driver, and it is host-testable. */
int cube_software_render(Buffer *target, float rotation, float pitch);

#ifdef __cplusplus
}
#endif
#endif
