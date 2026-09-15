/*
 * Image registry: PNG files decoded once and handed to the renderer as a
 * texture array.  Loading goes through ds_files, so the same name resolves to a
 * file next to the game folder on the host and to a packed sprite inside the APK.
 */
#include "ds_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds_files.h"
#include "engine.h"

/* --- images --------------------------------------------------------------- */

static DsImage images[DS_MAX_IMAGES];
static int image_count;
static uint64_t image_revision = 1;

int32_t ds_image_valid(int32_t handle) {
    return handle >= 0 && handle < image_count && images[handle].rgba ? 1 : 0;
}

int32_t ds_image_count(void) { return image_count; }

int32_t ds_image_width(int32_t handle) { return ds_image_valid(handle) ? images[handle].width : 0; }

int32_t ds_image_height(int32_t handle) { return ds_image_valid(handle) ? images[handle].height : 0; }

uint64_t ds_image_revision(void) { return image_revision; }

int32_t ds_image_find(const char *name) {
    if (!name) return -1;
    for (int32_t handle = 0; handle < image_count; ++handle)
        if (!strcmp(images[handle].name, name)) return handle;
    return -1;
}

int32_t ds_image_add(const char *name, uint8_t *rgba, int32_t width, int32_t height) {
    if (!rgba || image_count >= DS_MAX_IMAGES) return -1;
    DsImage *image = &images[image_count];
    snprintf(image->name, sizeof(image->name), "%s", name ? name : "");
    image->width = width;
    image->height = height;
    image->rgba = rgba;
    ++image_revision;
    return image_count++;
}

const DsImage *ds_image_at(int32_t handle) { return ds_image_valid(handle) ? &images[handle] : NULL; }

void ds_image_reset(void) {
    for (int index = 0; index < image_count; ++index) {
        free(images[index].rgba);
        images[index].rgba = NULL;
        images[index].width = 0;
        images[index].height = 0;
        images[index].name[0] = '\0';
    }
    image_count = 0;
    ++image_revision;
}
