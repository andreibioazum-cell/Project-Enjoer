/* Game file access for Enjoer: a folder on the host, `assets/` inside the APK on
 * Android.  The engine only ever asks for a file by name, which is what lets the
 * same .ds sources run from `games/brick` during development and from the packed
 * archive on a device. */
#ifndef DS_FILES_H
#define DS_FILES_H

#include <stddef.h>

#define DS_FILES_NAME 48

#ifdef __cplusplus
extern "C" {
#endif

/* Host only: the directory that holds game.manifest and the .ds files. */
void ds_files_set_root(const char *root);
const char *ds_files_root(void);
void ds_files_set_asset_manager(void *asset_manager);

/* Returns a malloc'd NUL terminated buffer, or NULL when the file is missing. */
char *ds_files_read(const char *name, size_t *length);

/* Lists `*.ds` files in the game folder, sorted.  On Android an archive cannot
 * be enumerated, so the manifest's `scripts` list is the source of truth and
 * this returns 0. */
int ds_files_list_ds(char names[][DS_FILES_NAME], int capacity);

#ifdef __cplusplus
}
#endif
#endif /* DS_FILES_H */
