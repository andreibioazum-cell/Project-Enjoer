cmake_minimum_required(VERSION 3.16...3.31)
project(Enjoer LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# The NDK *is* clang, so the host build follows suit: the same compiler family
# and the same -O3 for the DimScript VM, the renderer and the generated C.  A
# debug build keeps -O0 on purpose, everything else gets -O3.
option(ENJOER_USE_VULKAN "Use Vulkan for the Android cube surface" ON)
set(ENJOER_GAME_DIR "games/brick" CACHE PATH "Game folder packed into assets/game/ (empty = no game)")

if(NOT ANDROID)
    message(FATAL_ERROR
        "The APK is the shipping target. Use the NDK toolchain; the host "
        "preview is built with tools/preview/build.sh.")
endif()

find_program(CLANG_C clang)
find_program(CLANG_CXX clang++)
if(CLANG_C AND CLANG_CXX AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(STATUS "Enjoer: switching to clang (${CLANG_C})")
    set(CMAKE_C_COMPILER "${CLANG_C}" CACHE FILEPATH "C compiler" FORCE)
    set(CMAKE_CXX_COMPILER "${CLANG_CXX}" CACHE FILEPATH "C++ compiler" FORCE)
endif()

foreach(language C CXX)
    string(TOLOWER "${language}" flag)
    set(CMAKE_${language}_FLAGS_DEBUG "-O0 -g3" CACHE STRING "" FORCE)
    set(CMAKE_${language}_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)
    set(CMAKE_${language}_FLAGS_RELWITHDEBINFO "-O3 -g -DNDEBUG" CACHE STRING "" FORCE)
    set(CMAKE_${language}_FLAGS "-Wall -Wextra -Werror ${CMAKE_${language}_FLAGS}")
endforeach()

add_library(enjoer SHARED
    src/main.c
    src/cube_game.c
    src/core/log.c
    src/core/state.c
    src/enjoer_draw.c
    src/dimscript_runtime.c
    src/ds_manifest.c
    src/ds_files.c
    src/ds_vm.c
    src/ds_vm_heap.c
    src/ds_vm_lang.c
    src/ds_vm_exec.c
    src/generated/clicker.c
    src/vulkan_cube.cpp
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

# The game itself is data, not code: gamepack copies game.manifest and the .ds
# files next to the library so the runtime can open them through the asset
# manager.  Regenerate the whole staging folder with `ninja enjoer_assets`.
if(ENJOER_GAME_DIR)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_target(enjoer_assets ALL
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/gamepack.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/${ENJOER_GAME_DIR}"
                --staging "${CMAKE_CURRENT_BINARY_DIR}/game"
        BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/game/AndroidManifest.xml"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gamepack.py"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        COMMENT "Packing ${ENJOER_GAME_DIR} into assets/game")
    add_dependencies(enjoer enjoer_assets)
endif()
