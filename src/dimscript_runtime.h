/*
 * Small C ABI used by generated DimScript programs.
 *
 * DimScript deliberately keeps the runtime independent from Vulkan.  The game
 * loop calls the generated callbacks, while the renderer can later attach a
 * text sink to this ABI.  Until a font atlas exists, ds_render_text records
 * the call and intentionally draws nothing.
 */
#ifndef DIMSCRIPT_RUNTIME_H
#define DIMSCRIPT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*DimScriptTextSink)(const char *text, float x, float y, float scale,
                                  float r, float g, float b);

typedef struct DimScriptRenderState {
    float red;
    float green;
    float blue;
    uint64_t text_calls;
    DimScriptTextSink text_sink;
} DimScriptRenderState;

void ds_runtime_init(void);
void ds_runtime_shutdown(void);
void *ds_alloc(size_t size);
void ds_delete(void **value);

void ds_render_color(float red, float green, float blue);
void ds_render_text(const char *text, float x, float y, float scale);
void ds_render_set_text_sink(DimScriptTextSink sink);
const DimScriptRenderState *ds_render_state(void);
uint64_t ds_render_text_count(void);

const char *ds_int_to_string(int64_t value);
const char *ds_float_to_string(double value);
const char *ds_bool_to_string(int value);
const char *ds_concat(const char *left, const char *right);
void ds_log(const char *text);

#ifdef __cplusplus
}
#endif
#endif
