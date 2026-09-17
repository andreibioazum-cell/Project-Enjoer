/*
 * DimScript runtime — STRICT COMPILER MODE, MANUAL MEMORY, NO REFCOUNT.
 * Speed like C, AOT to machine code.
 *
 * Model:
 *   - ds_alloc = malloc header + payload, zeroed, store dtor
 *   - ds_release = call dtor + free
 *   - ds_keep = identity (no cost)
 *   - ds_assign / ds_move = raw *slot = value
 *   - ds_release_slot = free + NULL
 *   - strings/lists are borrowed, never auto-released
 *   - literals are immortal, allocated once
 */

#define _POSIX_C_SOURCE 200809L
#include "dimscript_runtime.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds_files.h"
#include "ds_font.h"
#include "ds_image.h"
#include "ds_ttf.h"
#include "engine.h"
#include "enjoer_draw.h"

typedef struct DsHeader {
    DsDtor dtor;
} DsHeader;

struct DsString {
    uint32_t length;
    char bytes[];
};

#define DS_MAX_LIST_CAPACITY ((int64_t)1 << 24)

static DsHeader *header_of(void *value) {
    return value ? (DsHeader*)value - 1 : NULL;
}

void *ds_alloc(size_t size, DsDtor dtor) {
    DsHeader *h = (DsHeader*)calloc(1, sizeof(DsHeader) + size);
    if (!h) {
        ds_fail("DimScript: out of memory");
        return NULL;
    }
    h->dtor = dtor;
    return h + 1;
}

void ds_free(void *value) {
    if (!value) return;
    free(header_of(value));
}

void ds_assign(void **slot, void *value) {
    *slot = value;
}

void ds_move(void **slot, void *value) {
    *slot = value;
}

void ds_release_slot(void **slot) {
    if (!slot) return;
    void *v = *slot;
    *slot = NULL;
    if (!v) return;
    DsHeader *h = header_of(v);
    if (h->dtor) h->dtor(v);
    free(h);
}

void *ds_require(void *value, const char *where) {
    if (!value) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%s: nil deref", where ? where : "DimScript");
        ds_fail(msg);
    }
    return value;
}

/* --- errors --- */
void ds_fail(const char *message) {
    app_fail("%s", message ? message : "DimScript error");
}
void ds_fail_where(const char *where, const char *message) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s: %s", where ? where : "DimScript", message);
    ds_fail(buf);
}
void ds_fail_list_index(const char *where, int64_t index, int64_t count) {
    app_fail("%s: index %lld out of %lld", where ? where : "DimScript",
             (long long)index, (long long)count);
}
double ds_div(double l, double r, const char *where) {
    if (r == 0.0) ds_fail_where(where, "div by zero");
    return l / r;
}
double ds_mod(double l, double r, const char *where) {
    if (r == 0.0) ds_fail_where(where, "mod by zero");
    double res = fmod(l, r);
    if (res != 0.0 && ((res < 0.0) != (r < 0.0))) res += r;
    return res;
}
int64_t ds_imod(int64_t l, int64_t r, const char *where) {
    if (r == 0) ds_fail_where(where, "mod by zero");
    int64_t res = l % r;
    if (res != 0 && ((res < 0) != (r < 0))) res += r;
    return res;
}

/* --- strings --- */
static DsString *string_alloc(size_t len) {
    DsString *s = (DsString*)ds_alloc(sizeof(DsString) + len + 1, NULL);
    s->length = (uint32_t)len;
    s->bytes[len] = '\0';
    return s;
}

DsString *ds_string_new(const char *bytes, size_t length) {
    DsString *s = string_alloc(length);
    if (length && bytes) memcpy(s->bytes, bytes, length);
    return s;
}

DsString *ds_string_dup(const DsString *value) {
    DsString *copy = NULL;
    if (!value) return NULL;
    copy = string_alloc(value->length);
    if (value->length) memcpy(copy->bytes, value->bytes, value->length);
    return copy;
}

void ds_string_replace(DsString **slot, DsString *value) {
    DsString *fresh = ds_string_dup(value);
    if (!slot) {
        ds_release(fresh);
        return;
    }
    if (*slot) ds_release(*slot);
    *slot = fresh;
}

void ds_string_dtor(void *value) { ds_release(value); }

static DsString **interned = NULL;
static int interned_count = 0;

DsString **ds_intern_literals(const DsLiteralSource *sources, int count) {
    if (interned) return interned;
    if (count <= 0) return NULL;
    interned = (DsString**)calloc((size_t)count, sizeof(DsString*));
    if (!interned) return NULL;
    for (int i = 0; i < count; ++i) {
        DsString *s = NULL;
        for (int j = 0; j < i; ++j) {
            if (interned[j] && interned[j]->length == sources[i].length &&
                !memcmp(interned[j]->bytes, sources[i].bytes, sources[i].length)) {
                s = interned[j];
                break;
            }
        }
        if (!s) {
            s = string_alloc(sources[i].length);
            memcpy(s->bytes, sources[i].bytes, sources[i].length);
        }
        interned[i] = s;
    }
    interned_count = count;
    return interned;
}

static void free_interned(void) {
    if (!interned) return;
    for (int i = 0; i < interned_count; ++i) {
        DsString *s = interned[i];
        int dup = 0;
        for (int j = 0; j < i; ++j) if (interned[j] == s) { dup = 1; break; }
        if (!dup && s) free(header_of(s));
    }
    free(interned);
    interned = NULL;
    interned_count = 0;
}

const char *ds_cstr(const DsString *v) { return v ? v->bytes : ""; }
int64_t ds_string_length(const DsString *v) { return v ? (int64_t)v->length : 0; }
int32_t ds_string_compare(const DsString *l, const DsString *r) {
    int res = strcmp(ds_cstr(l), ds_cstr(r));
    return res < 0 ? -1 : res > 0 ? 1 : 0;
}
DsString *ds_string_char_at(const DsString *v, int64_t idx, const char *where) {
    int64_t len = v ? (int64_t)v->length : 0;
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) {
        ds_fail_list_index(where, idx, len);
        return NULL;
    }
    return ds_string_new(v->bytes + idx, 1);
}
DsString *ds_int_to_string(int64_t value) {
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld", (long long)value);
    return ds_string_new(buf, len > 0 ? (size_t)len : 0);
}
DsString *ds_float_to_string(double value) {
    char buf[48];
    int len = snprintf(buf, sizeof(buf), "%.6g", value);
    if (len <= 0) len = 0;
    if (!strchr(buf, '.') && !strchr(buf, 'e') && !strchr(buf, 'n') &&
        !strchr(buf, 'i') && len < (int)sizeof(buf)-2) {
        buf[len++] = '.';
        buf[len++] = '0';
        buf[len] = '\0';
    }
    return ds_string_new(buf, (size_t)len);
}
DsString *ds_bool_to_string(int v) {
    return v ? ds_string_new("true",4) : ds_string_new("false",5);
}
DsString *ds_concat(DsString *left, DsString *right) {
    size_t ll = left ? left->length : 0;
    size_t rl = right ? right->length : 0;
    DsString *res = string_alloc(ll + rl);
    if (ll) memcpy(res->bytes, left->bytes, ll);
    if (rl) memcpy(res->bytes + ll, right->bytes, rl);
    return res; /* borrows inputs, no release */
}
DsString *ds_text_join(int count, ...) {
    va_list args;
    size_t total = 0;
    DsString **parts = count > 0 ? (DsString**)calloc((size_t)count, sizeof(DsString*)) : NULL;
    va_start(args, count);
    for (int i = 0; i < count; ++i) {
        parts[i] = va_arg(args, DsString*);
        total += parts[i] ? parts[i]->length : 0;
    }
    va_end(args);
    DsString *res = string_alloc(total);
    size_t used = 0;
    for (int i = 0; i < count; ++i) {
        if (parts[i] && parts[i]->length) {
            memcpy(res->bytes + used, parts[i]->bytes, parts[i]->length);
            used += parts[i]->length;
        }
    }
    free(parts);
    return res;
}
void ds_log(DsString *text) {
    app_log("%s", ds_cstr(text));
}
double ds_number_of_text(DsString *text) {
    return text ? strtod(text->bytes, NULL) : 0.0;
}

/* --- lists --- */
struct DsList {
    int kind;
    size_t elem_size;
    DsDtor elem_dtor;
    int64_t count;
    int64_t capacity;
    void *items;
};

static int element_is_reference(int kind) {
    return kind == DS_ELEM_STRING || kind == DS_ELEM_LIST || kind == DS_ELEM_OBJECT;
}
static size_t element_size_for(int kind) {
    switch(kind) {
        case DS_ELEM_FLOAT: return sizeof(double);
        case DS_ELEM_INT: return sizeof(int64_t);
        case DS_ELEM_BOOL: return sizeof(int);
        default: return sizeof(void*);
    }
}
static void *element_at(const DsList *list, int64_t idx) {
    return (char*)list->items + (size_t)idx * list->elem_size;
}

DsList *ds_list_new(int kind, size_t elem_size, DsDtor elem_dtor, int64_t capacity) {
    DsList *list = (DsList*)ds_alloc(sizeof(DsList), ds_list_destroy);
    list->kind = kind;
    list->elem_size = element_size_for(kind);
    if (element_is_reference(kind) && elem_size) {
        size_t p = sizeof(void*);
        list->elem_size = ((elem_size + p - 1) / p) * p;
    }
    list->elem_dtor = elem_dtor;
    if (capacity > 0) ds_list_reserve(list, capacity);
    return list;
}
void ds_list_destroy(void *value) {
    DsList *list = (DsList*)value;
    if (!list) return;
    /* manual: free items, but not elements unless dtor says */
    if (list->elem_dtor) {
        for (int64_t i = 0; i < list->count; ++i) {
            void *e = *(void**)element_at(list, i);
            if (e) list->elem_dtor(e);
        }
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}
int64_t ds_list_count(const DsList *list) { return list ? list->count : 0; }
void ds_list_reserve(DsList *list, int64_t cap) {
    if (!list || cap <= list->capacity) return;
    if (cap > DS_MAX_LIST_CAPACITY) ds_fail("list too big");
    int64_t grown = list->capacity ? list->capacity : 4;
    while (grown < cap) grown *= 2;
    void *items = realloc(list->items, (size_t)grown * list->elem_size);
    if (!items) ds_fail("out of memory growing list");
    list->items = items;
    list->capacity = grown;
}
static void list_grow(DsList *list, int64_t need) {
    if (need > list->capacity) ds_list_reserve(list, need);
}
static void *list_slot(DsList *list, int64_t idx, const char *where) {
    if (idx < 0) idx += list->count;
    if (idx < 0 || idx >= list->count) {
        ds_fail_list_index(where, idx, list->count);
        return NULL;
    }
    return (char*)list->items + (size_t)idx * list->elem_size;
}
static void *list_append_slot(DsList *list) {
    list_grow(list, list->count + 1);
    return (char*)list->items + (size_t)(list->count++) * list->elem_size;
}
static void *object_slot(const DsList *list, int64_t idx, const char *where) {
    return list_slot((DsList*)list, idx, where);
}
void ds_list_push_float(DsList *l, double v) { *(double*)list_append_slot(l) = v; }
void ds_list_push_int(DsList *l, int64_t v) { *(int64_t*)list_append_slot(l) = v; }
void ds_list_push_bool(DsList *l, int v) { *(int*)list_append_slot(l) = v ? 1 : 0; }
void ds_list_push_string(DsList *l, DsString *v) { *(void**)list_append_slot(l) = ds_string_dup(v); }
void ds_list_push_list(DsList *l, DsList *v) { *(void**)list_append_slot(l) = v; }
void ds_list_push_object(DsList *l, void *v) { *(void**)list_append_slot(l) = v; }

double ds_list_get_float(const DsList *l, int64_t i, const char *w) { return *(double*)object_slot(l,i,w); }
int64_t ds_list_get_int(const DsList *l, int64_t i, const char *w) { return *(int64_t*)object_slot(l,i,w); }
int ds_list_get_bool(const DsList *l, int64_t i, const char *w) { return *(int*)object_slot(l,i,w); }
DsString *ds_list_get_string(const DsList *l, int64_t i, const char *w) { return *(DsString**)object_slot(l,i,w); }
DsList *ds_list_get_list(const DsList *l, int64_t i, const char *w) { return *(DsList**)object_slot(l,i,w); }
void *ds_list_get_object(const DsList *l, int64_t i, const char *w) { return *(void**)object_slot(l,i,w); }

void ds_list_set_float(DsList *l, int64_t i, double v) { double *s = (double*)list_slot(l,i,NULL); if(s) *s=v; }
void ds_list_set_int(DsList *l, int64_t i, int64_t v) { int64_t *s = (int64_t*)list_slot(l,i,NULL); if(s) *s=v; }
void ds_list_set_bool(DsList *l, int64_t i, int v) { int *s = (int*)list_slot(l,i,NULL); if(s) *s=v?1:0; }
void ds_list_set_string(DsList *l, int64_t i, DsString *v) {
    /* Owning slot: duplicate first so `list[i] = list[i]` stays safe. */
    void **s = (void**)list_slot(l,i,NULL);
    if (!s) { return; }
    DsString *fresh = ds_string_dup(v);
    if (*s) ds_release(*s);
    *s = fresh;
}
void ds_list_set_list(DsList *l, int64_t i, DsList *v) { void **s = (void**)list_slot(l,i,NULL); if(s) *s=v; }
void ds_list_set_object(DsList *l, int64_t i, void *v) { void **s = (void**)list_slot(l,i,NULL); if(s) *s=v; }

void ds_list_remove_at(DsList *l, int64_t idx, const char *where) {
    if (idx < 0) idx += l->count;
    if (idx < 0 || idx >= l->count) { ds_fail_list_index(where, idx, l->count); return; }
    char *base = (char*)l->items;
    char *slot = base + (size_t)idx * l->elem_size;
    /* manual: if dtor, call it for removed element */
    if (l->elem_dtor) {
        void *e = *(void**)slot;
        if (e) l->elem_dtor(e);
    }
    memmove(slot, slot + l->elem_size, (size_t)(l->count - idx -1) * l->elem_size);
    --l->count;
}
void ds_list_clear(DsList *l) {
    if (!l) return;
    if (l->elem_dtor) {
        for (int64_t i=0;i<l->count;++i) {
            void *e = *(void**)element_at(l,i);
            if (e) l->elem_dtor(e);
        }
    }
    l->count = 0;
}
int64_t ds_list_index_of_float(const DsList *l, double v) {
    const double *items = (const double*)l->items;
    for (int64_t i=0;i<l->count;++i) if (items[i]==v) return i;
    return -1;
}
int64_t ds_list_index_of_int(const DsList *l, int64_t v) {
    const int64_t *items = (const int64_t*)l->items;
    for (int64_t i=0;i<l->count;++i) if (items[i]==v) return i;
    return -1;
}
int64_t ds_list_index_of_object(const DsList *l, const void *v) {
    for (int64_t i=0;i<l->count;++i) if (*(void*const*)element_at(l,i)==v) return i;
    return -1;
}
int64_t ds_list_index_of_bool(const DsList *l, int v) {
    const int *items = (const int*)l->items;
    for (int64_t i=0;i<l->count;++i) if (items[i]==(v?1:0)) return i;
    return -1;
}
int64_t ds_list_index_of_string(const DsList *l, DsString *v) {
    for (int64_t i=0;i<l->count;++i) if (!ds_string_compare(*(DsString*const*)element_at(l,i), v)) return i;
    return -1;
}
static void *list_insert_slot(DsList *l, int64_t idx) {
    if (idx < 0) idx += l->count;
    if (idx < 0) idx = 0;
    if (idx > l->count) idx = l->count;
    list_grow(l, l->count+1);
    char *base = (char*)l->items;
    char *slot = base + (size_t)idx * l->elem_size;
    if (idx < l->count) memmove(slot + l->elem_size, slot, (size_t)(l->count - idx)*l->elem_size);
    ++l->count;
    return slot;
}
void ds_list_insert_float(DsList *l, int64_t i, double v) { *(double*)list_insert_slot(l,i)=v; }
void ds_list_insert_int(DsList *l, int64_t i, int64_t v) { *(int64_t*)list_insert_slot(l,i)=v; }
void ds_list_insert_bool(DsList *l, int64_t i, int v) { *(int*)list_insert_slot(l,i)=v?1:0; }
void ds_list_insert_string(DsList *l, int64_t i, DsString *v) { *(void**)list_insert_slot(l,i)=ds_string_dup(v); }
void ds_list_insert_list(DsList *l, int64_t i, DsList *v) { *(void**)list_insert_slot(l,i)=v; }
void ds_list_insert_object(DsList *l, int64_t i, void *v) { *(void**)list_insert_slot(l,i)=v; }

DsString *ds_list_join(const DsList *list, DsString *sep) {
    const char *sep_text = ds_cstr(sep);
    size_t sep_len = sep ? sep->length : 0;
    size_t total = 0;
    char scratch[64];
    for (int64_t i=0;i<list->count;++i) {
        if (i) total += sep_len;
        switch(list->kind) {
            case DS_ELEM_FLOAT: total += (size_t)snprintf(scratch,sizeof(scratch),"%.6g", ((double*)list->items)[i]); break;
            case DS_ELEM_INT: total += (size_t)snprintf(scratch,sizeof(scratch),"%lld",(long long)((int64_t*)list->items)[i]); break;
            case DS_ELEM_BOOL: total += ((int*)list->items)[i] ? 4u : 5u; break;
            case DS_ELEM_STRING: { const DsString *it = *(DsString*const*)element_at(list,i); total += it ? it->length : 0; break; }
            default: break;
        }
    }
    DsString *res = string_alloc(total);
    size_t used = 0;
    for (int64_t i=0;i<list->count;++i) {
        if (i && sep_len) { memcpy(res->bytes+used, sep_text, sep_len); used+=sep_len; }
        switch(list->kind) {
            case DS_ELEM_FLOAT: { int len=snprintf(scratch,sizeof(scratch),"%.6g", ((double*)list->items)[i]); if(len>0){ memcpy(res->bytes+used,scratch,(size_t)len); used+=(size_t)len; } break; }
            case DS_ELEM_INT: { int len=snprintf(scratch,sizeof(scratch),"%lld",(long long)((int64_t*)list->items)[i]); if(len>0){ memcpy(res->bytes+used,scratch,(size_t)len); used+=(size_t)len; } break; }
            case DS_ELEM_BOOL: { const char *t=((int*)list->items)[i]?"true":"false"; size_t l=strlen(t); memcpy(res->bytes+used,t,l); used+=l; break; }
            case DS_ELEM_STRING: { const DsString *it=*(DsString*const*)element_at(list,i); if(it&&it->length){ memcpy(res->bytes+used,it->bytes,it->length); used+=it->length; } break; }
            default: break;
        }
    }
    return res;
}

DsList *ds_list_of_floats(int64_t c, const double *v) {
    DsList *l=ds_list_new(DS_ELEM_FLOAT,0,NULL,c);
    for(int64_t i=0;i<c;++i) ds_list_push_float(l,v[i]);
    return l;
}
DsList *ds_list_of_ints(int64_t c, const int64_t *v) {
    DsList *l=ds_list_new(DS_ELEM_INT,0,NULL,c);
    for(int64_t i=0;i<c;++i) ds_list_push_int(l,v[i]);
    return l;
}
DsList *ds_list_of_bools(int64_t c, const int *v) {
    DsList *l=ds_list_new(DS_ELEM_BOOL,0,NULL,c);
    for(int64_t i=0;i<c;++i) ds_list_push_bool(l,v[i]);
    return l;
}
DsList *ds_list_of_strings(int64_t c, DsString *const *v) {
    DsList *l=ds_list_new(DS_ELEM_STRING,0,ds_string_dtor,c);
    for(int64_t i=0;i<c;++i) ds_list_push_string(l,v[i]);
    return l;
}
DsList *ds_list_of_lists(int64_t c, DsList *const *v) {
    DsList *l=ds_list_new(DS_ELEM_LIST,0,NULL,c);
    for(int64_t i=0;i<c;++i) ds_list_push_list(l,v[i]);
    return l;
}
DsList *ds_list_of_objects(int64_t c, void *const *v, size_t es, DsDtor dtor) {
    DsList *l=ds_list_new(DS_ELEM_OBJECT,es,dtor,c);
    for(int64_t i=0;i<c;++i) ds_list_push_object(l,v[i]);
    return l;
}

/* --- images --- */
int32_t ds_image_load(DsString *name) {
    const char *file = ds_cstr(name);
    if (!file[0]) return -1;
    int32_t h = ds_image_find(file);
    if (h >= 0) return h;
    size_t len=0;
    char *bytes = ds_files_read(file,&len);
    if (!bytes) { app_log_error("no image %s", file); return -1; }
    /* PNG only: JPEG and friends are named and refused here, never decoded
     * to garbage and never handed to the renderer. */
    if (!ds_png_magic_ok((const uint8_t*)bytes,len)) {
        app_log_error("%s: only PNG images are supported (%s data)", file,
                      ds_image_format_name((const uint8_t*)bytes,len));
        free(bytes);
        return -1;
    }
    int32_t w=0, ht=0;
    const char *err=NULL;
    uint8_t *rgba = ds_png_decode((const uint8_t*)bytes,len,&w,&ht,&err);
    free(bytes);
    if (!rgba) { app_log_error("%s: %s", file, err?err:"png error"); return -1; }
    h = ds_image_add(file, rgba, w, ht);
    if (h<0) { app_log_error("%s too many images", file); free(rgba); }
    return h;
}

/* --- fonts --- */
int32_t ds_font_load(DsString *name) {
    const char *file = ds_cstr(name);
    if (!file[0]) return -1;
    int32_t h = ds_font_find(file);
    if (h >= 0) return h;
    size_t len=0;
    char *bytes = ds_files_read(file,&len);
    if (!bytes) { app_log_error("no font %s", file); return -1; }
    if (!ds_font_magic_ok((const uint8_t*)bytes,len)) {
        app_log_error("%s: not a TrueType/OpenType font", file);
        free(bytes);
        return -1;
    }
    h = ds_font_add(file, (uint8_t*)bytes, len);
    if (h<0) { app_log_error("%s too many fonts", file); free(bytes); }
    /* A broken face keeps its registry entry: texts in it stay recorded but
     * undrawn, exactly like the default face (see ds_ttf.h). */
    else ds_ttf_load_face(h);
    return h;
}

/* --- render --- */
/* Straight alpha: nothing is premultiplied anywhere in the pipeline — the
 * blenders (Vulkan pipeline and software rasterizer alike) do src-over-dst,
 * so color_alpha(WHITE, 0.5) is a true half-strength sheet, not grey. */
static float current_color[4] = {1.0f,1.0f,1.0f,1.0f};
static int32_t current_font = -1;
static uint64_t text_calls;
static uint64_t image_calls;

void ds_render_color(float r,float g,float b){ current_color[0]=r; current_color[1]=g; current_color[2]=b; current_color[3]=1.0f; }
void ds_render_color_alpha(float r,float g,float b,float a){ current_color[0]=r; current_color[1]=g; current_color[2]=b; current_color[3]=a; }
const float *ds_render_current_color(void){ return current_color; }
void ds_render_clear(float r,float g,float b){
    EnjoerFrame *f=enjoer_frame();
    f->clear_color[0]=r; f->clear_color[1]=g; f->clear_color[2]=b; f->has_clear=1;
}
void ds_render_rect(float x,float y,float w,float h){ enjoer_draw_rect(x,y,w,h,current_color[0],current_color[1],current_color[2],current_color[3]); }
void ds_render_frame(float x,float y,float w,float h,float t){ enjoer_draw_frame_rect(x,y,w,h,t,current_color[0],current_color[1],current_color[2],current_color[3]); }
void ds_render_circle(float x,float y,float r){ enjoer_draw_circle(x,y,r,0,current_color[0],current_color[1],current_color[2],current_color[3]); }
void ds_render_ring(float x,float y,float r,float t){ enjoer_draw_ring(x,y,r,t,0,current_color[0],current_color[1],current_color[2],current_color[3]); }
void ds_render_line(float x0,float y0,float x1,float y1,float th){ enjoer_draw_line(x0,y0,x1,y1,th,current_color[0],current_color[1],current_color[2],current_color[3]); }
void ds_render_triangle(float x0,float y0,float x1,float y1,float x2,float y2){ enjoer_draw_triangle(x0,y0,x1,y1,x2,y2,current_color[0],current_color[1],current_color[2],current_color[3]); }
void ds_render_font(int32_t h){
    /* -1 selects the default face; a bogus positive handle is ignored rather
     * than recorded, so a typo cannot poison the rest of the frame. */
    if (h == -1 || ds_font_valid(h)) current_font = h;
}
int32_t ds_render_current_font(void){ return current_font; }
void ds_render_text(DsString *text,float x,float y,float scale){
    EnjoerFrame *f=enjoer_frame();
    if (f->text_count < ENJOER_DRAW_MAX_TEXT && text) {
        EnjoerTextCommand *c=&f->texts[f->text_count++];
        memset(c, 0, sizeof(*c));
        snprintf(c->text,sizeof(c->text),"%s", ds_cstr(text));
        c->x=x; c->y=y; c->scale=scale; c->r=current_color[0]; c->g=current_color[1]; c->b=current_color[2]; c->a=current_color[3];
        c->font=current_font;
        /* Resolve right away: glyph quads land in the batch exactly where the
         * script asked for them, so a screen-fade sheet or a warning banner
         * drawn after a label really sits on top of it (and the toast panel
         * covers its own text's background, never the other way round).  The
         * record stays for the debug mirrors (/info, frame_dump). */
        ds_ttf_draw_command(c);
        c->resolved=1;
        f->texts_resolved=1;
    }
    ++text_calls; ++f->text_total;
}
double ds_render_text_width(DsString *text,float scale){
    if (!text) return 0.0;
    return ds_ttf_measure(current_font, ds_cstr(text), scale);
}
void ds_render_image(int32_t h,float x,float y,float w,float ht){
    if (!ds_image_valid(h)) return;
    ds_render_image_region(h,x,y,w,ht,0,0,1,1);
}
void ds_render_image_rot(int32_t h,float x,float y,float w,float ht,float angle){
    float u0=0.0f, v0=0.0f, u1=1.0f, v1=1.0f;
    if (!ds_image_valid(h)) return;
    /* Same layer coordinates as an upright quad; only the corners move. */
    ds_image_layer_uv(h, &u0, &v0);
    ds_image_layer_uv(h, &u1, &v1);
    enjoer_draw_image_quad_rot(x,y,w,ht,u0,v0,u1,v1,h,current_color[0],current_color[1],current_color[2],current_color[3],angle);
    ++image_calls;
}
void ds_render_image_region(int32_t h,float x,float y,float w,float ht,float u0,float v0,float u1,float v1){
    if (!ds_image_valid(h)) return;
    /* A script measures texture coordinates against the image it loaded — the
     * whole sprite is (0,0)-(1,1) whatever the file's size.  The renderer
     * samples an array layer that is as large as the biggest image, so the
     * coordinates are restated in that space here: without this every sprite
     * drawn on the GPU came out as the whole layer, a small picture in the top
     * left corner of a quad that was mostly transparent margin. */
    ds_image_layer_uv(h, &u0, &v0);
    ds_image_layer_uv(h, &u1, &v1);
    enjoer_draw_image_quad(x,y,w,ht,u0,v0,u1,v1,h,current_color[0],current_color[1],current_color[2],current_color[3]);
    ++image_calls;
}
uint64_t ds_render_text_count(void){ return text_calls; }
uint64_t ds_render_image_count(void){ return image_calls; }

/* --- math --- */
double ds_math_floor(double v){ return floor(v); }
double ds_math_ceil(double v){ return ceil(v); }
double ds_math_round(double v){ return round(v); }
double ds_math_abs(double v){ return fabs(v); }
double ds_math_sign(double v){ return v<0.0?-1.0:v>0.0?1.0:0.0; }
double ds_math_sqrt(double v){ return v<0.0?0.0:sqrt(v); }
double ds_math_sin(double v){ return sin(v); }
double ds_math_cos(double v){ return cos(v); }
double ds_math_tan(double v){ return tan(v); }
double ds_math_pow(double b,double e){ return pow(b,e); }
double ds_math_min(double l,double r){ return l<r?l:r; }
double ds_math_max(double l,double r){ return l>r?l:r; }
double ds_math_lerp(double f,double t,double a){ return f+(t-f)*a; }
double ds_math_pi(void){ return 3.14159265358979323846; }
double ds_math_e(void){ return 2.71828182845904523536; }

/* --- engine + input --- */
static DsEngineState engine_state;
static int quit_requested;
static uint64_t random_state = 0x2545F4914F6CDD1Dull;

void ds_engine_quit(void){ quit_requested=1; }
int ds_engine_quit_requested(void){ return quit_requested; }
void ds_engine_reset(int w,int h){
    quit_requested=0;
    engine_state.width=w>0?w:1;
    engine_state.height=h>0?h:1;
    engine_state.time=0.0;
    engine_state.delta_time=0.0;
    engine_state.fps=0.0;
    engine_state.touch_count=0;
    engine_state.key_count=0;
    engine_state.frame=0;
    random_state=0x2545F4914F6CDD1Dull;
    for(int i=0;i<DS_MAX_TOUCHES;++i){ DsTouchState *t=&engine_state.touches[i]; t->used=0; t->down=0; t->id=-1; t->x=0; t->y=0; }
    for(int i=0;i<DS_MAX_KEYS;++i) engine_state.key_down[i]=0;
}
double ds_engine_width(void){ return (double)engine_state.width; }
double ds_engine_height(void){ return (double)engine_state.height; }
double ds_engine_time(void){ return engine_state.time; }
double ds_engine_delta(void){ return engine_state.delta_time; }
double ds_engine_fps(void){ return engine_state.fps; }
uint64_t ds_engine_frame(void){ return engine_state.frame; }
const DsEngineState *ds_engine_state(void){ return &engine_state; }
double ds_engine_random(void){
    random_state ^= random_state >> 12;
    random_state ^= random_state << 25;
    random_state ^= random_state >> 27;
    uint64_t v = random_state * 2685821657736338717ull;
    return (double)(v >> 11) / 9007199254740992.0;
}
void ds_engine_new_frame(double t,double dt,double fps){ engine_state.time=t; engine_state.delta_time=dt; engine_state.fps=fps; ++engine_state.frame; }
int ds_engine_touch_count(void){ return engine_state.touch_count; }
double ds_engine_touch_x(int i){ const DsTouchState *t=(i>=0&&i<DS_MAX_TOUCHES)?&engine_state.touches[i]:NULL; return t?(double)t->x:0.0; }
double ds_engine_touch_y(int i){ const DsTouchState *t=(i>=0&&i<DS_MAX_TOUCHES)?&engine_state.touches[i]:NULL; return t?(double)t->y:0.0; }
int ds_engine_touch_down(int i){ const DsTouchState *t=(i>=0&&i<DS_MAX_TOUCHES)?&engine_state.touches[i]:NULL; return t?t->down:0; }
void ds_engine_touch(int id,float x,float y,int down){
    int slot=-1;
    for(int i=0;i<DS_MAX_TOUCHES;++i) if(engine_state.touches[i].used && engine_state.touches[i].id==id){ slot=i; break; }
    if(slot<0 && down) for(int i=0;i<DS_MAX_TOUCHES;++i) if(!engine_state.touches[i].used){ slot=i; break; }
    if(slot<0) return;
    DsTouchState *t=&engine_state.touches[slot];
    t->used=1; t->id=id; t->x=x; t->y=y; t->down=down?1:0;
    engine_state.touch_count=0;
    for(int i=0;i<DS_MAX_TOUCHES;++i) if(engine_state.touches[i].used && engine_state.touches[i].down) ++engine_state.touch_count;
}
void ds_engine_key(const char *name,int down){
    if(!name||!name[0]) return;
    int slot=-1;
    for(int i=0;i<engine_state.key_count;++i) if(!strcmp(engine_state.key_names[i],name)) slot=i;
    if(slot<0){ if(engine_state.key_count>=DS_MAX_KEYS) return; slot=engine_state.key_count++; snprintf(engine_state.key_names[slot],sizeof(engine_state.key_names[slot]),"%s",name); }
    engine_state.key_down[slot]=down?1:0;
}
int ds_engine_key_down(DsString *name){
    const char *txt=ds_cstr(name);
    int d=0;
    for(int i=0;i<engine_state.key_count;++i) if(!strcmp(engine_state.key_names[i],txt)) d=engine_state.key_down[i];
    return d;
}
void ds_runtime_init(void){ text_calls=0; image_calls=0; current_color[0]=current_color[1]=current_color[2]=current_color[3]=1.0f; current_font=-1; ds_image_reset(); ds_font_reset(); ds_ttf_reset(); }
void ds_runtime_shutdown(void){ free_interned(); ds_image_reset(); ds_font_reset(); ds_ttf_reset(); }
