#!/bin/sh
# Same C modules as Android. SANITIZE=1 enables ASan/UBSan/float-cast checks.
set -eu
cd "$(dirname "$0")/../.."
CC=${CC:-gcc}
DIR=${BUILD_DIR:-build-tests/regression}
mkdir -p "$DIR/fixtures"
FLAGS="-std=c99 -Wall -Wextra -Werror -Itools/preview/compat -I."
if [ "${SANITIZE:-0}" = 1 ]; then
    FLAGS="$FLAGS -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer"
else
    FLAGS="$FLAGS -O2"
fi
CORE="core/log.c core/state.c core/assets.c tools/preview/host_compat.c"
WORLD="rbx/rbx_world.c rbx/rbx_terrain.c rbx/rbx_mesh.c rbx/rbx_edits.c rbx/rbx_water.c"
ACTORS="rbx/rbx_player.c rbx/rbx_input.c rbx/rbx_interact.c"
# Intentional splitting of source/flag lists, no .c includes or generated runtime.
$CC $FLAGS $CORE graphics/image.c rbx/rbx_render.c rbx/rbx_shapes.c rbx/rbx_material.c tools/tests/render.c -lm -lpthread -o "$DIR/render"
"$DIR/render"
$CC $FLAGS $CORE $WORLD $ACTORS rbx/rbx_perf.c rbx/rbx_game.c rbx/rbx_scene.c rbx/rbx_hud.c tools/tests/controls.c -lm -lpthread -o "$DIR/controls"
"$DIR/controls"
$CC $FLAGS $CORE $WORLD tools/tests/world.c -lm -o "$DIR/world"
"$DIR/world"
$CC $FLAGS $CORE $WORLD tools/tests/water.c -lm -o "$DIR/water"
"$DIR/water"
$CC $FLAGS $CORE $WORLD rbx/rbx_render.c rbx/rbx_shapes.c rbx/rbx_material.c graphics/image.c \
    tools/tests/culling.c -lm -lpthread -o "$DIR/culling"
"$DIR/culling"
$CC $FLAGS $CORE $WORLD $ACTORS tools/tests/edits.c -lm -o "$DIR/edits"
"$DIR/edits" "$DIR/fixtures"
$CC $FLAGS -DPREVIEW_EXTERNAL_AUDIO $CORE sound/sound.c tools/tests/audio.c -lm -lpthread -o "$DIR/audio"
"$DIR/audio"
$CC $FLAGS $CORE $WORLD $ACTORS rbx/rbx_perf.c rbx/rbx_game.c rbx/rbx_scene.c rbx/rbx_hud.c \
    rbx/rbx_render.c rbx/rbx_shapes.c rbx/rbx_material.c graphics/image.c graphics/gfx_frame.c \
    graphics/gfx_draw.c graphics/gfx_text.c graphics/ttf/ttf_font.c graphics/ttf/ttf_outline.c \
    tools/tests/game.c -lm -lpthread -o "$DIR/game"
"$DIR/game"
