/* src/render_sw.c — the software rasterizer as a RenderBackend. */
#include "render.h"
#include "graphics/gfx_sw.h"
#include "geometrium/geometrium_sw.h"

const RenderBackend sw_render_backend = {
    .name = "software rasterizer",
    .windowed = 0,
    .init = sw_gfx_init,
    .shutdown = sw_gfx_shutdown,
    .attach_window = NULL,
    .begin_frame = sw_gfx_begin_frame,
    .end_frame = sw_gfx_end_frame,
    .cancel_frame = sw_gfx_cancel_frame,
    .error_screen = sw_gfx_error_screen,
    .rect = sw_rect,
    .roundrect = sw_roundrect,
    .circle = sw_circle,
    .ring = sw_ring,
    .line = sw_line,
    .image_draw = sw_image_draw,
    .text_scaled = sw_text_scaled,
    .text_width = sw_text_width,
    .begin3d = sw_geometrium3d_begin,
    .sky = sw_geometrium3d_sky,
    .fog = sw_geometrium3d_fog,
    .surface = geometrium3d_quad_surface,
    .segment = sw_geometrium3d_segment,
    .visible = sw_geometrium3d_visible,
    .face_visible = sw_geometrium3d_face_visible,
    .project = sw_geometrium3d_project,
    .depth_clear = sw_geometrium3d_depth_clear,
    .viewmodel = sw_geometrium3d_viewmodel,
    .polygon = sw_geometrium3d_polygon,
    .chunks = NULL,
    .end3d = sw_geometrium3d_end,
};
