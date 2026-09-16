/* game.manifest reader.  Written by hand rather than generated so that the
 * engine can read a game folder with nothing but libc. */
#include "ds_manifest.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim(char *text) {
    char *start = text;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') ++start;
    if (start != text) memmove(text, start, strlen(start) + 1);
    size_t length = strlen(text);
    while (length > 0 && (text[length - 1] == ' ' || text[length - 1] == '\t' ||
                           text[length - 1] == '\r' || text[length - 1] == '\n'))
        text[--length] = '\0';
}

/* Removes a trailing `--` comment of ONE line, ignoring the marker inside a
 * quoted value.  The caller splits the buffer on newlines first, so `\n` here
 * would already be a terminator; stopping at it keeps the helper safe to call on
 * a whole file too. */
static void strip_comment(char *text) {
    int in_quote = 0;
    char quote = 0;
    for (char *cursor = text; *cursor && *cursor != '\n'; ++cursor) {
        if (in_quote) {
            if (*cursor == '\\' && cursor[1]) ++cursor;
            else if (*cursor == quote) in_quote = 0;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            in_quote = 1;
            quote = *cursor;
            continue;
        }
        if (cursor[0] == '-' && cursor[1] == '-') {
            cursor[0] = '\0';
            return;
        }
        if (cursor[0] == '/' && cursor[1] == '/') {
            cursor[0] = '\0';
            return;
        }
    }
}

static void fail(DsGameManifest *manifest, const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(manifest->error, sizeof(manifest->error), format, args);
    va_end(args);
}

static int hex_component(const char *text, int *out) {
    int value = 0;
    for (int index = 0; index < 2; ++index) {
        const char digit = text[index];
        value *= 16;
        if (digit >= '0' && digit <= '9') value += digit - '0';
        else if (digit >= 'a' && digit <= 'f') value += digit - 'a' + 10;
        else if (digit >= 'A' && digit <= 'F') value += digit - 'A' + 10;
        else return 0;
    }
    *out = value;
    return 1;
}

static int read_string_value(const char *value, char *out, size_t capacity) {
    const size_t length = strlen(value);
    if (length >= 2 && ((value[0] == '"' && value[length - 1] == '"') ||
                         (value[0] == '\'' && value[length - 1] == '\''))) {
        ++value;
        size_t size = length - 2;
        if (size >= capacity) size = capacity - 1;
        size_t used = 0;
        for (size_t index = 0; index < size; ++index) {
            char character = value[index];
            if (character == '\\' && index + 1 < size) {
                const char next = value[++index];
                if (next == 'n') character = '\n';
                else if (next == 't') character = '\t';
                else character = next;
            }
            out[used++] = character;
        }
        out[used] = '\0';
        return 1;
    }
    /* Unquoted values are accepted for friendliness, e.g. `version = 1.0`. */
    snprintf(out, capacity, "%s", value);
    return 1;
}

void ds_manifest_default(DsGameManifest *manifest) {
    memset(manifest, 0, sizeof(*manifest));
    snprintf(manifest->title, sizeof(manifest->title), "Enjoer Game");
    snprintf(manifest->author, sizeof(manifest->author), "unknown");
    snprintf(manifest->package, sizeof(manifest->package), "com.cb4.game");
    snprintf(manifest->version, sizeof(manifest->version), "1.0");
    snprintf(manifest->orientation, sizeof(manifest->orientation), "sensorLandscape");
    manifest->version_code = 1;
    manifest->target_fps = 60;
    manifest->resizeable = 1;
    manifest->show_fps = 0;
    manifest->clear_color[0] = 0.05f;
    manifest->clear_color[1] = 0.08f;
    manifest->clear_color[2] = 0.15f;
}

int ds_manifest_package_valid(const DsGameManifest *manifest) {
    int segments = 0;
    int in_segment = 0;
    if (!manifest) return 0;
    for (const char *cursor = manifest->package; *cursor; ++cursor) {
        if (*cursor == '.') {
            if (!in_segment) return 0;
            in_segment = 0;
            ++segments;
            continue;
        }
        if (!isalnum((unsigned char)*cursor) && *cursor != '_') return 0;
        if (!in_segment) {
            if (*cursor >= '0' && *cursor <= '9') return 0;
            in_segment = 1;
        }
    }
    return in_segment && segments >= 1;
}

/* Parses `["a", "b"]` into a fixed name table.  `extensions` is a `|` separated
 * list of required suffixes, or NULL for no check. */
static int read_name_list(DsGameManifest *manifest, const char *value, char *table,
                           size_t name_size, int capacity, int line_number, const char *key,
                           const char *extensions) {
    char list[1024];
    snprintf(list, sizeof(list), "%s", value);
    for (char *cursor = list; *cursor; ++cursor)
        if (*cursor == '[' || *cursor == ']' || *cursor == ',') *cursor = ' ';
    int count = 0;
    char *cursor = list;
    while (*cursor) {
        while (*cursor == ' ' || *cursor == '"' || *cursor == '\'' || *cursor == '\t')
            ++cursor;
        if (!*cursor) break;
        char *end = cursor;
        while (*end && *end != ' ' && *end != '"' && *end != '\'') ++end;
        const size_t size = (size_t)(end - cursor);
        if (size == 0) break;
        if (count >= capacity) {
            fail(manifest, "%s:%d: больше %d имён в '%s'", DS_MANIFEST_NAME, line_number,
                 capacity, key);
            return -1;
        }
        if (size >= name_size) {
            fail(manifest, "%s:%d: имя в '%s' длиннее %d символов", DS_MANIFEST_NAME,
                 line_number, key, (int)name_size - 1);
            return -1;
        }
        if (extensions) {
            int known = 0;
            char wanted[64];
            snprintf(wanted, sizeof(wanted), "%s", extensions);
            for (char *extension = strtok(wanted, "|"); extension;
                 extension = strtok(NULL, "|")) {
                const size_t length = strlen(extension);
                if (size > length && !memcmp(cursor + size - length, extension, length))
                    known = 1;
            }
            if (!known) {
                fail(manifest, "%s:%d: '%s' ждёт файлы %s", DS_MANIFEST_NAME, line_number, key,
                     extensions);
                return -1;
            }
        }
        memcpy(table + (size_t)count * name_size, cursor, size);
        table[(size_t)count * name_size + size] = '\0';
        ++count;
        cursor = end;
    }
    return count;
}

int ds_manifest_parse(DsGameManifest *manifest, const char *text, size_t length) {
    if (!manifest || !text) return 0;
    char *copy = (char *)malloc(length + 1);
    if (!copy) return 0;
    memcpy(copy, text, length);
    copy[length] = '\0';

    int line_number = 0;
    int ok = 1;
    char *cursor = copy;
    const char *end = copy + length;
    while (ok && cursor < end) {
        /* Split in place: every key is then a plain NUL-terminated string, and
         * a `--` comment can never eat the rest of the file. */
        char *line = cursor;
        char *newline = (char *)memchr(cursor, '\n', (size_t)(end - cursor));
        if (newline) {
            *newline = '\0';
            cursor = newline + 1;
        } else {
            cursor = (char *)end;
        }
        ++line_number;
        strip_comment(line);
        trim(line);
        if (!line[0]) continue;
        char *equal = strchr(line, '=');
        if (!equal) {
            fail(manifest, "%s:%d: ожидается 'key = value'", DS_MANIFEST_NAME, line_number);
            ok = 0;
            break;
        }
        *equal = '\0';
        char *key = line;
        char *value = equal + 1;
        trim(key);
        trim(value);
        const size_t key_length = strlen(key);
        for (size_t index = 0; index < key_length; ++index) {
            if (!islower((unsigned char)key[index]) && key[index] != '_') {
                fail(manifest, "%s:%d: имя '%s' недопустимо (только строчные буквы и '_')",
                     DS_MANIFEST_NAME, line_number, key);
                ok = 0;
                break;
            }
        }
        if (!ok) break;

        if (!strcmp(key, "icon") || !strcmp(key, "icons") || !strncmp(key, "icon_", 5)) {
            fail(manifest, "%s:%d: иконки пока не поддерживаются уберите ключ '%s' "
                           "(иконки добавим позже, вместе с растровыми ресурсами)",
                 DS_MANIFEST_NAME, line_number, key);
            ok = 0;
            break;
        }
        if (!strcmp(key, "title") || !strcmp(key, "author") || !strcmp(key, "package") ||
            !strcmp(key, "version") || !strcmp(key, "orientation")) {
            char *target = NULL;
            size_t capacity = 0;
            if (!strcmp(key, "title")) {
                target = manifest->title;
                capacity = sizeof(manifest->title);
            } else if (!strcmp(key, "author")) {
                target = manifest->author;
                capacity = sizeof(manifest->author);
            } else if (!strcmp(key, "package")) {
                target = manifest->package;
                capacity = sizeof(manifest->package);
            } else if (!strcmp(key, "version")) {
                target = manifest->version;
                capacity = sizeof(manifest->version);
            } else {
                target = manifest->orientation;
                capacity = sizeof(manifest->orientation);
            }
            read_string_value(value, target, capacity);
            if (!strcmp(key, "orientation")) {
                static const char *const ALLOWED[] = {"portrait", "landscape", "sensor",
                                                      "sensorLandscape", "sensorPortrait"};
                int known = 0;
                for (int index = 0; index < (int)(sizeof(ALLOWED) / sizeof(ALLOWED[0])); ++index)
                    if (!strcmp(target, ALLOWED[index])) known = 1;
                if (!known) {
                    fail(manifest, "%s:%d: orientation='%s', ожидалось portrait/landscape/sensor/"
                                   "sensorLandscape/sensorPortrait",
                         DS_MANIFEST_NAME, line_number, target);
                    ok = 0;
                    break;
                }
            }
            if (!strcmp(key, "package") && !ds_manifest_package_valid(manifest)) {
                fail(manifest, "%s:%d: package='%s' не похоже на com.name.game", DS_MANIFEST_NAME,
                     line_number, target);
                ok = 0;
                break;
            }
            continue;
        }
        if (!strcmp(key, "version_code") || !strcmp(key, "target_fps")) {
            const int number = atoi(value);
            if (!strcmp(key, "version_code")) {
                if (number < 1) {
                    fail(manifest, "%s:%d: version_code должно быть >= 1", DS_MANIFEST_NAME,
                         line_number);
                    ok = 0;
                    break;
                }
                manifest->version_code = number;
            } else {
                if (number < 20 || number > 240) {
                    fail(manifest, "%s:%d: target_fps в диапазоне 20..240", DS_MANIFEST_NAME,
                         line_number);
                    ok = 0;
                    break;
                }
                manifest->target_fps = number;
            }
            continue;
        }
        if (!strcmp(key, "cube")) {
            fail(manifest, "%s:%d: 3D-куб удалён — Enjoer теперь только 2D, уберите ключ 'cube'",
                 DS_MANIFEST_NAME, line_number);
            ok = 0;
            break;
        }
        if (!strcmp(key, "resizeable") || !strcmp(key, "show_fps")) {
            int flag = 0;
            if (!strcmp(value, "true") || !strcmp(value, "1")) flag = 1;
            else if (!strcmp(value, "false") || !strcmp(value, "0")) flag = 0;
            else {
                fail(manifest, "%s:%d: '%s' принимает true или false", DS_MANIFEST_NAME,
                     line_number, key);
                ok = 0;
                break;
            }
            if (!strcmp(key, "resizeable")) manifest->resizeable = flag;
            else manifest->show_fps = flag;
            continue;
        }
        if (!strcmp(key, "clear_color")) {
            float components[3] = {0.0f, 0.0f, 0.0f};
            if (value[0] == '#' && strlen(value) == 7) {
                for (int index = 0; index < 3; ++index) {
                    int byte = 0;
                    if (!hex_component(value + 1 + index * 2, &byte)) {
                        fail(manifest, "%s:%d: не цвет '%s'", DS_MANIFEST_NAME, line_number, value);
                        ok = 0;
                        break;
                    }
                    components[index] = (float)byte / 255.0f;
                }
                if (!ok) break;
            } else {
                char triple[64];
                snprintf(triple, sizeof(triple), "%s", value);
                for (char *cursor = triple; *cursor; ++cursor)
                    if (*cursor == ',' || *cursor == '[') *cursor = ' ';
                int count = 0;
                char *cursor = triple;
                while (*cursor && count < 3) {
                    char *end = NULL;
                    const double parsed = strtod(cursor, &end);
                    if (end == cursor) break;
                    components[count++] = (float)parsed;
                    cursor = end;
                }
                if (count != 3) {
                    fail(manifest, "%s:%d: clear_color ожидает '#rrggbb' или три числа 0..1",
                         DS_MANIFEST_NAME, line_number);
                    ok = 0;
                    break;
                }
            }
            for (int index = 0; index < 3; ++index) {
                if (components[index] < 0.0f || components[index] > 1.0f) {
                    fail(manifest, "%s:%d: компоненты clear_color в диапазоне 0..1",
                         DS_MANIFEST_NAME, line_number);
                    ok = 0;
                    break;
                }
                manifest->clear_color[index] = components[index];
            }
            if (!ok) break;
            continue;
        }
        if (!strcmp(key, "scripts")) {
            const int count = read_name_list(manifest, value, &manifest->scripts[0][0],
                                             sizeof(manifest->scripts[0]), DS_MANIFEST_SCRIPTS,
                                             line_number, key, ".ds");
            if (count < 0) {
                ok = 0;
                break;
            }
            manifest->script_count = count;
            continue;
        }
        if (!strcmp(key, "images")) {
            const int count = read_name_list(manifest, value, &manifest->images[0][0],
                                             sizeof(manifest->images[0]), DS_MANIFEST_IMAGES,
                                             line_number, key, ".png");
            if (count < 0) {
                ok = 0;
                break;
            }
            manifest->image_count = count;
            continue;
        }
        if (!strcmp(key, "fonts")) {
            const int count = read_name_list(manifest, value, &manifest->fonts[0][0],
                                             sizeof(manifest->fonts[0]), DS_MANIFEST_FONTS,
                                             line_number, key, ".ttf|.otf");
            if (count < 0) {
                ok = 0;
                break;
            }
            manifest->font_count = count;
            continue;
        }
        fail(manifest, "%s:%d: неизвестный ключ '%s' (можно: title, author, package, version, "
                       "version_code, orientation, target_fps, resizeable, show_fps, "
                       "clear_color, scripts, images, fonts)",
             DS_MANIFEST_NAME, line_number, key);
        ok = 0;
    }

    free(copy);
    return ok;
}
