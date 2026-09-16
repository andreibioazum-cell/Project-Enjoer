/*
 * Font registry: TTF/OTF files read once and kept for the text pass.
 * Loading goes through ds_files, so the same name resolves to a file next to
 * the game folder on the host and to a packed font inside the APK.
 */
#include "ds_font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DsFont fonts[DS_MAX_FONTS];
static int font_count;
static uint64_t font_revision = 1;

int ds_font_magic_ok(const uint8_t *bytes, size_t size) {
    if (!bytes || size < 4) return 0;
    /* 0x00010000 (TrueType), 'true' / 'typ1' (Mac), 'OTTO' (CFF/OpenType). */
    if (bytes[0] == 0x00 && bytes[1] == 0x01 && bytes[2] == 0x00 && bytes[3] == 0x00)
        return 1;
    if (!memcmp(bytes, "true", 4) || !memcmp(bytes, "typ1", 4) || !memcmp(bytes, "OTTO", 4))
        return 1;
    return 0;
}

int32_t ds_font_valid(int32_t handle) {
    return handle >= 0 && handle < font_count && fonts[handle].bytes ? 1 : 0;
}

int32_t ds_font_count(void) { return font_count; }

uint64_t ds_font_revision(void) { return font_revision; }

int32_t ds_font_find(const char *name) {
    if (!name) return -1;
    for (int32_t handle = 0; handle < font_count; ++handle)
        if (!strcmp(fonts[handle].name, name)) return handle;
    return -1;
}

int32_t ds_font_add(const char *name, uint8_t *bytes, size_t size) {
    if (!bytes || !size || font_count >= DS_MAX_FONTS) return -1;
    DsFont *font = &fonts[font_count];
    snprintf(font->name, sizeof(font->name), "%s", name ? name : "");
    font->bytes = bytes;
    font->size = size;
    ++font_revision;
    return font_count++;
}

const DsFont *ds_font_at(int32_t handle) { return ds_font_valid(handle) ? &fonts[handle] : NULL; }

void ds_font_reset(void) {
    for (int index = 0; index < font_count; ++index) {
        free(fonts[index].bytes);
        fonts[index].bytes = NULL;
        fonts[index].size = 0;
        fonts[index].name[0] = '\0';
    }
    font_count = 0;
    ++font_revision;
}
