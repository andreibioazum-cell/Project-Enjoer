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

# The file lists live in sources.list so the APK and the PC preview build the
# same set of translation units; each section is one hardware configuration.
function(enjoer_sources out name)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/sources.list" lines)
    set(collecting FALSE)
    set(files "")
    foreach(line IN LISTS lines)
        if(line MATCHES "^\\[")
            if(line STREQUAL "[${name}]")
                set(collecting TRUE)
            else()
                set(collecting FALSE)
            endif()
            continue()
        endif()
        string(STRIP "${line}" stripped)
        if(collecting AND NOT stripped STREQUAL "" AND NOT stripped MATCHES "^#")
            list(APPEND files "${stripped}")
        endif()
    endforeach()
    set(${out} "${files}" PARENT_SCOPE)
endfunction()

enjoer_sources(ENJOER_COMMON common)
enjoer_sources(ENJOER_SOFTWARE software)
enjoer_sources(ENJOER_VULKAN vulkan)
enjoer_sources(ENJOER_ANDROID android)

add_library(enjoer SHARED
    ${ENJOER_COMMON}
    ${ENJOER_SOFTWARE}
    ${ENJOER_VULKAN}
    ${ENJOER_ANDROID}
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
# Filename contract of the existing, unchanged APK workflow and manifest.
set_target_properties(enjoer PROPERTIES OUTPUT_NAME ds_game)
target_include_directories(enjoer PRIVATE
    src
    src/geometrium
    ${ANDROID_NDK}/sources/android/native_app_glue)
# The Vulkan backend is always compiled in and picks a device at runtime; the
# loader is dlopen()ed, so libvulkan is never a link dependency.
target_compile_definitions(enjoer PRIVATE ENJOER_VULKAN=1)
target_link_libraries(enjoer PRIVATE android log m)
