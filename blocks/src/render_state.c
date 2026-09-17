/* src/render_state.c — the active backend table and the platform window.
 *
 * Kept in its own translation unit so that test binaries which link only part
 * of the game (see tools/tests/run.sh) can still see a defined variable:
 * `render_backend` stays NULL there and the game code treats that as "no
 * backend table", which is exactly what a stubbed drawing API means. */
#include "render.h"

/* The software backend is the default whenever it is linked in. The reference
 * is weak on purpose: test suites that stub the drawing API link neither
 * backend, and there `render_backend` simply stays NULL. */
#if defined(__GNUC__) || defined(__clang__)
extern const RenderBackend sw_render_backend __attribute__((weak));
#else
extern const RenderBackend sw_render_backend;
#endif

const RenderBackend *render_backend = &sw_render_backend;
static void *platform_window;

void render_set_window(void *native_window) {
    platform_window = native_window;
    if (render_backend && render_backend->attach_window) render_backend->attach_window(native_window);
}

int render_windowed(void) { return render_backend ? render_backend->windowed : 0; }

const char *render_backend_name(void) { return render_backend ? render_backend->name : "none"; }

void *render_platform_window(void) { return platform_window; }
