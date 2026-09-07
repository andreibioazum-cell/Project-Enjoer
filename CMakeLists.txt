cmake_minimum_required(VERSION 3.16)
project(Enjoer C)
set(CMAKE_C_STANDARD 99)

# The only shipping platform is Android: libds_game.so, packaged by the APK
# described in game/AndroidManifest.xml. The game is a pure-C 3D voxel
# world called Geometrium.
#
# Source layout (every file is its own translation unit):
#   src/main.c            Android entry point (native activity)
#   src/core/             app state, error handling, logging, asset reads
#   src/graphics/         2D rendering: primitives, textures, text (+ ttf/)
#   src/sound/            audio: PCM mixer and AudioTrack output
#   src/geometrium/       the 3D world: render, terrain, player, input, HUD
# Runtime assets (PNG tiles, font, WAV effects) live in assets/.
if(NOT ANDROID)
    message(FATAL_ERROR
        "Enjoer builds for Android only. Run CMake through the NDK toolchain "
        "file, e.g.: cmake -B build "
        "-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake "
        "-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29. "
        "For the PC preview see tools/preview (plain gcc, no CMake).")
endif()

add_library(enjoer SHARED
    src/main.c
    src/core/log.c
    src/core/state.c
    src/core/assets.c
    src/graphics/gfx_frame.c
    src/graphics/gfx_draw.c
    src/graphics/image.c
    src/graphics/gfx_text.c
    src/graphics/ttf/ttf_outline.c
    src/graphics/ttf/ttf_font.c
    src/sound/sound.c
    src/sound/sound_android.c
    src/geometrium/geometrium_render.c
    src/geometrium/geometrium_shapes.c
    src/geometrium/geometrium_material.c
    src/geometrium/geometrium_terrain.c
    src/geometrium/geometrium_world.c
    src/geometrium/geometrium_light.c
    src/geometrium/geometrium_water.c
    src/geometrium/geometrium_mesh.c
    src/geometrium/geometrium_edits.c
    src/geometrium/geometrium_interact.c
    src/geometrium/geometrium_perf.c
    src/geometrium/geometrium_player.c
    src/geometrium/geometrium_input.c
    src/geometrium/geometrium_scene.c
    src/geometrium/geometrium_hud.c
    src/geometrium/geometrium_hand.c
    src/geometrium/geometrium_game.c
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
# Filename contract of the existing, unchanged APK workflow and manifest.
set_target_properties(enjoer PROPERTIES OUTPUT_NAME ds_game)
target_include_directories(enjoer PRIVATE
    src
    src/geometrium
    ${ANDROID_NDK}/sources/android/native_app_glue)
target_link_libraries(enjoer PRIVATE android log m)
