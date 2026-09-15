/* PNG images: the bridge between `image.load("sprites.png")` in a script and the
 * texture array the renderer samples.
 *
 * The registry is deliberately tiny and static (DS_MAX_IMAGES slots, no
 * unloading besides ds_image_reset) because a game's sprites are known at
 * startup.  ds_image_revision() changes whenever the set of images changes, and
 * that is the only thing the Vulkan side polls: it re-uploads the texture array
 * exactly when a new image appears, never per frame.  The font atlases live in
 * the same registry and mutate in place (new glyphs rasterize on demand), so
 * the text pass calls ds_image_touch() after adding glyphs to trigger the
 * same re-upload. */
#ifndef DS_IMAGE_H
#define DS_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS_MAX_IMAGES 32
#define DS_IMAGE_NAME_LENGTH 48

typedef struct DsImage {
    char name[DS_IMAGE_NAME_LENGTH];
    int32_t width;
    int32_t height;
    uint8_t *rgba; /* tightly packed, 4 bytes per pixel */
} DsImage;

/* Frees every image; called by ds_runtime_shutdown and at startup. */
void ds_image_reset(void);
int32_t ds_image_count(void);
int32_t ds_image_valid(int32_t handle);
int32_t ds_image_width(int32_t handle);
int32_t ds_image_height(int32_t handle);
const DsImage *ds_image_at(int32_t handle);
uint64_t ds_image_revision(void);
/* Bumps the revision without adding an image: for in-place atlas updates. */
void ds_image_touch(void);

/* Handle of an already loaded file, or -1.  Lets `image.load` return the same
 * handle when a script asks for the same sprite sheet twice. */
int32_t ds_image_find(const char *name);
/* Takes ownership of a freshly decoded RGBA8 buffer and returns its handle. */
int32_t ds_image_add(const char *name, uint8_t *rgba, int32_t width, int32_t height);

/* True for the first bytes of a PNG image. */
int ds_png_magic_ok(const uint8_t *bytes, size_t size);
/* Short name of the image format sniffed from the header: "PNG", "JPEG",
 * "GIF", "BMP", "WebP", "TIFF" or "unknown".  Only PNG decodes — the rest is
 * named so the loader can refuse it out loud. */
const char *ds_image_format_name(const uint8_t *bytes, size_t size);

/* Decodes a PNG buffer into RGBA8.  Returns NULL and sets *error on failure. */
uint8_t *ds_png_decode(const uint8_t *data, size_t length, int32_t *width, int32_t *height,
                       const char **error);

#ifdef __cplusplus
}
#endif
#endif /* DS_IMAGE_H */
