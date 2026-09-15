/* C ABI for the C++ 2D renderer.  Enjoer is a 2D engine: scripts draw in screen
 * pixels, and the renderer presents exactly the triangle batch the game built
 * — no 3D scene, no depth, no camera. */
#ifndef ENJOER_RENDERER_H
#define ENJOER_RENDERER_H

#include "engine.h"
#include "enjoer_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* native_window is an ANativeWindow* on Android and NULL in the HTTP preview.
 * With ENJOER_USE_VULKAN enabled it becomes the VkSurfaceKHR target.
 *
 * `frame` is the triangle batch the DimScript runtime filled during the draw
 * callback. */
int renderer_init(void *native_window, int width, int height);
void renderer_resize(int width, int height);
void renderer_render(Buffer *preview_target, const EnjoerFrame *frame);
void renderer_shutdown(void);
const char *renderer_backend(void);

#ifdef __cplusplus
}
#endif
#endif
