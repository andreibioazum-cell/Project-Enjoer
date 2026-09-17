/* Software renderer entry points: the 2D HUD, the frame lifecycle and the
 * error screen. Selected at runtime by src/render.c; the Vulkan build provides
 * the same names with the vk_ prefix in src/vk/. */
#ifndef GFX_SW_H
#define GFX_SW_H

#include "engine.h"

int sw_gfx_init(AAssetManager *assets);
int sw_gfx_begin_frame(Buffer *buffer);
void sw_gfx_end_frame(void);
void sw_gfx_cancel_frame(void);
void sw_gfx_shutdown(void);
void sw_gfx_error_screen(const char *message);

void sw_rect(float x, float y, float w, float h, uint32_t color);
void sw_roundrect(float x, float y, float w, float h, float radius, uint32_t color);
void sw_circle(float x, float y, float radius, uint32_t color);
void sw_ring(float x, float y, float radius, float thickness, uint32_t color);
void sw_line(float x1, float y1, float x2, float y2, float thickness, uint32_t color);
void sw_image_draw(const Image *image, float x, float y, float w, float h);
void sw_text_scaled(const char *string, float x, float y, uint32_t color, float scale);
int sw_text_width(const char *string);

#endif
