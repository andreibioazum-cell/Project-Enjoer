/* Runtime support shared by the DimScript interpreter and C emitted by the
 * ahead-of-time compiler.  Keep this file dependency free: it is compiled into
 * the Android .so as well as into the host preview and the regression tests. */
#define _POSIX_C_SOURCE 200809L
#include "dimscript_runtime.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DS_STRING_SLOTS 32

typedef struct {
    char *data;
    size_t capacity;
} StringSlot;

/* A list as seen by ahead-of-time compiled code.  Interpreted scripts use the
 * value-level lists inside the VM; both share this storage shape so the two
 * execution modes can be inspected by the same tools. */
struct DsNumberList {
    double *values;
    int count;
    int capacity;
};

static DimScriptRenderState render_state = {1.0f, 1.0f, 1.0f, 0, NULL};
static int quit_requested; /* set by engine.quit() in a script or generated C */
static _Thread_local StringSlot string_slots[DS_STRING_SLOTS];
static _Thread_local unsigned string_slot_index;
static DsEngineState engine_state;
static uint64_t random_state = 0x9e3779b97f4a7c15ull;
static DsNumberList *live_lists[64];
static int live_list_count;

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
    live_list_count = 0;
    ds_engine_reset(engine_state.width > 0 ? engine_state.width : 960,
                    engine_state.height > 0 ? engine_state.height : 540);
}

void ds_runtime_shutdown(void) {
    for (unsigned index = 0; index < DS_STRING_SLOTS; ++index) {
        free(string_slots[index].data);
        string_slots[index].data = NULL;
        string_slots[index].capacity = 0;
    }
    for (int index = 0; index < live_list_count; ++index) {
        free(live_lists[index]->values);
        free(live_lists[index]);
    }
    live_list_count = 0;
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

/* --- render --------------------------------------------------------------- */

static float *current_color(void) {
    return &render_state.red;
}

void ds_render_color(float red, float green, float blue) {
    render_state.red = red;
    render_state.green = green;
    render_state.blue = blue;
}

void ds_render_color_alpha(float red, float green, float blue, float alpha) {
    /* There is no blending in the pipeline yet, so alpha dims the colour
     * instead of compositing it.  A future text/quad pass can keep the same
     * script-level semantics and do it properly. */
    const float a = alpha < 0.0f ? 0.0f : alpha > 1.0f ? 1.0f : alpha;
    ds_render_color(red * a, green * a, blue * a);
}

const float *ds_render_current_color(void) {
    return current_color();
}

void ds_render_clear(float red, float green, float blue) {
    ds_render_color(red, green, blue);
    EnjoerFrame *frame = enjoer_frame();
    frame->clear_color[0] = red;
    frame->clear_color[1] = green;
    frame->clear_color[2] = blue;
    frame->has_clear = 1;
}

void ds_render_rect(float x, float y, float width, float height) {
    const float *color = current_color();
    enjoer_draw_rect(x, y, width, height, color[0], color[1], color[2]);
}

void ds_render_frame(float x, float y, float width, float height, float thickness) {
    const float *color = current_color();
    enjoer_draw_frame_rect(x, y, width, height, thickness, color[0], color[1], color[2]);
}

void ds_render_circle(float x, float y, float radius) {
    const float *color = current_color();
    enjoer_draw_circle(x, y, radius, 0, color[0], color[1], color[2]);
}

void ds_render_ring(float x, float y, float radius, float thickness) {
    const float *color = current_color();
    enjoer_draw_ring(x, y, radius, thickness, 0, color[0], color[1], color[2]);
}

void ds_render_line(float x0, float y0, float x1, float y1, float thickness) {
    const float *color = current_color();
    enjoer_draw_line(x0, y0, x1, y1, thickness, color[0], color[1], color[2]);
}

void ds_render_triangle(float x0, float y0, float x1, float y1, float x2, float y2) {
    const float *color = current_color();
    enjoer_draw_triangle(x0, y0, x1, y1, x2, y2, color[0], color[1], color[2]);
}

static void record_text(const char *text, float x, float y, float scale) {
    EnjoerFrame *frame = enjoer_frame();
    if (frame->text_count >= ENJOER_DRAW_MAX_TEXT) return;
    EnjoerTextCommand *command = &frame->texts[frame->text_count++];
    snprintf(command->text, sizeof(command->text), "%s", text ? text : "");
    command->x = x;
    command->y = y;
    command->scale = scale;
    command->r = render_state.red;
    command->g = render_state.green;
    command->b = render_state.blue;
}

void ds_render_text(const char *text, float x, float y, float scale) {
    ++render_state.text_calls;
    /* There is intentionally no font renderer yet.  The call is recorded so a
     * Vulkan text sink can be attached later without touching generated C, and
     * so the preview can overlay exactly what the script asked for. */
    record_text(text, x, y, scale);
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

/* --- strings -------------------------------------------------------------- */

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

/* --- lists --------------------------------------------------------------- */

DsNumberList *ds_number_list_new(void) {
    DsNumberList *list = (DsNumberList *)calloc(1, sizeof(DsNumberList));
    if (!list) return NULL;
    if (live_list_count < (int)(sizeof(live_lists) / sizeof(live_lists[0])))
        live_lists[live_list_count++] = list;
    return list;
}

static void ds_list_reserve(DsNumberList *list, int count) {
    if (list->capacity >= count) return;
    int capacity = list->capacity ? list->capacity : 8;
    while (capacity < count) capacity *= 2;
    double *grown = (double *)realloc(list->values, (size_t)capacity * sizeof(double));
    if (!grown) {
        fputs("DimScript: out of memory while growing a list\n", stderr);
        abort();
    }
    list->values = grown;
    list->capacity = capacity;
}

int ds_number_list_count(const DsNumberList *list) {
    return list ? list->count : 0;
}

void ds_number_list_push(DsNumberList *list, double value) {
    if (!list) return;
    ds_list_reserve(list, list->count + 1);
    list->values[list->count++] = value;
}

double ds_number_list_at(const DsNumberList *list, int index) {
    if (!list || index < 0 || index >= list->count) return 0.0;
    return list->values[index];
}

void ds_number_list_set(DsNumberList *list, int index, double value) {
    if (!list || index < 0 || index >= list->count) return;
    list->values[index] = value;
}

void ds_number_list_remove_at(DsNumberList *list, int index) {
    if (!list || index < 0 || index >= list->count) return;
    for (int cursor = index; cursor + 1 < list->count; ++cursor)
        list->values[cursor] = list->values[cursor + 1];
    --list->count;
}

void ds_number_list_clear(DsNumberList *list) {
    if (list) list->count = 0;
}

/* --- engine + input ------------------------------------------------------- */

void ds_engine_reset(int width, int height) {
    quit_requested = 0;
    engine_state.width = width > 0 ? width : 1;
    engine_state.height = height > 0 ? height : 1;
    engine_state.time = 0.0;
    engine_state.delta_time = 0.0;
    engine_state.fps = 0.0;
    engine_state.touch_count = 0;
    engine_state.key_count = 0;
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

void ds_engine_new_frame(double time, double delta_time, double fps) {
    engine_state.time = time;
    engine_state.delta_time = delta_time;
    engine_state.fps = fps;
    ++engine_state.frame;
}

/* Records the newest state of one pointer.  `down` is 1 while the finger is on
 * the screen; a released touch keeps its last coordinates so a script can read
 * where a gesture ended. */
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
            if (!engine_state.touches[index].used) { slot = index; break; }
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
        if (!strncmp(engine_state.key_names[index], name, sizeof(engine_state.key_names[index]))) slot = index;
    if (slot < 0) {
        if (engine_state.key_count >= DS_MAX_KEYS) return;
        slot = engine_state.key_count++;
        snprintf(engine_state.key_names[slot], sizeof(engine_state.key_names[slot]), "%s", name);
    }
    engine_state.key_down[slot] = down ? 1 : 0;
}

const DsEngineState *ds_engine_state(void) {
    return &engine_state;
}

float ds_engine_random(void) {
    /* xorshift64*: deterministic per process, no libm or syscall needed. */
    uint64_t value = random_state;
    value ^= value >> 12;
    value ^= value << 25;
    value ^= value >> 27;
    random_state = value;
    const uint64_t scaled = (value * 2685821657736338717ull) >> 40;
    return (float)((double)scaled / 16777216.0);
}

/* --- math -----------------------------------------------------------------
 * The VM implements the same formulas inline; generated C calls these.  Both
 * live here so a script written for the interpreter can be compiled without its
 * numbers changing: same rounding, same floored remainder, same square root. */
double ds_math_floor(double value) { return floor(value); }
double ds_math_ceil(double value) { return ceil(value); }
double ds_math_round(double value) { return floor(value + 0.5); }
double ds_math_abs(double value) { return value < 0.0 ? -value : value; }
double ds_math_sign(double value) { return value > 0.0 ? 1.0 : value < 0.0 ? -1.0 : 0.0; }
double ds_math_sqrt(double value) { return value < 0.0 ? 0.0 : sqrt(value); }
double ds_math_sin(double value) { return sin(value); }
double ds_math_cos(double value) { return cos(value); }
double ds_math_tan(double value) { return tan(value); }
double ds_math_pow(double base, double exponent) { return pow(base, exponent); }
double ds_math_min(double left, double right) { return left < right ? left : right; }
double ds_math_max(double left, double right) { return left > right ? left : right; }

double ds_math_mod(double left, double right) {
    if (right == 0.0) return 0.0; /* the VM raises; a compiled game clamps */
    const double result = fmod(left, right);
    return (result != 0.0 && ((result < 0.0) != (right < 0.0))) ? result + right : result;
}

double ds_math_lerp(double from, double to, double amount) {
    return from + (to - from) * amount;
}

double ds_math_pi(void) { return 3.14159265358979323846; }
double ds_math_e(void) { return 2.71828182845904523536; }

/* --- engine accessors for generated C ------------------------------------ */
void ds_engine_quit(void) { quit_requested = 1; }
int ds_engine_quit_requested(void) { return quit_requested; }
double ds_engine_width(void) { return (double)engine_state.width; }
double ds_engine_height(void) { return (double)engine_state.height; }
double ds_engine_time(void) { return engine_state.time; }
double ds_engine_delta(void) { return engine_state.delta_time; }
double ds_engine_fps(void) { return engine_state.fps; }
uint64_t ds_engine_frame(void) { return engine_state.frame; }
int ds_engine_touch_count(void) { return engine_state.touch_count; }

double ds_engine_touch_x(int index) {
    if (index < 0 || index >= engine_state.touch_count) return 0.0;
    return (double)engine_state.touches[index].x;
}

double ds_engine_touch_y(int index) {
    if (index < 0 || index >= engine_state.touch_count) return 0.0;
    return (double)engine_state.touches[index].y;
}

int ds_engine_touch_down(int index) {
    if (index < 0 || index >= engine_state.touch_count) return 0;
    return engine_state.touches[index].down;
}

int ds_engine_key_down(const char *name) {
    if (!name) return 0;
    for (int index = 0; index < engine_state.key_count; ++index)
        if (!strncmp(engine_state.key_names[index], name, sizeof(engine_state.key_names[index])))
            return engine_state.key_down[index];
    return 0;
}

/* --- helpers used by generated C ------------------------------------------
 * The interpreter owns its own value representation, so `print(a, b)`,
 * `len(text)`, `num(text)` and `str(value)` are tiny functions here rather than
 * compiler-generated code. */
const char *ds_text_join(int count, ...) {
    /* One space-separated line, like the interpreter's print.  Values already
     * went through ds_int_to_string/ds_float_to_string, so only text is joined
     * here; a NULL part prints as `nil`, matching `..` on nil. */
    char buffer[1024];
    size_t used = 0;
    buffer[0] = '\0';
    va_list args;
    va_start(args, count);
    for (int index = 0; index < count; ++index) {
        const char *part = va_arg(args, const char *);
        if (!part) part = "nil";
        if (index && used + 1 < sizeof(buffer)) buffer[used++] = ' ';
        const int written = snprintf(buffer + used, sizeof(buffer) - used, "%s", part);
        if (written < 0) break;
        used += (size_t)written;
        if (used >= sizeof(buffer) - 1) break;
    }
    va_end(args);
    buffer[used < sizeof(buffer) ? used : sizeof(buffer) - 1] = '\0';

    StringSlot *slot = next_string_slot();
    ensure_capacity(slot, used + 1);
    memcpy(slot->data, buffer, used + 1);
    return slot->data;
}

double ds_number_of_text(const char *text) {
    if (!text) return 0.0;
    char *end = NULL;
    const double value = strtod(text, &end);
    return end == text ? 0.0 : value;
}

int64_t ds_length_of(const char *text) {
    return (int64_t)(text ? strlen(text) : 0);
}
