cmake_minimum_required(VERSION 3.16...3.31)
project(Enjoer C)
set(CMAKE_C_STANDARD 99)

# The only shipping platform is Android: libds_game.so, packaged by the APK
# described in game/AndroidManifest.xml. The app is the Enjoer engine:
# projects with .escn scenes, typed nodes, physics and C scripts, rendered by
# the software 3D / voxel backend in src/engine/render/.
#
# Source layout (every file is its own translation unit):
#   src/main.c            Android entry point (native activity)
#   src/core/             app state, error handling, logging, asset reads,
#                         and the engine launcher router (game.c)
#   src/graphics/         2D rendering: primitives, textures, text (+ ttf/)
#   src/sound/            audio: PCM mixer and AudioTrack output
#   src/engine/           the engine: nodes, scenes, projects, physics,
#                         input, scripting (+ render/ 3D & voxel backend)
# Runtime assets (PNG tiles, font, WAV effects) and the staged projects live
# in assets/ and assets/projects/ (see stage_assets.py).
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
    src/engine/render/rend3d.c
    src/engine/render/rend_shapes.c
    src/engine/render/rend_material.c
    src/engine/render/rend_perf.c
    src/engine/render/voxel_terrain.c
    src/engine/render/voxel_world.c
    src/engine/render/voxel_light.c
    src/engine/render/voxel_water.c
    src/engine/render/voxel_mesh.c
    src/engine/render/voxel_edits.c
    src/engine/eng_api.c
    src/engine/eng_node.c
    src/engine/eng_mesh.c
    src/engine/eng_physics.c
    src/engine/eng_input.c
    src/engine/eng_scene.c
    src/engine/eng_script.c
    src/engine/eng_script_builtin.c
    src/engine/eng_project.c
    src/engine/eng_fs.c
    src/core/game.c
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
# Filename contract of the existing, unchanged APK workflow and manifest.
set_target_properties(enjoer PROPERTIES OUTPUT_NAME ds_game)
# The project root is on the include path so translation units can reach
# vendored single-file libraries as "third_party/..." (stb_image and friends);
# the PC preview mirrors this with `-I.` in tools/preview/build.sh.
target_include_directories(enjoer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    src
    ${ANDROID_NDK}/sources/android/native_app_glue)
target_link_libraries(enjoer PRIVATE android log m)
