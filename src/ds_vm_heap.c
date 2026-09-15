/* Memory, values and the frame-boundary collector for the DimScript VM.
 *
 * Two allocation domains exist on purpose:
 *   - the arena holds structural data (AST nodes, names, struct layouts) and is
 *     released only when the VM is destroyed;
 *   - the GC heap holds script values (strings, objects, lists) and is collected
 *     between frames, which is the only safe point.
 * That split is what keeps a clicker running for an hour on a phone without
 * growing forever while still avoiding a tracing GC inside an expression. */
#include "ds_vm_internal.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct DsArenaPage {
    struct DsArenaPage *next;
    size_t used;
    size_t capacity;
} DsArenaPage;

#define DS_HEAP_START (256u * 1024u)

static void *oom(size_t size) {
    (void)size;
    fputs("DimScript: out of memory\n", stderr);
    abort();
}

static size_t align8(size_t size) {
    return (size + 7u) & ~(size_t)7u;
}

void *ds_vm_arena(DsVM *vm, size_t size) {
    size = align8(size);
    DsArenaPage *page = (DsArenaPage *)vm->arena_pages;
    if (!page || page->used + size > page->capacity) {
        const size_t capacity = size + (16u * 1024u);
        DsArenaPage *fresh = (DsArenaPage *)malloc(sizeof(DsArenaPage) + capacity);
        if (!fresh) oom(capacity);
        fresh->next = page;
        fresh->used = 0;
        fresh->capacity = capacity;
        page = fresh;
        vm->arena_pages = fresh;
    }
    vm->arena_bytes += size;
    char *base = (char *)(page + 1);
    void *result = base + page->used;
    page->used += size;
    return result;
}

char *ds_vm_arena_string(DsVM *vm, const char *text, size_t length) {
    char *copy = (char *)ds_vm_arena(vm, length + 1);
    if (length && text) memcpy(copy, text, length);
    copy[length] = '\0';
    return copy;
}

/* --- errors --------------------------------------------------------------- */

void ds_vm_set_error(DsVM *vm, int line, int column, const char *format, ...) {
    if (!vm->error[0]) {
        va_list args;
        char detail[DS_MAX_ERROR / 2];
        va_start(args, format);
        vsnprintf(detail, sizeof(detail), format, args);
        va_end(args);
        if (line > 0) snprintf(vm->error, sizeof(vm->error), "%d:%d: %s", line, column, detail);
        else snprintf(vm->error, sizeof(vm->error), "%s", detail);
        vm->raised = 1;
    }
}

void ds_vm_raise(DsVM *vm, int line, int column, const char *format, ...) {
    va_list args;
    char detail[DS_MAX_ERROR];
    va_start(args, format);
    vsnprintf(detail, sizeof(detail), format, args);
    va_end(args);
    ds_vm_set_error(vm, line, column, "%s", detail);
}

/* --- allocation ----------------------------------------------------------- */

static void *gc_alloc(DsVM *vm, size_t size, DsKind kind) {
    DsGc *block = (DsGc *)malloc(size);
    if (!block) oom(size);
    block->next = vm->heap;
    block->size = (uint32_t)size;
    block->kind = (uint8_t)kind;
    block->mark = 0;
    block->persistent = 0;
    vm->heap = block;
    vm->heap_bytes += size;
    ++vm->heap_objects;
    return block;
}

DsValue ds_nil_value(void) {
    DsValue value;
    memset(&value, 0, sizeof(value));
    value.kind = DS_NIL;
    return value;
}

DsValue ds_bool_value(int boolean) {
    DsValue value = ds_nil_value();
    value.kind = DS_BOOL;
    value.as.boolean = boolean ? 1 : 0;
    return value;
}

DsValue ds_int_value(long long integer) {
    DsValue value = ds_nil_value();
    value.kind = DS_INT;
    value.as.integer = integer;
    return value;
}

DsValue ds_float_value(double number) {
    DsValue value = ds_nil_value();
    value.kind = DS_FLOAT;
    value.as.number = number;
    return value;
}

DsValue ds_string_value(DsString *string) {
    DsValue value = ds_nil_value();
    value.kind = DS_STRING;
    value.as.string = string;
    return value;
}

DsString *ds_new_string(DsVM *vm, const char *text, int length) {
    if (length < 0) length = text ? (int)strlen(text) : 0;
    DsString *string = (DsString *)gc_alloc(vm, sizeof(DsString) + (size_t)length, DS_STRING);
    string->length = length;
    if (length && text) memcpy(string->data, text, (size_t)length);
    string->data[length] = '\0';
    return string;
}

DsString *ds_copy_value_string(DsVM *vm, const DsValue *value) {
    if (value && value->kind == DS_STRING && value->as.string) return value->as.string;
    char buffer[256];
    const char *text = ds_value_text(vm, value, buffer, sizeof(buffer));
    return ds_new_string(vm, text, -1);
}

DsObject *ds_new_object(DsVM *vm, int struct_index) {
    const DsStruct *type = &vm->structs[struct_index];
    DsObject *object = (DsObject *)gc_alloc(vm, sizeof(DsObject), DS_OBJECT);
    object->type = struct_index;
    object->field_count = type->count;
    object->fields = type->count
                         ? (DsValue *)ds_vm_arena(vm, (size_t)type->count * sizeof(DsValue))
                         : NULL;
    /* Field storage is arena owned: an object's layout never changes, so the
     * values are as long lived as the AST while the object itself is swept. */
    for (int index = 0; index < type->count; ++index) {
        const DsField *field = &type->fields[index];
        object->fields[index] = ds_vm_default_for(vm, field->type, field->struct_index);
    }
    return object;
}

DsList *ds_new_list(DsVM *vm) {
    DsList *list = (DsList *)gc_alloc(vm, sizeof(DsList), DS_LIST);
    list->count = 0;
    list->capacity = 0;
    list->values = NULL;
    return list;
}

void ds_list_reserve(DsVM *vm, DsList *list, int count) {
    if (!list || list->capacity >= count) return;
    int capacity = list->capacity ? list->capacity : 4;
    while (capacity < count) capacity *= 2;
    DsValue *grown = (DsValue *)realloc(list->values, (size_t)capacity * sizeof(DsValue));
    if (!grown) oom((size_t)capacity * sizeof(DsValue));
    /* Zero the new tail so a stale value can never be traced. */
    for (int index = list->capacity; index < capacity; ++index) grown[index] = ds_nil_value();
    vm->heap_bytes += (size_t)(capacity - list->capacity) * sizeof(DsValue);
    list->values = grown;
    list->capacity = capacity;
}

/* --- collection ----------------------------------------------------------- */

void ds_mark_value(DsVM *vm, DsValue *value) {
    (void)vm;
    if (!value) return;
    if (value->kind == DS_STRING && value->as.string) {
        value->as.string->gc.mark = 1;
        return;
    }
    if (value->kind == DS_OBJECT && value->as.object) {
        DsObject *object = value->as.object;
        if (object->gc.mark) return;
        object->gc.mark = 1;
        for (int index = 0; index < object->field_count; ++index)
            ds_mark_value(vm, &object->fields[index]);
        return;
    }
    if (value->kind == DS_LIST && value->as.list) {
        DsList *list = value->as.list;
        if (list->gc.mark) return;
        list->gc.mark = 1;
        for (int index = 0; index < list->count; ++index)
            ds_mark_value(vm, &list->values[index]);
    }
}

static void sweep_block(DsGc *block) {
    if (block->kind == DS_LIST) {
        DsList *list = (DsList *)block;
        free(list->values);
    }
    free(block);
}

void ds_vm_collect(DsVM *vm) {
    if (!vm) return;
    DsGc *cursor = vm->heap;
    while (cursor) {
        cursor->mark = cursor->persistent ? 1 : 0;
        cursor = cursor->next;
    }
    for (int index = 0; index < vm->global_count; ++index)
        ds_mark_value(vm, &vm->globals[index].value);

    DsGc **link = &vm->heap;
    size_t live_bytes = 0;
    int live_objects = 0;
    while (*link) {
        DsGc *block = *link;
        if (block->mark) {
            live_bytes += block->size;
            ++live_objects;
            link = &block->next;
        } else {
            *link = block->next;
            sweep_block(block);
        }
    }
    vm->heap_objects = live_objects;
    vm->heap_bytes = live_bytes;
    vm->heap_next_gc = live_bytes * 2u > DS_HEAP_START ? live_bytes * 2u : DS_HEAP_START;
}

/* --- conversions ---------------------------------------------------------- */

const char *ds_kind_name(DsKind kind) {
    switch (kind) {
    case DS_BOOL: return "bool";
    case DS_INT: return "int";
    case DS_FLOAT: return "float";
    case DS_STRING: return "string";
    case DS_OBJECT: return "struct";
    case DS_LIST: return "list";
    default: return "nil";
    }
}

int ds_truthy(const DsValue *value) {
    if (!value) return 0;
    switch (value->kind) {
    case DS_BOOL: return value->as.boolean;
    case DS_INT: return value->as.integer != 0;
    case DS_FLOAT: return value->as.number != 0.0;
    case DS_STRING: return value->as.string && value->as.string->length > 0;
    case DS_OBJECT: return value->as.object != NULL;
    case DS_LIST: return value->as.list != NULL;
    default: return 0;
    }
}

int ds_to_number(const DsValue *value, double *out) {
    if (!value || !out) return 0;
    switch (value->kind) {
    case DS_INT:
        *out = (double)value->as.integer;
        return 1;
    case DS_FLOAT:
        *out = value->as.number;
        return 1;
    case DS_BOOL:
        *out = value->as.boolean ? 1.0 : 0.0;
        return 1;
    default:
        return 0;
    }
}

long long ds_to_integer(const DsValue *value) {
    double number = 0.0;
    if (value && value->kind == DS_INT) return value->as.integer;
    if (ds_to_number(value, &number)) return (long long)number;
    if (value && value->kind == DS_STRING && value->as.string) {
        return (long long)strtoll(value->as.string->data, NULL, 10);
    }
    return 0;
}

static void append(char *buffer, size_t capacity, size_t *used, const char *text, int length) {
    if (!buffer || *used >= capacity || !text) return;
    if (length < 0) length = (int)strlen(text);
    for (int index = 0; index < length && *used + 1 < capacity; ++index)
        buffer[(*used)++] = text[index];
    buffer[*used] = '\0';
}

static void format_float(char *out, size_t capacity, double value) {
    snprintf(out, capacity, "%.9g", value);
    /* %.9g prints 1 as "1"; scripts show whole floats the same way the
     * generated C does, so no ".0" suffix is added here. */
}

static void value_text_into(DsVM *vm, const DsValue *value, char *buffer, size_t capacity,
                            size_t *used, int depth) {
    char number[64];
    if (!value) {
        append(buffer, capacity, used, "nil", 3);
        return;
    }
    switch (value->kind) {
    case DS_BOOL:
        if (value->as.boolean) append(buffer, capacity, used, "true", 4);
        else append(buffer, capacity, used, "false", 5);
        break;
    case DS_INT:
        snprintf(number, sizeof(number), "%lld", value->as.integer);
        append(buffer, capacity, used, number, (int)strlen(number));
        break;
    case DS_FLOAT:
        if (value->as.number != value->as.number) append(buffer, capacity, used, "nan", 3);
        else if (value->as.number > 1.7976931348623157e307) append(buffer, capacity, used, "inf", 3);
        else if (value->as.number < -1.7976931348623157e307) append(buffer, capacity, used, "-inf", 4);
        else {
            format_float(number, sizeof(number), value->as.number);
            append(buffer, capacity, used, number, (int)strlen(number));
        }
        break;
    case DS_STRING:
        if (value->as.string)
            append(buffer, capacity, used, value->as.string->data, value->as.string->length);
        else
            append(buffer, capacity, used, "", 0);
        break;
    case DS_OBJECT:
        if (vm && value->as.object && value->as.object->type >= 0 &&
            value->as.object->type < vm->struct_count)
            append(buffer, capacity, used, vm->structs[value->as.object->type].name, -1);
        else
            append(buffer, capacity, used, "nil", 3);
        break;
    case DS_LIST: {
        if (depth > 2) {
            append(buffer, capacity, used, "[...]", 5);
            return;
        }
        append(buffer, capacity, used, "[", 1);
        const DsList *list = value->as.list;
        const int shown = list && list->count > 8 ? 8 : (list ? list->count : 0);
        for (int index = 0; index < shown; ++index) {
            if (index) append(buffer, capacity, used, ", ", 2);
            value_text_into(vm, &list->values[index], buffer, capacity, used, depth + 1);
        }
        if (list && list->count > shown) append(buffer, capacity, used, ", ...", 5);
        append(buffer, capacity, used, "]", 1);
        break;
    }
    default:
        append(buffer, capacity, used, "nil", 3);
        break;
    }
}

const char *ds_value_text(DsVM *vm, const DsValue *value, char *buffer, size_t capacity) {
    if (!buffer || capacity == 0) return "";
    size_t used = 0;
    buffer[0] = '\0';
    value_text_into(vm, value, buffer, capacity, &used, 0);
    return buffer;
}

int ds_values_equal(const DsValue *left, const DsValue *right) {
    if (!left || !right) return left == right;
    if (left->kind == DS_STRING || right->kind == DS_STRING) {
        char left_buffer[256], right_buffer[256];
        return strcmp(ds_value_text(NULL, left, left_buffer, sizeof(left_buffer)),
                      ds_value_text(NULL, right, right_buffer, sizeof(right_buffer))) == 0;
    }
    double a = 0.0, b = 0.0;
    if (ds_to_number(left, &a) && ds_to_number(right, &b)) return a == b;
    if (left->kind != right->kind) return 0;
    if (left->kind == DS_OBJECT) return left->as.object == right->as.object;
    if (left->kind == DS_LIST) return left->as.list == right->as.list;
    return 1; /* both nil */
}

/* A string that lives in the parse arena is never swept; the collector still
 * sees the persistent flag and leaves it alone. */
DsString *ds_literal_string(DsVM *vm, const char *text, int length) {
    DsString *string = (DsString *)ds_vm_arena(vm, sizeof(DsString) + (size_t)length);
    memset(string, 0, sizeof(DsGc));
    string->gc.kind = (uint8_t)DS_STRING;
    string->gc.mark = 1;
    string->gc.persistent = 1;
    string->gc.size = (uint32_t)(sizeof(DsString) + (size_t)length);
    string->length = length;
    if (length && text) memcpy(string->data, text, (size_t)length);
    string->data[length] = '\0';
    return string;
}
