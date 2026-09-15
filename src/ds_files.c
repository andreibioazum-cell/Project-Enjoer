#define _POSIX_C_SOURCE 200809L
#include "ds_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __ANDROID__
#include <android/asset_manager.h>
static AAssetManager *assets;
#else
#include <dirent.h>
#endif

static char root[512];

void ds_files_set_root(const char *value) {
    root[0] = '\0';
    if (!value) return;
    snprintf(root, sizeof(root), "%s", value);
    size_t length = strlen(root);
    while (length > 0 && root[length - 1] == '/') root[--length] = '\0';
}

const char *ds_files_root(void) { return root; }

#ifdef __ANDROID__
void ds_files_set_asset_manager(void *asset_manager) { assets = (AAssetManager *)asset_manager; }
#else
void ds_files_set_asset_manager(void *asset_manager) { (void)asset_manager; }
#endif

char *ds_files_read(const char *name, size_t *length) {
    if (length) *length = 0;
    if (!name || !name[0]) return NULL;
    if (strlen(name) > DS_FILES_NAME * 2) return NULL;

#ifdef __ANDROID__
    if (assets) {
        char path[128];
        snprintf(path, sizeof(path), "game/%s", name);
        AAsset *asset = AAssetManager_open(assets, path, AASSET_MODE_BUFFER);
        if (!asset) return NULL;
        const off_t size = AAsset_getLength(asset);
        if (size <= 0 || size > (8 * 1024 * 1024)) {
            AAsset_close(asset);
            return NULL;
        }
        char *buffer = (char *)malloc((size_t)size + 1);
        if (!buffer) {
            AAsset_close(asset);
            return NULL;
        }
        const int read = AAsset_read(asset, buffer, (size_t)size);
        AAsset_close(asset);
        if (read != (int)size) {
            free(buffer);
            return NULL;
        }
        buffer[size] = '\0';
        if (length) *length = (size_t)size;
        return buffer;
    }
    return NULL;
#else
    char path[768];
    if (root[0]) snprintf(path, sizeof(path), "%s/%s", root, name);
    else snprintf(path, sizeof(path), "%s", name);
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    const long size = ftell(file);
    rewind(file);
    if (size <= 0 || size > (8 * 1024 * 1024)) {
        fclose(file);
        return NULL;
    }
    char *buffer = (char *)malloc((size_t)size + 1);
    if (!buffer) {
        fclose(file);
        return NULL;
    }
    if (fread(buffer, 1, (size_t)size, file) != (size_t)size) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    buffer[size] = '\0';
    fclose(file);
    if (length) *length = (size_t)size;
    return buffer;
#endif
}

#ifndef __ANDROID__
static int has_ds_extension(const char *name) {
    const size_t length = strlen(name);
    return length > 3 && !strcmp(name + length - 3, ".ds");
}

int ds_files_list_ds(char names[][DS_FILES_NAME], int capacity) {
    if (!names || capacity <= 0 || !root[0]) return 0;
    DIR *directory = opendir(root);
    if (!directory) return 0;
    int count = 0;
    struct dirent *entry = NULL;
    while ((entry = readdir(directory)) != NULL) {
        if (!has_ds_extension(entry->d_name)) continue;
        if (strlen(entry->d_name) >= DS_FILES_NAME) continue;
        if (count >= capacity) break;
        snprintf(names[count], DS_FILES_NAME, "%s", entry->d_name);
        ++count;
    }
    closedir(directory);
    /* Stable alphabetical order so a folder loads the same way everywhere. */
    for (int index = 1; index < count; ++index) {
        char key[DS_FILES_NAME];
        snprintf(key, sizeof(key), "%s", names[index]);
        int cursor = index - 1;
        while (cursor >= 0 && strcmp(names[cursor], key) > 0) {
            snprintf(names[cursor + 1], DS_FILES_NAME, "%s", names[cursor]);
            --cursor;
        }
        snprintf(names[cursor + 1], DS_FILES_NAME, "%s", key);
    }
    return count;
}
#else
int ds_files_list_ds(char names[][DS_FILES_NAME], int capacity) {
    (void)names;
    (void)capacity;
    return 0;
}
#endif
