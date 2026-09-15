/*
 * DimScript runtime ABI.
 *
 * A DimScript game is compiled ahead of time (tools/aot.py -> plain C99 ->
 * clang), so nothing in this header is interpreted: every script value is an
 * ordinary C value and every script function an ordinary C function.  The
 * runtime only owns the four things a game cannot express in C on its own:
 *
 *   1. a reference counted heap for strings, lists and struct objects;
 *   2. the triangle batch the renderer consumes;
 *   3. the engine and input state the host feeds every frame;
 *   4. the errors a script is allowed to survive (they unwind through
 *      app_fail/setjmp, exactly like a hardware fault would for a C loop).
 *
 * There is no garbage collector and no shadow stack.  The compiler emits
 * `ds_keep` (retain) and `ds_move`/`ds_release_slot` (move / drop) at the exact
 * points where ownership of a reference changes hands, so a frame performs no
 * scanning work at all.  When NDEBUG is not set the counters behind
 * ds_ref_stats() are maintained, which is how tools/tests proves that every
 * reference is released.
 */
#ifndef DIMSCRIPT_RUNTIME_H
#define DIMSCRIPT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "ds_image.h"
#include "enjoer_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- reference counted heap ---------------------------------------------- */

typedef void (*DsDtor)(void *value);

/* Allocates a zeroed payload with one reference owned by the caller.  `dtor`
 * runs just before the payload is freed and is NULL when the object holds no
 * references of its own. */
void *ds_alloc(size_t size, DsDtor dtor);

/* Returns the reference it was given (NULL stays NULL).  Strings from the
 * literal pool are immortal and ignore retain/release entirely. */
void *ds_keep(void *value);
void ds_release(void *value);

/* `slot = value` for reference types: retains the new value, releases the old
 * one, stores.  `ds_move` is the same without the retain — the caller transfers
 * the reference it owns (a `new` object, a concatenated string, a list). */
void ds_assign(void **slot, void *value);
void ds_move(void **slot, void *value);
/* `delete x`: releases the reference in the slot and clears it. */
void ds_release_slot(void **slot);

/* Nil check with a script location, used wherever the generated C dereferences
 * an object.  A nil dereference stops the frame with a readable message
 * instead of a segfault. */
void *ds_require(void *value, const char *where);

typedef struct DsRefStats {
    uint64_t allocations;
    uint64_t frees;
    uint64_t retains;
    uint64_t releases;
    uint64_t live;   /* objects alive right now */
    uint64_t peak;   /* high water mark */
} DsRefStats;

const DsRefStats *ds_ref_stats(void);
uint64_t ds_live_objects(void);

/* --- strings --------------------------------------------------------------
 * Strings are immutable.  A string literal in a script becomes an interned
 * immortal string created once by ds_intern_literals, so a literal costs
 * nothing per frame and never touches the heap counters.
 *
 * Ownership rule for the whole API: a function that takes a `DsString *`
 * consumes that reference, so generated code wraps a borrowed string in
 * ds_keep() and passes a freshly built string as-is. */

typedef struct DsString DsString;

typedef struct DsLiteralSource {
    const char *bytes;
    size_t length;
} DsLiteralSource;

/* Builds the immortal pool of script literals (deduplicated).  Called once by
 * dimscript_init. */
DsString **ds_intern_literals(const DsLiteralSource *sources, int count);

DsString *ds_string_new(const char *bytes, size_t length);
DsString *ds_int_to_string(int64_t value);
DsString *ds_float_to_string(double value);
DsString *ds_bool_to_string(int value);
/* Consumes both operands, returns the fresh result. */
DsString *ds_concat(DsString *left, DsString *right);
/* Consumes every argument, returns the fresh result. */
DsString *ds_text_join(int count, ...);
const char *ds_cstr(const DsString *value);
int64_t ds_string_length(const DsString *value);
int32_t ds_string_compare(const DsString *left, const DsString *right);
/* Fresh one-character string; the index may be negative. */
DsString *ds_string_char_at(const DsString *value, int64_t index, const char *where);
void ds_log(DsString *text);          /* consumes */
double ds_number_of_text(DsString *text); /* consumes */

/* --- lists ---------------------------------------------------------------
 * One list type, six element kinds, so the code generator never boxes a float
 * just to put it in a list.  Reading an object or a string out of a list
 * retains it (the result is an owned reference), writing consumes it. */

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
void ds_list_push_string(DsList *list, DsString *value);   /* consumes */
void ds_list_push_list(DsList *list, DsList *value);       /* consumes */
void ds_list_push_object(DsList *list, void *value);       /* consumes */

double ds_list_get_float(const DsList *list, int64_t index, const char *where);
int64_t ds_list_get_int(const DsList *list, int64_t index, const char *where);
int ds_list_get_bool(const DsList *list, int64_t index, const char *where);
DsString *ds_list_get_string(const DsList *list, int64_t index, const char *where); /* owned */
DsList *ds_list_get_list(const DsList *list, int64_t index, const char *where);     /* owned */
void *ds_list_get_object(const DsList *list, int64_t index, const char *where);     /* owned */

void ds_list_set_float(DsList *list, int64_t index, double value);
void ds_list_set_int(DsList *list, int64_t index, int64_t value);
void ds_list_set_bool(DsList *list, int64_t index, int value);
void ds_list_set_string(DsList *list, int64_t index, DsString *value);  /* consumes */
void ds_list_set_list(DsList *list, int64_t index, DsList *value);      /* consumes */
void ds_list_set_object(DsList *list, int64_t index, void *value);      /* consumes */
/* Inserts before `index` (negative counts from the end); the value is consumed. */
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
/* Consumes the probe string it compares against. */
int64_t ds_list_index_of_string(const DsList *list, DsString *value);
/* Consumes the separator, returns the fresh joined string. */
DsString *ds_list_join(const DsList *list, DsString *separator);
void ds_list_destroy(void *list);

/* Literal helper: `[1.0, 2.0]` compiles to one call with a compound literal. */
DsList *ds_list_of_floats(int64_t count, const double *values);
DsList *ds_list_of_ints(int64_t count, const int64_t *values);
DsList *ds_list_of_bools(int64_t count, const int *values);
DsList *ds_list_of_strings(int64_t count, DsString *const *values); /* consumes */
DsList *ds_list_of_lists(int64_t count, DsList *const *values);     /* consumes */
DsList *ds_list_of_objects(int64_t count, void *const *values, size_t elem_size, DsDtor elem_dtor);

/* --- error boundary ------------------------------------------------------ */

void ds_fail(const char *message);
/* `"main.ds:88: " + message` — the location is baked into the generated C. */
void ds_fail_where(const char *where, const char *message);
void ds_fail_list_index(const char *where, int64_t index, int64_t count);
double ds_div(double left, double right, const char *where);
double ds_mod(double left, double right, const char *where);
int64_t ds_imod(int64_t left, int64_t right, const char *where);

/* --- render API (text is recorded, never rasterized: no font backend) ----- */

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
void ds_render_text(DsString *text, float x, float y, float scale); /* consumes */
void ds_render_image(int32_t handle, float x, float y, float width, float height);
void ds_render_image_region(int32_t handle, float x, float y, float width, float height,
                            float u0, float v0, float u1, float v1);
uint64_t ds_render_text_count(void);
uint64_t ds_render_image_count(void);

/* --- math ---------------------------------------------------------------- */

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

/* --- engine + input state, fed by the host every frame ------------------- */

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
int ds_engine_key_down(DsString *name);   /* consumes the name */
void ds_engine_new_frame(double time, double delta_time, double fps);
void ds_engine_touch(int id, float x, float y, int down);
void ds_engine_key(const char *name, int down);
const DsEngineState *ds_engine_state(void);

/* --- lifecycle ----------------------------------------------------------- */

void ds_runtime_init(void);
void ds_runtime_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif /* DIMSCRIPT_RUNTIME_H */
