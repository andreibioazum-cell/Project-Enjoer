/* Runtime support for C emitted by the DimScript compiler. */
#define _POSIX_C_SOURCE 200809L
#include "dimscript_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DS_STRING_SLOTS 32

typedef struct {
    char *data;
    size_t capacity;
} StringSlot;

static DimScriptRenderState render_state = {1.0f, 1.0f, 1.0f, 0, NULL};
static _Thread_local StringSlot string_slots[DS_STRING_SLOTS];
static _Thread_local unsigned string_slot_index;

static StringSlot *next_string_slot(void) {
    StringSlot *slot = &string_slots[string_slot_index++ % DS_STRING_SLOTS];
    if (!slot->data) {
        slot->capacity = 256;
        slot->data = (char *)malloc(slot->capacity);
        if (!slot->data) {
            fputs("DimScript: out of memory while formatting a string\n", stderr);
            abort();
        }
    }
    slot->data[0] = '\0';
    return slot;
}

static void ensure_capacity(StringSlot *slot, size_t required) {
    if (required <= slot->capacity) return;
    size_t capacity = slot->capacity;
    while (capacity < required) capacity *= 2;
    char *replacement = (char *)realloc(slot->data, capacity);
    if (!replacement) {
        fputs("DimScript: out of memory while growing a string\n", stderr);
        abort();
    }
    slot->data = replacement;
    slot->capacity = capacity;
}

void ds_runtime_init(void) {
    render_state.red = 1.0f;
    render_state.green = 1.0f;
    render_state.blue = 1.0f;
    render_state.text_calls = 0;
    render_state.text_sink = NULL;
    string_slot_index = 0;
}

void ds_runtime_shutdown(void) {
    for (unsigned index = 0; index < DS_STRING_SLOTS; ++index) {
        free(string_slots[index].data);
        string_slots[index].data = NULL;
        string_slots[index].capacity = 0;
    }
    render_state.text_sink = NULL;
}

void *ds_alloc(size_t size) {
    void *value = calloc(1, size ? size : 1);
    if (!value) {
        fputs("DimScript: out of memory\n", stderr);
        abort();
    }
    return value;
}

void ds_delete(void **value) {
    if (!value || !*value) return;
    free(*value);
    *value = NULL;
}

void ds_render_color(float red, float green, float blue) {
    render_state.red = red;
    render_state.green = green;
    render_state.blue = blue;
}

void ds_render_text(const char *text, float x, float y, float scale) {
    ++render_state.text_calls;
    /* There is intentionally no font renderer yet.  Keep the ABI and the
     * current colour so a Vulkan text sink can be attached without changing
     * generated C later. */
    if (render_state.text_sink)
        render_state.text_sink(text ? text : "", x, y, scale,
                               render_state.red, render_state.green, render_state.blue);
}

void ds_render_set_text_sink(DimScriptTextSink sink) {
    render_state.text_sink = sink;
}

const DimScriptRenderState *ds_render_state(void) {
    return &render_state;
}

uint64_t ds_render_text_count(void) {
    return render_state.text_calls;
}

const char *ds_int_to_string(int64_t value) {
    StringSlot *slot = next_string_slot();
    (void)snprintf(slot->data, slot->capacity, "%lld", (long long)value);
    return slot->data;
}

const char *ds_float_to_string(double value) {
    StringSlot *slot = next_string_slot();
    (void)snprintf(slot->data, slot->capacity, "%.9g", value);
    return slot->data;
}

const char *ds_bool_to_string(int value) {
    return value ? "true" : "false";
}

const char *ds_concat(const char *left, const char *right) {
    StringSlot *slot = next_string_slot();
    const char *safe_left = left ? left : "nil";
    const char *safe_right = right ? right : "nil";
    const size_t left_length = strlen(safe_left);
    const size_t right_length = strlen(safe_right);
    ensure_capacity(slot, left_length + right_length + 1);
    memcpy(slot->data, safe_left, left_length);
    memcpy(slot->data + left_length, safe_right, right_length + 1);
    return slot->data;
}

void ds_log(const char *text) {
    fprintf(stderr, "[DimScript] %s\n", text ? text : "nil");
}
