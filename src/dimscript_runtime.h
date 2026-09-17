/*
 * DimScript runtime ABI — STRICT COMPILER MODE.
 *
 * No reference counting, no GC, no shadow stack.
 * Every value is a raw C pointer / float / int.
 * AOT compiler (tools/aot.py) emits plain C99 that compiles to machine code
 * via clang (NDK) at -O3 — speed like C.
 *
 * Memory model — manual, like in C:
 *   - ds_alloc() = malloc + zero + store dtor in header
 *   - ds_release() = call dtor (if any) + free
 *   - ds_keep() is identity, no cost
 *   - ds_assign / ds_move = raw pointer store, no retain/release
 *   - ds_release_slot = free + NULL
 *   - strings / lists are borrowed by default, never auto-freed unless
 *     explicitly deleted via `delete x` in script
 *   - literals from ds_intern_literals are immortal (never freed)
 *
 * This gives zero overhead in hot path: draw() does no refcounting at all.
 */

#ifndef DIMSCRIPT_RUNTIME_H
#define DIMSCRIPT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "ds_font.h"
#include "ds_image.h"
#include "enjoer_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- manual heap ------------------------------------------------------- */

typedef void (*DsDtor)(void *value);

/* Allocates zeroed payload, stores dtor in hidden header. Caller owns it. */
void *ds_alloc(size_t size, DsDtor dtor);
void ds_free(void *value); /* raw free without dtor check, for internal use */

/* Manual management: no refcounting */
#define ds_keep(v) (v)
static inline void ds_release(void *value);
void ds_assign(void **slot, void *value); /* *slot = value */
void ds_move(void **slot, void *value);   /* *slot = value */
void ds_release_slot(void **slot);        /* free + NULL */

/* Nil check with location, aborts frame on nil deref */
void *ds_require(void *value, const char *where);

/* --- strings ----------------------------------------------------------- */

typedef struct DsString DsString;

typedef struct DsLiteralSource {
    const char *bytes;
    size_t length;
} DsLiteralSource;

DsString **ds_intern_literals(const DsLiteralSource *sources, int count);

DsString *ds_string_new(const char *bytes, size_t length);
DsString *ds_int_to_string(int64_t value);
DsString *ds_float_to_string(double value);
DsString *ds_bool_to_string(int value);
DsString *ds_concat(DsString *left, DsString *right); /* borrows both, returns owned */
DsString *ds_text_join(int count, ...); /* borrows all, returns owned */
/* Ownership: strings are immutable values, so storing one always copies.
 * ds_string_dup(NULL) is NULL; ds_string_replace frees the old slot value
 * after duplicating (self-assignment safe); ds_string_dtor is the list
 * element destructor for string lists, which own private duplicates. */
DsString *ds_string_dup(const DsString *value);
void ds_string_replace(DsString **slot, DsString *value); /* borrows value */
void ds_string_dtor(void *value);
const char *ds_cstr(const DsString *value);
int64_t ds_string_length(const DsString *value);
int32_t ds_string_compare(const DsString *left, const DsString *right);
DsString *ds_string_char_at(const DsString *value, int64_t index, const char *where);
void ds_log(DsString *text); /* borrows */
double ds_number_of_text(DsString *text); /* borrows */

/* --- lists -------------------------------------------------------------
 * One list type, six element kinds — no boxing for floats.
 * All getters return BORROWED pointers (no keep). Set/push borrows value.
 */

typedef enum DsElemKind {
    DS_ELEM_FLOAT = 0,
    DS_ELEM_INT,
    DS_ELEM_BOOL,
    DS_ELEM_STRING,
    DS_ELEM_LIST,
    DS_ELEM_OBJECT
} DsElemKind;

typedef struct DsList DsList;

DsList *ds_list_new(int kind, size_t elem_size, DsDtor elem_dtor, int64_t capacity);
int64_t ds_list_count(const DsList *list);
void ds_list_reserve(DsList *list, int64_t capacity);

void ds_list_push_float(DsList *list, double value);
void ds_list_push_int(DsList *list, int64_t value);
void ds_list_push_bool(DsList *list, int value);
void ds_list_push_string(DsList *list, DsString *value);
void ds_list_push_list(DsList *list, DsList *value);
void ds_list_push_object(DsList *list, void *value);

double ds_list_get_float(const DsList *list, int64_t index, const char *where);
int64_t ds_list_get_int(const DsList *list, int64_t index, const char *where);
int ds_list_get_bool(const DsList *list, int64_t index, const char *where);
DsString *ds_list_get_string(const DsList *list, int64_t index, const char *where); /* borrowed */
DsList *ds_list_get_list(const DsList *list, int64_t index, const char *where);     /* borrowed */
void *ds_list_get_object(const DsList *list, int64_t index, const char *where);     /* borrowed */

void ds_list_set_float(DsList *list, int64_t index, double value);
void ds_list_set_int(DsList *list, int64_t index, int64_t value);
void ds_list_set_bool(DsList *list, int64_t index, int value);
void ds_list_set_string(DsList *list, int64_t index, DsString *value);
void ds_list_set_list(DsList *list, int64_t index, DsList *value);
void ds_list_set_object(DsList *list, int64_t index, void *value);

void ds_list_insert_float(DsList *list, int64_t index, double value);
void ds_list_insert_int(DsList *list, int64_t index, int64_t value);
void ds_list_insert_bool(DsList *list, int64_t index, int value);
void ds_list_insert_string(DsList *list, int64_t index, DsString *value);
void ds_list_insert_list(DsList *list, int64_t index, DsList *value);
void ds_list_insert_object(DsList *list, int64_t index, void *value);
void ds_list_remove_at(DsList *list, int64_t index, const char *where);
void ds_list_clear(DsList *list);
int64_t ds_list_index_of_float(const DsList *list, double value);
int64_t ds_list_index_of_int(const DsList *list, int64_t value);
int64_t ds_list_index_of_object(const DsList *list, const void *value);
int64_t ds_list_index_of_bool(const DsList *list, int value);
int64_t ds_list_index_of_string(const DsList *list, DsString *value); /* borrows */
DsString *ds_list_join(const DsList *list, DsString *separator); /* borrows sep */
void ds_list_destroy(void *list);

DsList *ds_list_of_floats(int64_t count, const double *values);
DsList *ds_list_of_ints(int64_t count, const int64_t *values);
DsList *ds_list_of_bools(int64_t count, const int *values);
DsList *ds_list_of_strings(int64_t count, DsString *const *values);
DsList *ds_list_of_lists(int64_t count, DsList *const *values);
DsList *ds_list_of_objects(int64_t count, void *const *values, size_t elem_size, DsDtor elem_dtor);

/* --- error boundary ---------------------------------------------------- */

void ds_fail(const char *message);
void ds_fail_where(const char *where, const char *message);
void ds_fail_list_index(const char *where, int64_t index, int64_t count);
double ds_div(double left, double right, const char *where);
double ds_mod(double left, double right, const char *where);
int64_t ds_imod(int64_t left, int64_t right, const char *where);

/* --- render API -------------------------------------------------------- */

void ds_render_color(float red, float green, float blue);
void ds_render_color_alpha(float red, float green, float blue, float alpha);
const float *ds_render_current_color(void);
void ds_render_clear(float red, float green, float blue);
void ds_render_rect(float x, float y, float width, float height);
void ds_render_frame(float x, float y, float width, float height, float thickness);
void ds_render_circle(float x, float y, float radius);
void ds_render_ring(float x, float y, float radius, float thickness);
void ds_render_line(float x0, float y0, float x1, float y1, float thickness);
void ds_render_triangle(float x0, float y0, float x1, float y1, float x2, float y2);
void ds_render_text(DsString *text, float x, float y, float scale); /* borrows */
void ds_render_font(int32_t handle);
int32_t ds_render_current_font(void);
void ds_render_image(int32_t handle, float x, float y, float width, float height);
void ds_render_image_rot(int32_t handle, float x, float y, float width, float height,
                         float angle);
void ds_render_image_region(int32_t handle, float x, float y, float width, float height,
                            float u0, float v0, float u1, float v1);
/* Pixel width of `text` drawn at `scale` with the current font, measured with
 * the same TrueType advances the text pass lays glyphs out with — the number a
 * script needs to centre a label exactly (multi-line: the longest line). */
double ds_render_text_width(DsString *text, float scale); /* borrows */
uint64_t ds_render_text_count(void);
uint64_t ds_render_image_count(void);

/* --- assets ------------------------------------------------------------ */

/* `image.load("sprites.png")`: decode once, return the registry handle (-1 on
 * a missing or broken file).  The compiler emits this call; generated games
 * must see the declaration. */
int32_t ds_image_load(DsString *name); /* borrows */
/* `font.load("font.ttf")`: read once, validate the header, return the handle
 * (-1 on a missing or non-font file). */
int32_t ds_font_load(DsString *name); /* borrows */

/* --- math -------------------------------------------------------------- */

double ds_math_floor(double value);
double ds_math_ceil(double value);
double ds_math_round(double value);
double ds_math_abs(double value);
double ds_math_sign(double value);
double ds_math_sqrt(double value);
double ds_math_sin(double value);
double ds_math_cos(double value);
double ds_math_tan(double value);
double ds_math_pow(double base, double exponent);
double ds_math_min(double left, double right);
double ds_math_max(double left, double right);
double ds_math_lerp(double from, double to, double amount);
double ds_math_pi(void);
double ds_math_e(void);

/* --- engine + input ---------------------------------------------------- */

#define DS_MAX_TOUCHES 8
#define DS_MAX_KEYS 24

typedef struct DsTouchState {
    int id;
    float x;
    float y;
    int down;
    int used;
} DsTouchState;

typedef struct DsEngineState {
    int width;
    int height;
    double time;
    double delta_time;
    double fps;
    int touch_count;
    DsTouchState touches[DS_MAX_TOUCHES];
    int key_count;
    char key_names[DS_MAX_KEYS][16];
    int key_down[DS_MAX_KEYS];
    uint64_t frame;
} DsEngineState;

void ds_engine_quit(void);
int ds_engine_quit_requested(void);
void ds_engine_reset(int width, int height);
double ds_engine_width(void);
double ds_engine_height(void);
double ds_engine_time(void);
double ds_engine_delta(void);
double ds_engine_fps(void);
uint64_t ds_engine_frame(void);
double ds_engine_random(void);
int ds_engine_touch_count(void);
double ds_engine_touch_x(int index);
double ds_engine_touch_y(int index);
int ds_engine_touch_down(int index);
int ds_engine_key_down(DsString *name); /* borrows */
void ds_engine_new_frame(double time, double delta_time, double fps);
void ds_engine_touch(int id, float x, float y, int down);
void ds_engine_key(const char *name, int down);
const DsEngineState *ds_engine_state(void);

/* --- lifecycle --------------------------------------------------------- */

void ds_runtime_init(void);
void ds_runtime_shutdown(void);

/* Inline implementations for speed — like in C */
static inline void ds_release(void *value) {
    if (!value) return;
    /* header is right before payload */
    typedef struct { DsDtor dtor; } H;
    H *h = ((H*)value) - 1;
    if (h->dtor) h->dtor(value);
    free(h);
}

#ifdef __cplusplus
}
#endif
#endif /* DIMSCRIPT_RUNTIME_H */
