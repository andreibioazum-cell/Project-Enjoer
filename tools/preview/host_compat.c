/* Хост-реализация андроидных зависимостей для превью-сборки: файловый
 * «менеджер ассетов», лог в консоль, клавиатурный буфер и заглушки звука
 * (в браузере звук не нужен). Никакой логики мессенджера здесь нет. */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "runtime.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ── лог ───────────────────────────────────────────────────────────────── */
int __android_log_vprint(int prio, const char *tag, const char *fmt, va_list ap) {
    fprintf(stderr, "[%s] ", tag ? tag : "?");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    (void)prio;
    return 0;
}
int __android_log_print(int prio, const char *tag, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

/* ── файловый «менеджер ассетов» ───────────────────────────────────────── */
struct AAssetManager { char root[512]; };
struct AAsset {
    unsigned char *data;
    size_t size;
    size_t pos;
};
static struct AAssetManager host_am;

AAssetManager *host_asset_manager(const char *root) {
    snprintf(host_am.root, sizeof(host_am.root), "%s", root ? root : "game/assets");
    return &host_am;
}

AAsset *AAssetManager_open(AAssetManager *mgr, const char *filename, int mode) {
    (void)mode;
    if (!mgr || !filename) return NULL;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", mgr->root, filename);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0) { fclose(f); return NULL; }
    AAsset *a = (AAsset *)calloc(1, sizeof(*a));
    if (!a) { fclose(f); return NULL; }
    a->data = (unsigned char *)malloc((size_t)len);
    if (!a->data) { free(a); fclose(f); return NULL; }
    a->size = fread(a->data, 1, (size_t)len, f);
    fclose(f);
    return a;
}

off_t AAsset_getLength(AAsset *a) { return a ? (off_t)a->size : 0; }

int AAsset_read(AAsset *a, void *buf, size_t count) {
    if (!a || a->pos >= a->size) return 0;
    size_t left = a->size - a->pos;
    if (count > left) count = left;
    memcpy(buf, a->data + a->pos, count);
    a->pos += count;
    return (int)count;
}

void AAsset_close(AAsset *a) {
    if (!a) return;
    free(a->data);
    free(a);
}

/* ── клавиатура (эмуляция системного IME) ──────────────────────────────── */
#define KB_BUF 384
static char kb_text[KB_BUF];
static int kb_len = 0, kb_show_flag = 0, kb_enter_flag = 0;
static pthread_mutex_t kb_lock = PTHREAD_MUTEX_INITIALIZER;

static void kb_append_cp(unsigned int cp) {
    char u[5];
    int n = 0;
    if (cp < 0x80) { u[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { u[0] = (char)(0xC0 | (cp >> 6)); u[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) { u[0] = (char)(0xE0 | (cp >> 12)); u[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else if (cp <= 0x10FFFF) { u[0] = (char)(0xF0 | (cp >> 18)); u[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); u[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); u[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (n > 0 && kb_len + n < KB_BUF) {
        memcpy(kb_text + kb_len, u, (size_t)n);
        kb_len += n;
        kb_text[kb_len] = '\0';
    }
}

void keyboard_show(void) { pthread_mutex_lock(&kb_lock); kb_show_flag = 1; pthread_mutex_unlock(&kb_lock); }
void keyboard_hide(void) { pthread_mutex_lock(&kb_lock); kb_show_flag = 0; pthread_mutex_unlock(&kb_lock); }
int keyboard_visible(void) { pthread_mutex_lock(&kb_lock); int v = kb_show_flag; pthread_mutex_unlock(&kb_lock); return v; }
int keyboard_uses_editor(void) { return 0; }
void ds_set_activity(void *act) { (void)act; }

const char *keyboard_get_text(void) {
    char copy[KB_BUF];
    pthread_mutex_lock(&kb_lock);
    snprintf(copy, sizeof(copy), "%s", kb_text);
    pthread_mutex_unlock(&kb_lock);
    char *dup = (char *)malloc(strlen(copy) + 1);
    if (dup) strcpy(dup, copy);
    return dup ? dup : "";
}

const char *keyboard_get_raw(void) {
    pthread_mutex_lock(&kb_lock);
    static char snap[KB_BUF];
    snprintf(snap, sizeof(snap), "%s", kb_text);
    pthread_mutex_unlock(&kb_lock);
    return snap;
}

void keyboard_clear(void) {
    pthread_mutex_lock(&kb_lock);
    kb_text[0] = '\0';
    kb_len = 0;
    kb_enter_flag = 0;
    pthread_mutex_unlock(&kb_lock);
}

int keyboard_enter_pressed(void) {
    pthread_mutex_lock(&kb_lock);
    int e = kb_enter_flag;
    kb_enter_flag = 0;
    pthread_mutex_unlock(&kb_lock);
    return e;
}

void keyboard_type(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    if (!p) return;
    pthread_mutex_lock(&kb_lock);
    while (*p) {
        unsigned int cp;
        if (*p < 0x80) cp = *p++;
        else if ((*p & 0xE0) == 0xC0 && (p[1] & 0xC0) == 0x80) { cp = ((p[0] & 0x1F) << 6) | (p[1] & 0x3F); p += 2; }
        else if ((*p & 0xF0) == 0xE0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80) { cp = ((p[0] & 15) << 12) | ((p[1] & 63) << 6) | (p[2] & 63); p += 3; }
        else if ((*p & 0xF8) == 0xF0 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 && (p[3] & 0xC0) == 0x80) { cp = ((p[0] & 7) << 18) | ((p[1] & 63) << 12) | ((p[2] & 63) << 6) | (p[3] & 63); p += 4; }
        else { p++; continue; }
        if (cp == '\n' || cp == '\r') kb_enter_flag = 1;
        else if (cp >= 0x20) kb_append_cp(cp);
    }
    pthread_mutex_unlock(&kb_lock);
}

void keyboard_backspace(void) {
    pthread_mutex_lock(&kb_lock);
    while (kb_len > 0) {
        unsigned char c = (unsigned char)kb_text[--kb_len];
        kb_text[kb_len] = '\0';
        if ((c & 0xC0) != 0x80) break;
    }
    pthread_mutex_unlock(&kb_lock);
}

int keyboard_handle_key(int keycode, int action, int meta) {
    (void)keycode; (void)action; (void)meta;
    return 0;
}

void keyboard_commit_utf8(const char *utf8) { keyboard_type(utf8); }

/* только для хоста: нажать Enter из браузерной клавиатуры */
void host_key_enter(void) {
    pthread_mutex_lock(&kb_lock);
    kb_enter_flag = 1;
    pthread_mutex_unlock(&kb_lock);
}

/* ── звук: в превью тихо ───────────────────────────────────────────────── */
int snd_load(const char *name) { (void)name; return 0; }
int snd_play(const char *name) { (void)name; return 0; }
int sound_play(const char *name) { (void)name; return 0; }
int snd_loop(const char *name) { (void)name; return 0; }
void snd_stop(const char *name) { (void)name; }
int snd_playing(const char *name) { (void)name; return 0; }
void snd_volume(const char *name, double volume) { (void)name; (void)volume; }
void snd_stop_all(void) {}
int ds_sound_init(AAssetManager *assets) { (void)assets; return 1; }
void ds_sound_shutdown(void) {}
void ds_sound_pause(void) {}
void ds_sound_resume(void) {}
void ds_sound_set_java_vm(void *vm) { (void)vm; }
