#!/bin/sh
# Тот же C-движок, что в APK; результат /preview игнорируется Git.
set -eu
cd "$(dirname "$0")/../.."
${CC:-gcc} -O2 -std=c99 -Itools/preview/compat -I. \
    core/log.c core/state.c core/strings.c core/arrays.c \
    graphics/gfx_frame.c graphics/gfx_draw.c graphics/gfx_texture.c \
    graphics/gfx_text.c graphics/ttf/ttf_outline.c graphics/ttf/ttf_font.c \
    rbx/rbx_render.c rbx/rbx_shapes.c rbx/rbx_material.c \
    rbx/rbx_terrain.c rbx/rbx_world.c rbx/rbx_player.c rbx/rbx_input.c \
    rbx/rbx_scene.c rbx/rbx_hud.c rbx/rbx_game.c \
    tools/preview/host_compat.c tools/preview/host_main.c \
    -lm -lpthread -o preview
