/* game.manifest — what an Enjoer game tells the engine about itself.
 *
 * The file is a flat `key = value` list, one setting per line, with `--`
 * comments, i.e. the same lexical style as the scripts.  It is read at startup
 * by src/game.c and by tools/gamepack.py, which generates Android's
 * AndroidManifest.xml from the same values, so a game has one source of truth.
 *
 * The engine is 2D only: there is deliberately no 3D key, and a `cube` key is
 * a hard error that tells the author to delete it.
 *
 * Icons are not part of the format yet: an `icon` key is a hard error rather
 * than something silently ignored.
 */
#ifndef DS_MANIFEST_H
#define DS_MANIFEST_H

#include <stddef.h>

#define DS_MANIFEST_NAME "game.manifest"
#define DS_MANIFEST_TITLE 96
#define DS_MANIFEST_PACKAGE 96
#define DS_MANIFEST_WORD 32
#define DS_MANIFEST_SCRIPTS 32
#define DS_MANIFEST_IMAGES 32
#define DS_MANIFEST_IMAGE_NAME 64
#define DS_MANIFEST_FONTS 8
#define DS_MANIFEST_FONT_NAME 64

typedef struct DsGameManifest {
    char title[DS_MANIFEST_TITLE];
    char author[DS_MANIFEST_WORD];
    char package[DS_MANIFEST_PACKAGE];
    char version[DS_MANIFEST_WORD];
    char orientation[DS_MANIFEST_WORD];
    int version_code;
    int target_fps;
    int resizeable;
    int show_fps;
    float clear_color[3];
    char scripts[DS_MANIFEST_SCRIPTS][DS_MANIFEST_WORD];
    int script_count;
    /* Sprite sheets (`image.load`) and fonts (`font.load`), as named in the
     * manifest.  A device cannot scan assets/, so tools/gamepack.py writes the
     * complete lists into the packaged copy. */
    char images[DS_MANIFEST_IMAGES][DS_MANIFEST_IMAGE_NAME];
    int image_count;
    char fonts[DS_MANIFEST_FONTS][DS_MANIFEST_FONT_NAME];
    int font_count;
    char error[256];
} DsGameManifest;

/* Fills in the defaults used when a key is missing. */
void ds_manifest_default(DsGameManifest *manifest);
/* Parses `text`; returns 0 and fills manifest->error on a bad value. */
int ds_manifest_parse(DsGameManifest *manifest, const char *text, size_t length);
/* True when `name` may be used as an Android package segment (a.b.c). */
int ds_manifest_package_valid(const DsGameManifest *manifest);

#endif /* DS_MANIFEST_H */
