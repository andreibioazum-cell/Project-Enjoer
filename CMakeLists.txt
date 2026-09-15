cmake_minimum_required(VERSION 3.16...3.31)
project(Enjoer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The Android NDK *is* clang, and -O3 is what makes the DimScript VM fast enough
# to interpret a whole game every frame. The host does not use this file at all
# (tools/preview/build.sh and tools/tests/run.sh do, also with clang and -O3), so
# the flags below are the ones that end up in the shipping library.
#
# -Werror is deliberately NOT set here: the NDK compiles its own
# android_native_app_glue.c into this target, and a warning there is not ours to
# fix. The host test build keeps -Werror for every line of our code.
option(ENJOER_USE_VULKAN "Use Vulkan for the Android cube surface" ON)
set(ENJOER_GAME_DIR "games/brick" CACHE PATH "Game folder packed into assets/game/ (empty = none)")

if(NOT ANDROID)
    message(FATAL_ERROR
        "The APK is the shipping target. Use the NDK toolchain; the host "
        "preview is built with tools/preview/build.sh.")
endif()

add_library(enjoer SHARED
    src/main.c
    src/cube_game.c
    src/core/log.c
    src/core/state.c
    src/enjoer_draw.c
    src/dimscript_runtime.c
    src/ds_manifest.c
    src/ds_files.c
    src/ds_image.c
    src/ds_png.c
    src/generated/clicker.c
    src/vulkan_cube.cpp
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

# STRICT COMPILER MODE: NO VM, NO REFCOUNT, MANUAL MEMORY, SPEED LIKE C, AOT TO MACHINE CODE
# VM files removed: ds_vm.c, ds_vm_heap.c, ds_vm_lang.c, ds_vm_exec.c
# Only AOT compiler (dimscript/compiler.py -> C99 -> clang -O3 -> .so) remains

set_target_properties(enjoer PROPERTIES OUTPUT_NAME ds_game)
target_include_directories(enjoer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ANDROID_NDK}/sources/android/native_app_glue)
target_compile_definitions(enjoer PRIVATE
    ENJOER_USE_VULKAN=$<BOOL:${ENJOER_USE_VULKAN}>)
# These come after the toolchain's own flags, so -O3 wins over the NDK's default
# -O2 for a Release build, and a Debug build keeps its -O0 for the debugger.
target_compile_options(enjoer PRIVATE
    -Wall -Wextra -fno-strict-aliasing
    $<$<NOT:$<CONFIG:Debug>>:-O3>)
target_link_libraries(enjoer PRIVATE android log m)

if(ENJOER_USE_VULKAN)
    target_link_libraries(enjoer PRIVATE vulkan)
endif()

# A game is data, not code. tools/gamepack.py copies game.manifest and the .ds
# NOTE: ENJOER_GAME_DIR is a PATH cache entry, so CMake has already turned a
# relative value into an absolute one; concatenating the source dir onto it
# would double the prefix, which is exactly the mistake this comment exists for.
# files next to the library and generates AndroidManifest.xml from the same
# manifest, so the runtime and the package can never disagree. Python is a host
# tool and the NDK toolchain may not be able to find it, so this stays optional:
# `python3 tools/gamepack.py <game> --staging <dir>` does the same thing.
if(ENJOER_GAME_DIR)
    if(NOT IS_DIRECTORY "${ENJOER_GAME_DIR}")
        message(FATAL_ERROR "ENJOER_GAME_DIR=${ENJOER_GAME_DIR} is not a directory")
    endif()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        add_custom_target(enjoer_assets ALL
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gamepack.py"
                    "${ENJOER_GAME_DIR}"
                    --staging "${CMAKE_CURRENT_BINARY_DIR}/game"
            BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/game/AndroidManifest.xml"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gamepack.py"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            COMMENT "Packing ${ENJOER_GAME_DIR} into assets/game")
        add_dependencies(enjoer enjoer_assets)
    else()
        message(STATUS "Enjoer: no host Python3; run tools/gamepack.py manually for assets/game")
    endif()
endif()
