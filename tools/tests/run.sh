#!/bin/sh
# SANITIZE=1 tools/tests/run.sh — проверки с ASan/UBSan.
set -eu
cd "$(dirname "$0")/../.."
CC=${CC:-gcc}
DIR=${BUILD_DIR:-build-tests/regression}
mkdir -p "$DIR"
FLAGS="-std=c99 -Wall -Wextra -Werror -Itools/preview/compat -I."
if [ "${SANITIZE:-0}" = 1 ]; then
    FLAGS="$FLAGS -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer"
else
    FLAGS="$FLAGS -O2"
fi
# Намеренное разбиение FLAGS на аргументы shell.
$CC $FLAGS rbx/rbx_render.c rbx/rbx_shapes.c rbx/rbx_material.c tools/tests/render.c -lm -o "$DIR/render"
"$DIR/render"
$CC $FLAGS rbx/rbx_player.c rbx/rbx_input.c rbx/rbx_world.c rbx/rbx_terrain.c \
    rbx/rbx_game.c rbx/rbx_scene.c rbx/rbx_hud.c tools/tests/controls.c \
    -lm -o "$DIR/controls"
"$DIR/controls"

$CC $FLAGS rbx/rbx_world.c rbx/rbx_terrain.c tools/tests/world.c -lm -o "$DIR/world"
"$DIR/world"
