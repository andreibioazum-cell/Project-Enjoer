/* How screen pixels reach a pre-rotated Android surface.
 *
 * A phone panel has one fixed orientation (portrait, for almost every device)
 * and the compositor rotates the app's buffer into the orientation the app is
 * running in.  Vulkan exposes that as VkSurfaceCapabilitiesKHR::currentTransform
 * and, per the swapchain contract, an app that sets `preTransform` to it is
 * saying "I already drew my content for the panel, do not rotate it for me".
 *
 * So a landscape game on a portrait panel either
 *
 *   - asks for the panel-sized framebuffer and draws sideways into it
 *     (this file, and what the Android pre-rotation guidance recommends), or
 *   - renders upright into a display-sized framebuffer and pays for a
 *     compositor rotation every frame.
 *
 * It must not do what this engine used to do: take the panel-sized
 * framebuffer, draw upright into it and let the compositor rotate as well.
 * The result is content laid out for 2400x1080 written into 1080x2400 —
 * rotated, squashed and cut off, which reads as "the text is vertical and
 * nothing can be read".
 *
 * The window (ANativeWindow) size is in *display* orientation — that is the
 * pixel space a game draws in and the space touches arrive in.  The
 * framebuffer below is in the panel's, which is why 90 and 270 transpose it.
 *
 * The mapping is Android's own pre-rotation table (the same arithmetic the
 * viewport/scissor remap in the platform guidance uses), expressed once as a
 * per-point affine transform so the orthographic matrix and the swapchain
 * extent can never drift apart.  A driver that will not accept the transform it
 * reports gets identity: no pre-rotation, no matrix turn, the compositor's job —
 * which is the arrangement that has always worked, and the one to fall back to
 * rather than to a swapchain the loader refuses to create.  It is a header of pure arithmetic on purpose:
 * `src/vulkan_2d.cpp` needs it behind ENJOER_USE_VULKAN, and a host test can
 * check it without a GPU.
 */
#ifndef ENJOER_SURFACE_TRANSFORM_H
#define ENJOER_SURFACE_TRANSFORM_H

#ifdef __cplusplus
extern "C" {
#endif

/* The rotations a 2D game can compensate for with a matrix alone — exactly the
 * four values a surface can hand one, and nothing else: a mirror or an INHERIT
 * has no transform here to express it, so those are refused rather than guessed.
 * Degrees, deliberately, because that is what the mapping below reads; the
 * indices Vulkan uses for the same four are its own business (see
 * enjoer_surface_rotation_from_vk). */
typedef enum {
    ENJOER_SURFACE_ROTATE_0 = 0,
    ENJOER_SURFACE_ROTATE_90 = 90,   /* window is landscape, panel is portrait */
    ENJOER_SURFACE_ROTATE_180 = 180, /* upside-down, same orientation */
    ENJOER_SURFACE_ROTATE_270 = 270  /* window is landscape, the other way round */
} EnjoerSurfaceRotation;

/* Vulkan does not number those transforms by degrees: VkSurfaceTransformFlagBits
 * calls them 0, 1, 2, 3 (IDENTITY, ROTATE_90, ROTATE_180, ROTATE_270), keeps 4
 * for INHERIT_BIT, combines mirrors in the higher bits, and
 * VkSurfaceCapabilitiesKHR::supportedTransforms is that vocabulary again — as
 * flag bits.  Degrees are what the mapping and the matrix below want, so the
 * two are translated here, once: reading a Vulkan value as a degree count is how
 * a rotation gets dropped without anyone noticing. */
static inline int enjoer_surface_rotation_from_vk(int transform) {
    switch (transform) {
    case 0: return ENJOER_SURFACE_ROTATE_0;
    case 1: return ENJOER_SURFACE_ROTATE_90;
    case 2: return ENJOER_SURFACE_ROTATE_180;
    case 3: return ENJOER_SURFACE_ROTATE_270;
    default: return -1; /* INHERIT, a mirror, or a driver's own idea */
    }
}

static inline int enjoer_surface_rotation_to_vk(int rotation) {
    switch (rotation) {
    case ENJOER_SURFACE_ROTATE_90: return 1;
    case ENJOER_SURFACE_ROTATE_180: return 2;
    case ENJOER_SURFACE_ROTATE_270: return 3;
    default: return 0;
    }
}

/* The bit a rotation occupies in supportedTransforms. */
static inline int enjoer_surface_rotation_bit(int rotation) {
    return 1 << enjoer_surface_rotation_to_vk(rotation);
}

/* The rotation to pre-rotate the picture for, from the two fields of
 * VkSurfaceCapabilitiesKHR.  Pre-rotating is on the table only when the surface
 * asked for it, when a matrix can express it, and when the driver accepts that
 * very transform for a swapchain: a swapchain created with any other bit is
 * invalid, and invalid on a phone is a black screen.  Falling back to 0 leaves
 * the rotation to the compositor, which costs a blit per frame and is still
 * upright — the better way to be wrong. */
static inline int enjoer_surface_rotation_for(int current_transform, int supported_transforms) {
    const int degrees = enjoer_surface_rotation_from_vk(current_transform);
    if (degrees < 0) return ENJOER_SURFACE_ROTATE_0;
    if ((supported_transforms & enjoer_surface_rotation_bit(degrees)) == 0)
        return ENJOER_SURFACE_ROTATE_0;
    return degrees;
}

/* Size of the framebuffer that holds a `width` x `height` window (display
 * orientation) once the rotation is baked into the content. */
static inline void enjoer_surface_extent(int rotation, int width, int height, int *out_width,
                                         int *out_height) {
    const int w = width > 0 ? width : 1;
    const int h = height > 0 ? height : 1;
    if (rotation == ENJOER_SURFACE_ROTATE_90 || rotation == ENJOER_SURFACE_ROTATE_270) {
        *out_width = h;
        *out_height = w;
    } else {
        *out_width = w;
        *out_height = h;
    }
}

/* Size the swapchain images have to be for a `window_width` x `window_height`
 * window, already clamped into the [min,max] extent range a driver accepts.
 * The sentinel Vulkan uses for "no fixed size" (0xFFFFFFFF) and a driver that
 * reports a zero max both fall back to the window's own numbers, so this never
 * returns a degenerate extent and never needs a Vulkan header to be checked. */
static inline void enjoer_surface_framebuffer(int rotation, int window_width, int window_height,
                                              int min_width, int min_height, int max_width,
                                              int max_height, int *out_width, int *out_height) {
    int width = 1, height = 1;
    const int sentinel = (int)0xFFFFFFFF;
    enjoer_surface_extent(rotation, window_width, window_height, &width, &height);
    if (max_width > 0 && max_width != sentinel) {
        if (width > max_width) width = max_width;
        if (width < 1) width = 1;
    }
    if (max_height > 0 && max_height != sentinel) {
        if (height > max_height) height = max_height;
        if (height < 1) height = 1;
    }
    if (min_width > 0 && min_width != sentinel && width < min_width) width = min_width;
    if (min_height > 0 && min_height != sentinel && height < min_height) height = min_height;
    *out_width = width;
    *out_height = height;
}

/* Where one screen pixel lands in the framebuffer: (x, y) in window pixels,
 * origin top left and y down, to (u, v) in framebuffer pixels. */
static inline void enjoer_surface_map(int rotation, float buffer_width, float buffer_height,
                                      float x, float y, float *out_u, float *out_v) {
    switch (rotation) {
    case ENJOER_SURFACE_ROTATE_90:
        *out_u = buffer_width - y;
        *out_v = x;
        break;
    case ENJOER_SURFACE_ROTATE_180:
        *out_u = buffer_width - x;
        *out_v = buffer_height - y;
        break;
    case ENJOER_SURFACE_ROTATE_270:
        *out_u = y;
        *out_v = buffer_height - x;
        break;
    default:
        *out_u = x;
        *out_v = y;
        break;
    }
}

/* Column-major mat4 (GLSL layout) that turns the screen pixels a script draws
 * in into clip space for this framebuffer: a plain 2D ortho with the surface
 * rotation folded in front of it.  `m` receives 16 floats.
 *
 * The one rule this matrix must keep: window pixel (x, y) lands exactly on the
 * framebuffer pixel enjoer_surface_map() assigns it.  With the plain
 * full-screen viewport the renderer records (positive height), Vulkan's NDC
 * runs (-1, -1) at the framebuffer's top-left and (+1, +1) at its bottom-right
 * — the same direction the game's pixels run — so both axes take the SAME
 * form: ndc = 2*u/width - 1, ndc = 2*v/height - 1.  Negating one axis is the
 * classic OpenGL habit, and here it is not a flip but a mirror: the
 * determinant goes negative, the "rotation" becomes a reflection, and on a
 * pre-rotated surface that shows up as the landscape game with every letter
 * backwards and upside down — readable only with the head tilted, and then
 * still mirrored.  A rotation never mirrors text; a reflection always does. */
static inline void enjoer_surface_ortho(int rotation, float buffer_width, float buffer_height,
                                        float *m) {
    float u0 = 0.0f, v0 = 0.0f;
    float ux = 0.0f, uy = 0.0f, vx = 0.0f, vy = 0.0f;
    int i = 0;
    for (i = 0; i < 16; ++i) m[i] = 0.0f;
    /* The affine coefficients of enjoer_surface_map(), read off the origin and
     * the two unit steps: the matrix is then the same mapping the function
     * documents, so the two cannot disagree. */
    enjoer_surface_map(rotation, buffer_width, buffer_height, 0.0f, 0.0f, &u0, &v0);
    enjoer_surface_map(rotation, buffer_width, buffer_height, 1.0f, 0.0f, &ux, &uy);
    enjoer_surface_map(rotation, buffer_width, buffer_height, 0.0f, 1.0f, &vx, &vy);
    {
        const float a = ux - u0, b = vx - u0, c = u0; /* u = a*x + b*y + c */
        const float d = uy - v0, e = vy - v0, f = v0; /* v = d*x + e*y + f */
        const float su = buffer_width > 0.0f ? 2.0f / buffer_width : 0.0f;
        const float sv = buffer_height > 0.0f ? 2.0f / buffer_height : 0.0f;
        /* Vulkan clip space: x right and y down across the framebuffer, so the
         * framebuffer axis that runs down the screen is scaled like the one
         * that runs across it — no sign is negated here, ever (see the
         * function comment: a negation is a mirror, not a rotation). */
        m[0] = a * su;
        m[4] = b * su;
        m[12] = c * su - 1.0f;
        m[1] = d * sv;
        m[5] = e * sv;
        m[13] = f * sv - 1.0f;
        m[10] = 1.0f;
        m[14] = 0.5f;
        m[15] = 1.0f;
    }
}

#ifdef __cplusplus
}
#endif
#endif /* ENJOER_SURFACE_TRANSFORM_H */
