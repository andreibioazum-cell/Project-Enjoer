#!/bin/sh
# Same C engine as the APK; the /preview binary itself is Git-ignored.
set -eu
cd "$(dirname "$0")/../.."
${CC:-gcc} -O2 -std=c99 -Itools/preview/compat -Isrc -I. \
    src/core/log.c src/core/state.c src/core/assets.c \
    src/graphics/gfx_frame.c src/graphics/gfx_draw.c src/graphics/image.c \
    src/graphics/gfx_text.c src/graphics/ttf/ttf_outline.c src/graphics/ttf/ttf_font.c \
    src/geometrium/geometrium_render.c src/geometrium/geometrium_shapes.c src/geometrium/geometrium_material.c \
    src/geometrium/geometrium_terrain.c src/geometrium/geometrium_world.c src/geometrium/geometrium_light.c src/geometrium/geometrium_water.c src/geometrium/geometrium_mesh.c src/geometrium/geometrium_edits.c \
    src/geometrium/geometrium_interact.c src/geometrium/geometrium_perf.c src/geometrium/geometrium_player.c src/geometrium/geometrium_input.c \
    src/geometrium/geometrium_scene.c src/geometrium/geometrium_hud.c src/geometrium/geometrium_hand.c src/geometrium/geometrium_game.c \
    src/platformium/platformium_level.c src/platformium/platformium_input.c src/platformium/platformium_camera.c \
    src/platformium/platformium_particles.c src/platformium/platformium_player.c src/platformium/platformium_actors.c \
    src/platformium/platformium_render.c src/platformium/platformium_hud.c src/platformium/platformium_game.c \
    src/core/game.c \
    src/engine/eng_api.c src/engine/eng_node.c src/engine/eng_mesh.c src/engine/eng_scene.c src/engine/eng_script.c src/engine/eng_project.c \
    tools/preview/host_compat.c tools/preview/host_main.c tools/preview/frame_jpeg.c \
    -rdynamic -lm -lpthread -ldl -o preview
