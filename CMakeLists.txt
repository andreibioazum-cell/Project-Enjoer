cmake_minimum_required(VERSION 3.16...3.31)
project(Enjoer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vulkan ships with the Android NDK (headers + libvulkan.so, API 24+), so the
# GPU renderer has no external dependency. The C++ renderer keeps a small
# software fallback for the host preview and for smoke builds.
option(ENJOER_USE_VULKAN "Use Vulkan for the Android cube surface" ON)

if(NOT ANDROID)
    message(FATAL_ERROR
        "The APK is the shipping target. Use the NDK toolchain; the host "
        "preview is built with tools/preview/build.sh.")
endif()

add_library(enjoer SHARED
    src/main.c
    src/cube_game.c
    src/dimscript_runtime.c
    src/generated/clicker.c
    src/vulkan_cube.cpp
    src/core/log.c
    src/core/state.c
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

set_target_properties(enjoer PROPERTIES OUTPUT_NAME ds_game)
target_include_directories(enjoer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ANDROID_NDK}/sources/android/native_app_glue)
target_compile_definitions(enjoer PRIVATE
    ENJOER_USE_VULKAN=$<BOOL:${ENJOER_USE_VULKAN}>)
target_link_libraries(enjoer PRIVATE android log m)

if(ENJOER_USE_VULKAN)
    target_link_libraries(enjoer PRIVATE vulkan)
endif()
