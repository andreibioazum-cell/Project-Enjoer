#!/bin/sh
# Same C modules as Android. SANITIZE=1 enables ASan/UBSan/float-cast checks.
set -eu
cd "$(dirname "$0")/../.."
CC=${CC:-gcc}
DIR=${BUILD_DIR:-build-tests/regression}
mkdir -p "$DIR/fixtures"
FLAGS="-std=c99 -Wall -Wextra -Werror -Itools/preview/compat -Isrc -I."
if [ "${SANITIZE:-0}" = 1 ]; then
    FLAGS="$FLAGS -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer"
else
    FLAGS="$FLAGS -O2"
fi
CORE="src/core/log.c src/core/state.c src/core/assets.c tools/preview/host_compat.c"
WORLD="src/geometrium/geometrium_world.c src/geometrium/geometrium_light.c src/geometrium/geometrium_water.c src/geometrium/geometrium_terrain.c src/geometrium/geometrium_mesh.c src/geometrium/geometrium_edits.c"
ACTORS="src/geometrium/geometrium_player.c src/geometrium/geometrium_input.c src/geometrium/geometrium_interact.c"
# Intentional splitting of source/flag lists, no .c includes or generated runtime.
$CC $FLAGS $CORE src/graphics/image.c src/geometrium/geometrium_render.c src/geometrium/geometrium_shapes.c src/geometrium/geometrium_material.c tools/tests/render.c -lm -o "$DIR/render"
"$DIR/render"
$CC $FLAGS $CORE src/geometrium/geometrium_hand.c tools/tests/hand.c -lm -o "$DIR/hand"
"$DIR/hand"
$CC $FLAGS $CORE $WORLD $ACTORS src/geometrium/geometrium_perf.c src/geometrium/geometrium_game.c src/geometrium/geometrium_scene.c src/geometrium/geometrium_hud.c src/geometrium/geometrium_hand.c tools/tests/controls.c -lm -o "$DIR/controls"
"$DIR/controls"
$CC $FLAGS $CORE $WORLD tools/tests/world.c -lm -o "$DIR/world"
"$DIR/world"
$CC $FLAGS $CORE $WORLD tools/tests/water.c -lm -o "$DIR/water"
"$DIR/water" "$DIR/fixtures/water"
$CC $FLAGS $CORE $WORLD $ACTORS src/geometrium/geometrium_hand.c tools/tests/edits.c -lm -o "$DIR/edits"
"$DIR/edits" "$DIR/fixtures"
$CC $FLAGS -DPREVIEW_EXTERNAL_AUDIO $CORE src/sound/sound.c tools/tests/audio.c -lm -lpthread -o "$DIR/audio"
"$DIR/audio"
$CC $FLAGS $CORE $WORLD $ACTORS src/geometrium/geometrium_perf.c src/geometrium/geometrium_game.c src/geometrium/geometrium_scene.c src/geometrium/geometrium_hud.c src/geometrium/geometrium_hand.c \
    src/geometrium/geometrium_render.c src/geometrium/geometrium_shapes.c src/geometrium/geometrium_material.c src/graphics/image.c src/graphics/gfx_frame.c \
    src/graphics/gfx_draw.c src/graphics/gfx_text.c src/graphics/ttf/ttf_font.c src/graphics/ttf/ttf_outline.c \
    tools/tests/game.c -lm -o "$DIR/game"
"$DIR/game"
