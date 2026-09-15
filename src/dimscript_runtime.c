/*
 * DimScript runtime: reference counted heap, strings, lists, images, the
 * triangle batch and the engine/input state.
 *
 * Reference counting model
 * ------------------------
 * Every heap object starts with a small header:
 *
 *     DsHeader { refs, kind, dtor }
 *
 * `refs == 0` marks an immortal object (an interned script literal): retain and
 * release are no-ops, so a string constant used in a draw call costs nothing.
 * `dtor` is the only thing that has to know the payload layout and is emitted
 * by the compiler for every struct that owns references, so releasing a struct
 * releases its list/string/struct fields and nothing else.  There is no
 * tracing, no mark phase, no write barrier and no shadow stack: the generated C
 * decides statically where a reference is created (ds_keep), moved (ds_move) or
 * dropped (ds_release_slot), which is why a frame allocates nothing and scans
 * nothing.
 */
#define _POSIX_C_SOURCE 200809L
#include "dimscript_runtime.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ds_files.h"
#include "ds_image.h"
#include "engine.h"
#include "enjoer_draw.h"

#ifdef DS_REF_COUNTERS
static DsRefStats stats;
#define DS_COUNT(field) (++stats.field)
#else
#define DS_COUNT(field) ((void)0)
#endif

typedef struct DsHeader {
    uint32_t refs; /* 0 = immortal (interned literal) */
    uint32_t kind;
    DsDtor dtor;
} DsHeader;

struct DsString {
    DsHeader header;
    uint32_t length;
    char bytes[];
};

#define DS_MAX_LIST_CAPACITY ((int64_t)1 << 24)

static void ds_free_header(DsHeader *header) {
    if (header->dtor) header->dtor(header + 1);
    free(header);
    DS_COUNT(frees);
}

void *ds_alloc(size_t size, DsDtor dtor) {
    DsHeader *header = (DsHeader *)calloc(1, sizeof(DsHeader) + size);
    if (!header) {
        ds_fail("DimScript: out of memory");
        return NULL;
    }
    header->refs = 1;
    header->dtor = dtor;
    DS_COUNT(allocations);
#ifdef DS_REF_COUNTERS
    if (stats.live + 1 > stats.peak) stats.peak = stats.live + 1;
    ++stats.live;
#endif
    return header + 1;
}

/* Every public entry point takes the payload pointer, which sits right after the
 * header: going back one header is the only correct way to reach it. */
static DsHeader *header_of(void *value) {
    return value ? (DsHeader *)value - 1 : NULL;
}

void *ds_keep(void *value) {
    DsHeader *header = header_of(value);
    if (!header || header->refs == 0) return value;
    ++header->refs;
    DS_COUNT(retains);
    return value;
}

void ds_release(void *value) {
    DsHeader *header = header_of(value);
    if (!header || header->refs == 0) return;
    DS_COUNT(releases);
    if (--header->refs == 0) {
#ifdef DS_REF_COUNTERS
        if (stats.live) --stats.live;
#endif
        ds_free_header(header);
    }
}

void ds_assign(void **slot, void *value) {
    void *previous = *slot;
    if (previous == value) return;
    ds_keep(value);
    *slot = value;
    ds_release(previous);
}

void ds_move(void **slot, void *value) {
    void *previous = *slot;
    if (previous == value) return;
    *slot = value;
    ds_release(previous);
}

void ds_release_slot(void **slot) {
    void *previous = *slot;
    *slot = NULL;
    ds_release(previous);
}

void *ds_require(void *value, const char *where) {
    if (!value) {
        char message[256];
        snprintf(message, sizeof(message), "%s: обращение к полю удалённого или пустого объекта",
                 where ? where : "DimScript");
        ds_fail(message);
    }
    return value;
}

const DsRefStats *ds_ref_stats(void) {
#ifdef DS_REF_COUNTERS
    return &stats;
#else
    static const DsRefStats empty;
    return &empty;
#endif
}

uint64_t ds_live_objects(void) {
#ifdef DS_REF_COUNTERS
    return stats.live;
#else
    return 0;
#endif
}

/* --- errors --------------------------------------------------------------- */

void ds_fail(const char *message) {
    app_fail("%s", message ? message : "DimScript error");
}

void ds_fail_where(const char *where, const char *message) {
    char buffer[256];
    snprintf(buffer, sizeof(buffer), "%s: %s", where ? where : "DimScript", message);
    ds_fail(buffer);
}

void ds_fail_list_index(const char *where, int64_t index, int64_t count) {
    app_fail("%s: индекс %lld вне списка из %lld элементов", where ? where : "DimScript",
             (long long)index, (long long)count);
}

double ds_div(double left, double right, const char *where) {
    if (right == 0.0) ds_fail_where(where, "деление на ноль");
    return left / right;
}

double ds_mod(double left, double right, const char *where) {
    if (right == 0.0) ds_fail_where(where, "остаток по нулю");
    double result = fmod(left, right);
    if (result != 0.0 && ((result < 0.0) != (right < 0.0))) result += right;
    return result;
}

int64_t ds_imod(int64_t left, int64_t right, const char *where) {
    if (right == 0) ds_fail_where(where, "остаток по нулю");
    int64_t result = left % right;
    if (result != 0 && ((result < 0) != (right < 0))) result += right;
    return result;
}

/* --- strings -------------------------------------------------------------- */

static DsString *string_alloc(size_t length) {
    DsString *string = (DsString *)ds_alloc(sizeof(DsString) + length + 1, NULL);
    string->length = (uint32_t)length;
    string->bytes[length] = '\0';
    return string;
}

DsString *ds_string_new(const char *bytes, size_t length) {
    DsString *string = string_alloc(length);
    if (length && bytes) memcpy(string->bytes, bytes, length);
    return string;
}

static DsString **interned;
static int interned_count;

DsString **ds_intern_literals(const DsLiteralSource *sources, int count) {
    if (interned) return interned;
    if (count <= 0) return NULL;
    interned = (DsString **)calloc((size_t)count, sizeof(DsString *));
    if (!interned) return NULL;
    for (int index = 0; index < count; ++index) {
        DsString *string = NULL;
        for (int previous = 0; previous < index; ++previous) {
            /* Identical literals share one object: a script that prints the same
             * label in twenty places owns one immortal string, not twenty. */
            if (interned[previous] && interned[previous]->length == sources[index].length &&
                !memcmp(interned[previous]->bytes, sources[index].bytes,
                        sources[index].length)) {
                string = interned[previous];
                break;
            }
        }
        if (!string) {
            string = string_alloc(sources[index].length);
            memcpy(string->bytes, sources[index].bytes, sources[index].length);
            string->header.refs = 0; /* immortal */
        }
        interned[index] = string;
    }
    interned_count = count;
    return interned;
}

static void free_interned(void) {
    for (int index = 0; index < interned_count; ++index) {
        DsString *string = interned[index];
        int duplicate = 0;
        for (int previous = 0; previous < index; ++previous)
            if (interned[previous] == string) duplicate = 1;
        if (!duplicate && string) {
            /* The payload sits right after its header, so the header is what
             * free() gets.  An immortal was never counted as alive when it was
             * released, but shutting the runtime down does have to discount it. */
            free((DsHeader *)string - 1);
            DS_COUNT(frees);
#ifdef DS_REF_COUNTERS
            if (stats.live) --stats.live;
#endif
        }
    }
    free(interned);
    interned = NULL;
    interned_count = 0;
}

const char *ds_cstr(const DsString *value) {
    return value ? value->bytes : "";
}

int64_t ds_string_length(const DsString *value) {
    return value ? (int64_t)value->length : 0;
}

int32_t ds_string_compare(const DsString *left, const DsString *right) {
    const char *a = ds_cstr(left);
    const char *b = ds_cstr(right);
    const int result = strcmp(a, b);
    return result < 0 ? -1 : result > 0 ? 1 : 0;
}

DsString *ds_string_char_at(const DsString *value, int64_t index, const char *where) {
    const int64_t length = value ? (int64_t)value->length : 0;
    if (index < 0) index += length;
    if (index < 0 || index >= length) {
        ds_fail_list_index(where, index, length);
        return NULL;
    }
    /* Script strings are UTF-8, so a "character" is one byte; that is what the
     * reference implementation did as well. */
    return ds_string_new(value->bytes + index, 1);
}

DsString *ds_int_to_string(int64_t value) {
    char buffer[32];
    const int length = snprintf(buffer, sizeof(buffer), "%lld", (long long)value);
    return ds_string_new(buffer, (size_t)(length > 0 ? length : 0));
}

DsString *ds_float_to_string(double value) {
    char buffer[48];
    int length = snprintf(buffer, sizeof(buffer), "%.6g", value);
    if (length <= 0) length = 0;
    /* Keep `2` readable as `2.0`: a script that concatenates a float should see
     * a float, not an integer that happens to be printed the same way. */
    if (!strchr(buffer, '.') && !strchr(buffer, 'e') && !strchr(buffer, 'n') &&
        !strchr(buffer, 'i') && length < (int)sizeof(buffer) - 2) {
        buffer[length++] = '.';
        buffer[length++] = '0';
        buffer[length] = '\0';
    }
    return ds_string_new(buffer, (size_t)length);
}

DsString *ds_bool_to_string(int value) {
    return value ? ds_string_new("true", 4) : ds_string_new("false", 5);
}

DsString *ds_concat(DsString *left, DsString *right) {
    const size_t left_length = left ? left->length : 0;
    const size_t right_length = right ? right->length : 0;
    DsString *result = string_alloc(left_length + right_length);
    if (left_length) memcpy(result->bytes, left->bytes, left_length);
    if (right_length) memcpy(result->bytes + left_length, right->bytes, right_length);
    ds_release(left);
    ds_release(right);
    return result;
}

DsString *ds_text_join(int count, ...) {
    va_list args;
    size_t total = 0;
    DsString **parts = count > 0 ? (DsString **)calloc((size_t)count, sizeof(DsString *)) : NULL;
    va_start(args, count);
    for (int index = 0; index < count; ++index) {
        parts[index] = va_arg(args, DsString *);
        total += parts[index] ? parts[index]->length : 0;
    }
    va_end(args);
    DsString *result = string_alloc(total);
    size_t used = 0;
    for (int index = 0; index < count; ++index) {
        if (parts[index] && parts[index]->length) {
            memcpy(result->bytes + used, parts[index]->bytes, parts[index]->length);
            used += parts[index]->length;
        }
        ds_release(parts[index]);
    }
    free(parts);
    return result;
}

void ds_log(DsString *text) {
    app_log("%s", ds_cstr(text));
    ds_release(text);
}

double ds_number_of_text(DsString *text) {
    const double value = text ? strtod(text->bytes, NULL) : 0.0;
    ds_release(text);
    return value;
}

/* --- lists ---------------------------------------------------------------- */

struct DsList {
    DsHeader header;
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
    switch (kind) {
    case DS_ELEM_FLOAT: return sizeof(double);
    case DS_ELEM_INT: return sizeof(int64_t);
    case DS_ELEM_BOOL: return sizeof(int);
    default: return sizeof(void *);
    }
}

/* Reference elements live at a pointer-aligned stride that is not their natural
 * size, so they are only ever reached through this helper.  Scalars keep
 * elem_size == sizeof(scalar) (see ds_list_new) and may additionally be indexed
 * through a typed pointer. */
static void *element_at(const DsList *list, int64_t index) {
    return (char *)list->items + (size_t)index * list->elem_size;
}

DsList *ds_list_new(int kind, size_t elem_size, DsDtor elem_dtor, int64_t capacity) {
    DsList *list = (DsList *)ds_alloc(sizeof(DsList), ds_list_destroy);
    list->kind = kind;
    list->elem_size = element_size_for(kind);
    if (element_is_reference(kind) && elem_size) {
        /* Elements are pointers, and the object itself lives on the heap, so the
         * stride only has to be pointer aligned — never the size of the struct,
         * which would misalign every second element. */
        const size_t pointer = sizeof(void *);
        list->elem_size = ((elem_size + pointer - 1) / pointer) * pointer;
    }
    list->elem_dtor = elem_dtor;
    if (capacity > 0) ds_list_reserve(list, capacity);
    return list;
}

void ds_list_destroy(void *value) {
    DsList *list = (DsList *)value;
    if (!list) return;
    if (element_is_reference(list->kind)) {
        for (int64_t index = 0; index < list->count; ++index) {
            /* One element owns exactly one reference; releasing it runs the
             * element destructor (for structs) through the normal path. */
            void *element = *(void **)element_at(list, index);
            if (element) ds_release(element);
        }
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

int64_t ds_list_count(const DsList *list) { return list ? list->count : 0; }

void ds_list_reserve(DsList *list, int64_t capacity) {
    if (!list || capacity <= list->capacity) return;
    if (capacity > DS_MAX_LIST_CAPACITY) ds_fail("DimScript: список слишком большой");
    int64_t grown = list->capacity ? list->capacity : 4;
    while (grown < capacity) grown *= 2;
    void *items = realloc(list->items, (size_t)grown * list->elem_size);
    if (!items) ds_fail("DimScript: out of memory while growing a list");
    list->items = items;
    list->capacity = grown;
}

static void list_grow(DsList *list, int64_t needed) {
    if (needed > list->capacity) ds_list_reserve(list, needed);
}

static void *list_slot(DsList *list, int64_t index, const char *where) {
    if (index < 0) index += list->count;
    if (index < 0 || index >= list->count) {
        ds_fail_list_index(where, index, list->count);
        return NULL;
    }
    return (char *)list->items + (size_t)index * list->elem_size;
}

static void *list_append_slot(DsList *list) {
    list_grow(list, list->count + 1);
    return (char *)list->items + (size_t)(list->count++) * list->elem_size;
}

static void *object_slot(const DsList *list, int64_t index, const char *where) {
    return list_slot((DsList *)list, index, where);
}

void ds_list_push_float(DsList *list, double value) {
    *(double *)list_append_slot(list) = value;
}

void ds_list_push_int(DsList *list, int64_t value) {
    *(int64_t *)list_append_slot(list) = value;
}

void ds_list_push_bool(DsList *list, int value) {
    *(int *)list_append_slot(list) = value ? 1 : 0;
}

void ds_list_push_string(DsList *list, DsString *value) {
    *(void **)list_append_slot(list) = value;
}

void ds_list_push_list(DsList *list, DsList *value) {
    *(void **)list_append_slot(list) = value;
}

void ds_list_push_object(DsList *list, void *value) {
    *(void **)list_append_slot(list) = value;
}

double ds_list_get_float(const DsList *list, int64_t index, const char *where) {
    return *(double *)object_slot(list, index, where);
}

int64_t ds_list_get_int(const DsList *list, int64_t index, const char *where) {
    return *(int64_t *)object_slot(list, index, where);
}

int ds_list_get_bool(const DsList *list, int64_t index, const char *where) {
    return *(int *)object_slot(list, index, where);
}

DsString *ds_list_get_string(const DsList *list, int64_t index, const char *where) {
    return (DsString *)ds_keep(*(void **)object_slot(list, index, where));
}

DsList *ds_list_get_list(const DsList *list, int64_t index, const char *where) {
    return (DsList *)ds_keep(*(void **)object_slot(list, index, where));
}

void *ds_list_get_object(const DsList *list, int64_t index, const char *where) {
    return ds_keep(*(void **)object_slot(list, index, where));
}

static void list_store_reference(DsList *list, int64_t index, void *value, const char *where) {
    void **slot = (void **)list_slot(list, index, where);
    if (!slot) return;
    void *previous = *slot;
    *slot = value;
    ds_release(previous);
}

void ds_list_set_float(DsList *list, int64_t index, double value) {
    double *slot = (double *)list_slot(list, index, NULL);
    if (slot) *slot = value;
}

void ds_list_set_int(DsList *list, int64_t index, int64_t value) {
    int64_t *slot = (int64_t *)list_slot(list, index, NULL);
    if (slot) *slot = value;
}

void ds_list_set_bool(DsList *list, int64_t index, int value) {
    int *slot = (int *)list_slot(list, index, NULL);
    if (slot) *slot = value ? 1 : 0;
}

void ds_list_set_string(DsList *list, int64_t index, DsString *value) {
    list_store_reference(list, index, value, NULL);
}

void ds_list_set_list(DsList *list, int64_t index, DsList *value) {
    list_store_reference(list, index, value, NULL);
}

void ds_list_set_object(DsList *list, int64_t index, void *value) {
    list_store_reference(list, index, value, NULL);
}

void ds_list_remove_at(DsList *list, int64_t index, const char *where) {
    if (index < 0) index += list->count;
    if (index < 0 || index >= list->count) {
        ds_fail_list_index(where, index, list->count);
        return;
    }
    char *base = (char *)list->items;
    char *slot = base + (size_t)index * list->elem_size;
    if (element_is_reference(list->kind)) ds_release(*(void **)slot);
    memmove(slot, slot + list->elem_size, (size_t)(list->count - index - 1) * list->elem_size);
    --list->count;
}

void ds_list_clear(DsList *list) {
    if (!list) return;
    if (element_is_reference(list->kind))
        for (int64_t index = 0; index < list->count; ++index)
            ds_release(*(void **)element_at(list, index));
    list->count = 0;
}

int64_t ds_list_index_of_float(const DsList *list, double value) {
    const double *items = (const double *)list->items;
    for (int64_t index = 0; index < list->count; ++index)
        if (items[index] == value) return index;
    return -1;
}

int64_t ds_list_index_of_int(const DsList *list, int64_t value) {
    const int64_t *items = (const int64_t *)list->items;
    for (int64_t index = 0; index < list->count; ++index)
        if (items[index] == value) return index;
    return -1;
}

int64_t ds_list_index_of_object(const DsList *list, const void *value) {
    for (int64_t index = 0; index < list->count; ++index)
        if (*(void *const *)element_at(list, index) == value) return index;
    return -1;
}

int64_t ds_list_index_of_bool(const DsList *list, int value) {
    const int *items = (const int *)list->items;
    for (int64_t index = 0; index < list->count; ++index)
        if (items[index] == (value ? 1 : 0)) return index;
    return -1;
}

int64_t ds_list_index_of_string(const DsList *list, DsString *value) {
    int64_t found = -1;
    for (int64_t index = 0; index < list->count; ++index)
        if (!ds_string_compare(*(DsString *const *)element_at(list, index), value)) {
            found = index;
            break;
        }
    ds_release(value);
    return found;
}

/* Shared body of the six insert wrappers: shift the tail, write the slot. */
static void *list_insert_slot(DsList *list, int64_t index) {
    if (index < 0) index += list->count;
    if (index < 0) index = 0;
    if (index > list->count) index = list->count;
    list_grow(list, list->count + 1);
    char *base = (char *)list->items;
    char *slot = base + (size_t)index * list->elem_size;
    if (index < list->count)
        memmove(slot + list->elem_size, slot, (size_t)(list->count - index) * list->elem_size);
    ++list->count;
    return slot;
}

void ds_list_insert_float(DsList *list, int64_t index, double value) {
    *(double *)list_insert_slot(list, index) = value;
}

void ds_list_insert_int(DsList *list, int64_t index, int64_t value) {
    *(int64_t *)list_insert_slot(list, index) = value;
}

void ds_list_insert_bool(DsList *list, int64_t index, int value) {
    *(int *)list_insert_slot(list, index) = value ? 1 : 0;
}

void ds_list_insert_string(DsList *list, int64_t index, DsString *value) {
    *(void **)list_insert_slot(list, index) = value;
}

void ds_list_insert_list(DsList *list, int64_t index, DsList *value) {
    *(void **)list_insert_slot(list, index) = value;
}

void ds_list_insert_object(DsList *list, int64_t index, void *value) {
    *(void **)list_insert_slot(list, index) = value;
}

DsString *ds_list_join(const DsList *list, DsString *separator) {
    const char *separator_text = ds_cstr(separator);
    const size_t separator_length = separator ? separator->length : 0;
    size_t total = 0;
    char scratch[64];
    for (int64_t index = 0; index < list->count; ++index) {
        if (index) total += separator_length;
        switch (list->kind) {
        case DS_ELEM_FLOAT: total += (size_t)snprintf(scratch, sizeof(scratch), "%.6g", ((double *)list->items)[index]); break;
        case DS_ELEM_INT: total += (size_t)snprintf(scratch, sizeof(scratch), "%lld", (long long)((int64_t *)list->items)[index]); break;
        case DS_ELEM_BOOL: total += ((int *)list->items)[index] ? 4u : 5u; break;
        case DS_ELEM_STRING: {
            const DsString *item = *(DsString *const *)element_at(list, index);
            total += item ? item->length : 0;
            break;
        }
        default: break;
        }
    }
    DsString *result = string_alloc(total);
    size_t used = 0;
    for (int64_t index = 0; index < list->count; ++index) {
        if (index && separator_length) {
            memcpy(result->bytes + used, separator_text, separator_length);
            used += separator_length;
        }
        switch (list->kind) {
        case DS_ELEM_FLOAT: {
            int length = snprintf(scratch, sizeof(scratch), "%.6g", ((double *)list->items)[index]);
            if (length > 0) { memcpy(result->bytes + used, scratch, (size_t)length); used += (size_t)length; }
            break;
        }
        case DS_ELEM_INT: {
            int length = snprintf(scratch, sizeof(scratch), "%lld", (long long)((int64_t *)list->items)[index]);
            if (length > 0) { memcpy(result->bytes + used, scratch, (size_t)length); used += (size_t)length; }
            break;
        }
        case DS_ELEM_BOOL: {
            const char *text = ((int *)list->items)[index] ? "true" : "false";
            const size_t length = strlen(text);
            memcpy(result->bytes + used, text, length);
            used += length;
            break;
        }
        case DS_ELEM_STRING: {
            const DsString *item = *(DsString *const *)element_at(list, index);
            if (item && item->length) {
                memcpy(result->bytes + used, item->bytes, item->length);
                used += item->length;
            }
            break;
        }
        default: break;
        }
    }
    ds_release(separator);
    return result;
}

DsList *ds_list_of_floats(int64_t count, const double *values) {
    DsList *list = ds_list_new(DS_ELEM_FLOAT, 0, NULL, count);
    for (int64_t index = 0; index < count; ++index) ds_list_push_float(list, values[index]);
    return list;
}

DsList *ds_list_of_ints(int64_t count, const int64_t *values) {
    DsList *list = ds_list_new(DS_ELEM_INT, 0, NULL, count);
    for (int64_t index = 0; index < count; ++index) ds_list_push_int(list, values[index]);
    return list;
}

DsList *ds_list_of_bools(int64_t count, const int *values) {
    DsList *list = ds_list_new(DS_ELEM_BOOL, 0, NULL, count);
    for (int64_t index = 0; index < count; ++index) ds_list_push_bool(list, values[index]);
    return list;
}

DsList *ds_list_of_strings(int64_t count, DsString *const *values) {
    DsList *list = ds_list_new(DS_ELEM_STRING, 0, NULL, count);
    for (int64_t index = 0; index < count; ++index) ds_list_push_string(list, values[index]);
    return list;
}

DsList *ds_list_of_lists(int64_t count, DsList *const *values) {
    DsList *list = ds_list_new(DS_ELEM_LIST, 0, NULL, count);
    for (int64_t index = 0; index < count; ++index) ds_list_push_list(list, values[index]);
    return list;
}

DsList *ds_list_of_objects(int64_t count, void *const *values, size_t elem_size, DsDtor elem_dtor) {
    DsList *list = ds_list_new(DS_ELEM_OBJECT, elem_size, elem_dtor, count);
    for (int64_t index = 0; index < count; ++index) ds_list_push_object(list, values[index]);
    return list;
}


/* --- images (script facing) ---------------------------------------------- */

int32_t ds_image_load(DsString *name) {
    const char *file = ds_cstr(name);
    if (!file[0]) {
        ds_release(name);
        return -1;
    }
    int32_t handle = ds_image_find(file);
    if (handle >= 0) {
        ds_release(name);
        return handle;
    }
    size_t length = 0;
    char *bytes = ds_files_read(file, &length);
    if (!bytes) {
        app_log_error("DimScript: нет файла изображения %s", file);
        ds_release(name);
        return -1;
    }
    int32_t width = 0, height = 0;
    const char *error = NULL;
    uint8_t *rgba = ds_png_decode((const uint8_t *)bytes, length, &width, &height, &error);
    free(bytes);
    if (!rgba) {
        app_log_error("DimScript: %s: %s", file, error ? error : "ошибка PNG");
        ds_release(name);
        return -1;
    }
    handle = ds_image_add(file, rgba, width, height);
    if (handle < 0) {
        app_log_error("DimScript: %s не помещается: предел %d изображений", file, DS_MAX_IMAGES);
        free(rgba);
    }
    ds_release(name);
    return handle;
}

/* --- render batch --------------------------------------------------------- */

static float current_color[3] = {1.0f, 1.0f, 1.0f};
static uint64_t text_calls;
static uint64_t image_calls;

void ds_render_color(float red, float green, float blue) {
    current_color[0] = red;
    current_color[1] = green;
    current_color[2] = blue;
}

void ds_render_color_alpha(float red, float green, float blue, float alpha) {
    /* The batch has no blending yet, so alpha scales towards black the same way
     * the reference behaviour did; kept deliberately simple. */
    current_color[0] = red * alpha;
    current_color[1] = green * alpha;
    current_color[2] = blue * alpha;
}

const float *ds_render_current_color(void) { return current_color; }

void ds_render_clear(float red, float green, float blue) {
    EnjoerFrame *frame = enjoer_frame();
    frame->clear_color[0] = red;
    frame->clear_color[1] = green;
    frame->clear_color[2] = blue;
    frame->has_clear = 1;
}

void ds_render_rect(float x, float y, float width, float height) {
    enjoer_draw_rect(x, y, width, height, current_color[0], current_color[1], current_color[2]);
}

void ds_render_frame(float x, float y, float width, float height, float thickness) {
    enjoer_draw_frame_rect(x, y, width, height, thickness, current_color[0], current_color[1],
                           current_color[2]);
}

void ds_render_circle(float x, float y, float radius) {
    enjoer_draw_circle(x, y, radius, 0, current_color[0], current_color[1], current_color[2]);
}

void ds_render_ring(float x, float y, float radius, float thickness) {
    enjoer_draw_ring(x, y, radius, thickness, 0, current_color[0], current_color[1], current_color[2]);
}

void ds_render_line(float x0, float y0, float x1, float y1, float thickness) {
    enjoer_draw_line(x0, y0, x1, y1, thickness, current_color[0], current_color[1], current_color[2]);
}

void ds_render_triangle(float x0, float y0, float x1, float y1, float x2, float y2) {
    enjoer_draw_triangle(x0, y0, x1, y1, x2, y2, current_color[0], current_color[1], current_color[2]);
}

void ds_render_text(DsString *text, float x, float y, float scale) {
    /* No font backend: the call is recorded with its colour and position, which
     * is exactly what a future glyph pass needs. */
    EnjoerFrame *frame = enjoer_frame();
    if (frame->text_count < ENJOER_DRAW_MAX_TEXT && text) {
        EnjoerTextCommand *command = &frame->texts[frame->text_count++];
        snprintf(command->text, sizeof(command->text), "%s", ds_cstr(text));
        command->x = x;
        command->y = y;
        command->scale = scale;
        command->r = current_color[0];
        command->g = current_color[1];
        command->b = current_color[2];
    }
    ++text_calls;
    ++frame->text_total;
    ds_release(text);
}

void ds_render_image(int32_t handle, float x, float y, float width, float height) {
    if (!ds_image_valid(handle)) return;
    ds_render_image_region(handle, x, y, width, height, 0.0f, 0.0f, 1.0f, 1.0f);
}

void ds_render_image_region(int32_t handle, float x, float y, float width, float height,
                            float u0, float v0, float u1, float v1) {
    if (!ds_image_valid(handle)) return;
    enjoer_draw_image_quad(x, y, width, height, u0, v0, u1, v1, handle, current_color[0],
                           current_color[1], current_color[2]);
    ++image_calls;
}

uint64_t ds_render_text_count(void) { return text_calls; }
uint64_t ds_render_image_count(void) { return image_calls; }

/* --- math ----------------------------------------------------------------- */

double ds_math_floor(double value) { return floor(value); }
double ds_math_ceil(double value) { return ceil(value); }
double ds_math_round(double value) { return round(value); }
double ds_math_abs(double value) { return fabs(value); }
double ds_math_sign(double value) { return value < 0.0 ? -1.0 : value > 0.0 ? 1.0 : 0.0; }
double ds_math_sqrt(double value) { return value < 0.0 ? 0.0 : sqrt(value); }
double ds_math_sin(double value) { return sin(value); }
double ds_math_cos(double value) { return cos(value); }
double ds_math_tan(double value) { return tan(value); }
double ds_math_pow(double base, double exponent) { return pow(base, exponent); }
double ds_math_min(double left, double right) { return left < right ? left : right; }
double ds_math_max(double left, double right) { return left > right ? left : right; }
double ds_math_lerp(double from, double to, double amount) { return from + (to - from) * amount; }
double ds_math_pi(void) { return 3.14159265358979323846; }
double ds_math_e(void) { return 2.71828182845904523536; }

/* --- engine + input ------------------------------------------------------- */

static DsEngineState engine_state;
static int quit_requested;
static uint64_t random_state = 0x2545F4914F6CDD1Dull;

void ds_engine_quit(void) { quit_requested = 1; }
int ds_engine_quit_requested(void) { return quit_requested; }

void ds_engine_reset(int width, int height) {
    quit_requested = 0;
    engine_state.width = width > 0 ? width : 1;
    engine_state.height = height > 0 ? height : 1;
    engine_state.time = 0.0;
    engine_state.delta_time = 0.0;
    engine_state.fps = 0.0;
    engine_state.touch_count = 0;
    engine_state.key_count = 0;
    engine_state.frame = 0;
    random_state = 0x2545F4914F6CDD1Dull;
    for (int index = 0; index < DS_MAX_TOUCHES; ++index) {
        DsTouchState *touch = &engine_state.touches[index];
        touch->used = 0;
        touch->down = 0;
        touch->id = -1;
        touch->x = 0.0f;
        touch->y = 0.0f;
    }
    for (int index = 0; index < DS_MAX_KEYS; ++index) engine_state.key_down[index] = 0;
}

double ds_engine_width(void) { return (double)engine_state.width; }
double ds_engine_height(void) { return (double)engine_state.height; }
double ds_engine_time(void) { return engine_state.time; }
double ds_engine_delta(void) { return engine_state.delta_time; }
double ds_engine_fps(void) { return engine_state.fps; }
uint64_t ds_engine_frame(void) { return engine_state.frame; }
const DsEngineState *ds_engine_state(void) { return &engine_state; }

double ds_engine_random(void) {
    /* xorshift64*: deterministic, so a replay of the same frames in a test
     * produces the same pixels. */
    random_state ^= random_state >> 12;
    random_state ^= random_state << 25;
    random_state ^= random_state >> 27;
    const uint64_t value = random_state * 2685821657736338717ull;
    return (double)(value >> 11) / 9007199254740992.0;
}

void ds_engine_new_frame(double time, double delta_time, double fps) {
    engine_state.time = time;
    engine_state.delta_time = delta_time;
    engine_state.fps = fps;
    ++engine_state.frame;
}

int ds_engine_touch_count(void) { return engine_state.touch_count; }

double ds_engine_touch_x(int index) {
    const DsTouchState *touch = (index >= 0 && index < DS_MAX_TOUCHES) ? &engine_state.touches[index] : NULL;
    return touch ? (double)touch->x : 0.0;
}

double ds_engine_touch_y(int index) {
    const DsTouchState *touch = (index >= 0 && index < DS_MAX_TOUCHES) ? &engine_state.touches[index] : NULL;
    return touch ? (double)touch->y : 0.0;
}

int ds_engine_touch_down(int index) {
    const DsTouchState *touch = (index >= 0 && index < DS_MAX_TOUCHES) ? &engine_state.touches[index] : NULL;
    return touch ? touch->down : 0;
}

void ds_engine_touch(int id, float x, float y, int down) {
    int slot = -1;
    for (int index = 0; index < DS_MAX_TOUCHES; ++index) {
        if (engine_state.touches[index].used && engine_state.touches[index].id == id) {
            slot = index;
            break;
        }
    }
    if (slot < 0 && down) {
        for (int index = 0; index < DS_MAX_TOUCHES; ++index) {
            if (!engine_state.touches[index].used) {
                slot = index;
                break;
            }
        }
    }
    if (slot < 0) return;
    DsTouchState *touch = &engine_state.touches[slot];
    touch->used = 1;
    touch->id = id;
    touch->x = x;
    touch->y = y;
    touch->down = down ? 1 : 0;
    engine_state.touch_count = 0;
    for (int index = 0; index < DS_MAX_TOUCHES; ++index)
        if (engine_state.touches[index].used && engine_state.touches[index].down)
            ++engine_state.touch_count;
}

void ds_engine_key(const char *name, int down) {
    if (!name || !name[0]) return;
    int slot = -1;
    for (int index = 0; index < engine_state.key_count; ++index)
        if (!strcmp(engine_state.key_names[index], name)) slot = index;
    if (slot < 0) {
        if (engine_state.key_count >= DS_MAX_KEYS) return;
        slot = engine_state.key_count++;
        snprintf(engine_state.key_names[slot], sizeof(engine_state.key_names[slot]), "%s", name);
    }
    engine_state.key_down[slot] = down ? 1 : 0;
}

int ds_engine_key_down(DsString *name) {
    const char *text = ds_cstr(name);
    int down = 0;
    for (int index = 0; index < engine_state.key_count; ++index)
        if (!strcmp(engine_state.key_names[index], text)) down = engine_state.key_down[index];
    ds_release(name);
    return down;
}

/* --- lifecycle ------------------------------------------------------------ */

void ds_runtime_init(void) {
    text_calls = 0;
    image_calls = 0;
    current_color[0] = current_color[1] = current_color[2] = 1.0f;
    ds_image_reset();
}

void ds_runtime_shutdown(void) {
    free_interned();
    ds_image_reset();
}
