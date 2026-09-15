/*
 * Shared runtime ABI for DimScript, both the native interpreter (src/ds_vm.c)
 * and the C that the ahead-of-time compiler emits.
 *
 * The runtime deliberately knows nothing about the language surface: it owns
 * allocation, string scratch buffers, lists, input/engine state and the
 * font-less frame that the Vulkan renderer later consumes.  That is why a
 * script can be interpreted on a device and ahead-of-time compiled on a host
 * while both produce identical pixels.
 */
#ifndef DIMSCRIPT_RUNTIME_H
#define DIMSCRIPT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#include "enjoer_draw.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- lifecycle ------------------------------------------------------------ */
void ds_runtime_init(void);
void ds_runtime_shutdown(void);
void *ds_alloc(size_t size);
void ds_delete(void **value);

/* --- render API (font-less; text is recorded, not rasterized) ----------- */
typedef void (*DimScriptTextSink)(const char *text, float x, float y, float scale,
                                  float r, float g, float b);

typedef struct DimScriptRenderState {
    float red;
    float green;
    float blue;
    uint64_t text_calls;
    DimScriptTextSink text_sink;
} DimScriptRenderState;

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
void ds_render_text(const char *text, float x, float y, float scale);
void ds_render_set_text_sink(DimScriptTextSink sink);
const DimScriptRenderState *ds_render_state(void);
uint64_t ds_render_text_count(void);

/* --- strings -------------------------------------------------------------- */
const char *ds_int_to_string(int64_t value);
const char *ds_float_to_string(double value);
const char *ds_bool_to_string(int value);
const char *ds_concat(const char *left, const char *right);
void ds_log(const char *text);
/* Helpers the generated C uses for the small print/len/num surface. */
const char *ds_text_join(int count, ...);
double ds_number_of_text(const char *text);
int64_t ds_length_of(const char *text);

/* --- lists (ahead-of-time compiled scripts) -------------------------------
 * Compiled games cannot allocate GC cells on the fly the way the interpreter
 * does, so they get this fixed-capacity number array instead.  The VM has its
 * own garbage-collected `DsList`; the two types never meet in one translation
 * unit, which is why the runtime one is spelled `DsNumberList`. */
typedef struct DsNumberList DsNumberList;
DsNumberList *ds_number_list_new(void);
int ds_number_list_count(const DsNumberList *list);
void ds_number_list_push(DsNumberList *list, double value);
double ds_number_list_at(const DsNumberList *list, int index);
void ds_number_list_set(DsNumberList *list, int index, double value);
void ds_number_list_remove_at(DsNumberList *list, int index);
void ds_number_list_clear(DsNumberList *list);

/* --- math (shared by the VM and by generated C) --------------------------- */
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
double ds_math_mod(double left, double right);
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

/* engine.quit() from either implementation raises this flag; the host polls it
 * once per frame and tears the app down, so a script can end its own game. */
void ds_engine_quit(void);
int ds_engine_quit_requested(void);
void ds_engine_reset(int width, int height);
double ds_engine_width(void);
double ds_engine_height(void);
double ds_engine_time(void);
double ds_engine_delta(void);
double ds_engine_fps(void);
uint64_t ds_engine_frame(void);
int ds_engine_touch_count(void);
double ds_engine_touch_x(int index);
double ds_engine_touch_y(int index);
int ds_engine_touch_down(int index);
int ds_engine_key_down(const char *name);
void ds_engine_new_frame(double time, double delta_time, double fps);
void ds_engine_touch(int id, float x, float y, int down);
void ds_engine_key(const char *name, int down);
const DsEngineState *ds_engine_state(void);
float ds_engine_random(void);

#ifdef __cplusplus
}
#endif
#endif /* DIMSCRIPT_RUNTIME_H */
