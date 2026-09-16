/* PNG images: the bridge between `image.load("sprites.png")` in a script and the
 * texture array the renderer samples.
 *
 * The registry is deliberately tiny and static (DS_MAX_IMAGES slots, no
 * unloading besides ds_image_reset) because a game's sprites are known at
 * startup.  The Vulkan side polls two counters instead of one:
 * ds_image_generation() moves when the *set* of images changes and the texture
 * array has to be rebuilt, while the dirty mask names the layers whose pixels
 * moved, so the common case is a plain copy into the array that already exists.
 * The font atlases live in the same registry and mutate in place (a glyph bakes
 * on first use — which is exactly when a score gains a new digit), so the text
 * pass marks its own atlas layer dirty rather than asking for a full reload. */
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
/* The set of images (count, names, sizes) changed: a renderer that owns a
 * texture array must rebuild it, not just copy pixels into it. */
uint64_t ds_image_generation(void);
/* Bit n set: layer n's pixels changed since the last ds_image_clear_dirty().
 * DS_MAX_IMAGES is 32, so one uint32_t holds every layer. */
uint32_t ds_image_dirty_mask(void);
void ds_image_clear_dirty(void);
/* Marks every layer dirty: for an in-place atlas update that cannot say which
 * layer it touched. */
void ds_image_touch(void);
/* Same, for the one layer that did change — this is what keeps a font atlas
 * that gains a glyph per tap from re-uploading the whole array. */
void ds_image_touch_layer(int32_t layer);

/* Size of one layer of the renderer's texture array.
 *
 * A renderer that owns a sampler2DArray has to give every layer the same size,
 * so the array is as wide and as tall as the largest image in it and a smaller
 * image keeps a transparent margin on the right and at the bottom.  That makes
 * "this image" and "the layer it lives in" two different coordinate spaces, and
 * the draw batch has to be explicit about which one its UVs are measured in:
 * the GPU samples the layer, so the batch carries layer coordinates, and
 * ds_image_layer_uv() is the one conversion that every producer of texture
 * coordinates goes through. */
int32_t ds_image_layer_width(void);
int32_t ds_image_layer_height(void);

/* Converts a texture coordinate measured against `handle`'s own pixels into one
 * measured against the array layer it is stored in, in place.  Whole-image
 * coordinates (0,0)-(1,1) therefore land on the image's own rectangle inside
 * the layer and never on the transparent margin around it. */
void ds_image_layer_uv(int32_t handle, float *u, float *v);

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
