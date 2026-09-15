/* Game fonts: the bridge between `font.load("font.ttf")` in a script and the
 * bytes a text pass rasterizes.
 *
 * Like images, fonts are loaded once through ds_files (a file next to the
 * game folder on the host, a packed asset inside the APK) and kept in a tiny
 * static registry — a game knows its fonts at startup.  ds_font_revision()
 * changes whenever the set changes.
 *
 * What `load` validates today is the file header (TrueType / OpenType magic),
 * so a typo in the name or a non-font file fails loudly at load time instead
 * of silently producing tofu.  Glyph rasterization on the device is the next
 * step; the preview already serves these bytes to the browser, which draws the
 * recorded `render.text` calls in the game's own font.
 */
#ifndef DS_FONT_H
#define DS_FONT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS_MAX_FONTS 8
#define DS_FONT_NAME_LENGTH 64

typedef struct DsFont {
    char name[DS_FONT_NAME_LENGTH];
    uint8_t *bytes;
    size_t size;
} DsFont;

/* True for the first bytes of a TrueType / OpenType font. */
int ds_font_magic_ok(const uint8_t *bytes, size_t size);

/* Frees every font; called by ds_runtime_shutdown and at startup. */
void ds_font_reset(void);
int32_t ds_font_count(void);
int32_t ds_font_valid(int32_t handle);
const DsFont *ds_font_at(int32_t handle);
uint64_t ds_font_revision(void);

/* Handle of an already loaded file, or -1.  Lets `font.load` return the same
 * handle when a script asks for the same font twice. */
int32_t ds_font_find(const char *name);
/* Takes ownership of a freshly read font file and returns its handle. */
int32_t ds_font_add(const char *name, uint8_t *bytes, size_t size);

#ifdef __cplusplus
}
#endif
#endif /* DS_FONT_H */
