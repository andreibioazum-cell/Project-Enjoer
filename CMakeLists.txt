cmake_minimum_required(VERSION 3.16...3.31)
project(Enjoer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Dawn is intentionally an external dependency: it is a large WebGPU
# implementation and should be built with the same NDK/toolchain as the app.
# The C++ renderer has a small preview fallback, so a checkout is still
# buildable before Dawn has been downloaded.
option(ENJOER_USE_DAWN "Use Dawn/WebGPU for the Android cube surface" ON)
set(DAWN_ROOT "" CACHE PATH "Path to a Dawn checkout (when ENJOER_USE_DAWN=ON)")
set(ENJOER_DAWN_TARGET "" CACHE STRING "Dawn CMake target, if not dawn_native")

if(NOT ANDROID)
    message(FATAL_ERROR
        "The APK is the shipping target. Use the NDK toolchain; the host "
        "preview is built with tools/preview/build.sh.")
endif()

add_library(enjoer SHARED
    src/main.c
    src/cube_game.c
    src/dawn_cube.cpp
    src/core/log.c
    src/core/state.c
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)

set_target_properties(enjoer PROPERTIES OUTPUT_NAME ds_game)
target_include_directories(enjoer PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${ANDROID_NDK}/sources/android/native_app_glue)
target_compile_definitions(enjoer PRIVATE
    ENJOER_USE_DAWN=$<BOOL:${ENJOER_USE_DAWN}>)
target_link_libraries(enjoer PRIVATE android log m)

if(ENJOER_USE_DAWN)
    if(NOT DAWN_ROOT)
        message(FATAL_ERROR
            "ENJOER_USE_DAWN=ON requires -DDAWN_ROOT=/path/to/dawn. "
            "Use OFF only for the dependency-free preview fallback.")
    endif()
    add_subdirectory(${DAWN_ROOT} ${CMAKE_BINARY_DIR}/dawn EXCLUDE_FROM_ALL)

    if(ENJOER_DAWN_TARGET)
        target_link_libraries(enjoer PRIVATE ${ENJOER_DAWN_TARGET})
    elseif(TARGET webgpu_dawn)
        target_link_libraries(enjoer PRIVATE webgpu_dawn)
    elseif(TARGET dawn::webgpu_dawn)
        target_link_libraries(enjoer PRIVATE dawn::webgpu_dawn)
    elseif(TARGET dawn_native)
        target_link_libraries(enjoer PRIVATE dawn_native)
        if(TARGET dawn_proc)
            target_link_libraries(enjoer PRIVATE dawn_proc)
        endif()
    elseif(TARGET dawn::dawn_native)
        target_link_libraries(enjoer PRIVATE dawn::dawn_native)
        if(TARGET dawn::dawn_proc)
            target_link_libraries(enjoer PRIVATE dawn::dawn_proc)
        endif()
    else()
        message(FATAL_ERROR
            "Dawn was added but no renderer target was found. Set "
            "-DENJOER_DAWN_TARGET=<Dawn target>.")
    endif()
endif()
