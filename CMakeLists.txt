cmake_minimum_required(VERSION 3.16)
project(EnjoerMessenger C)
set(CMAKE_C_STANDARD 99)

# Единственная целевая платформа — Android: библиотека libds_game.so, которую
# забирает APK из game/AndroidManifest.xml. Вся логика мессенджера — чистый C
# (messenger/*), без DimScript: генерация game/game.c больше не нужна.
if(NOT ANDROID)
    message(FATAL_ERROR
        "Enjoer Messenger собирается только под Android. Запусти CMake через "
        "инструментальный файл NDK, например: cmake -B build "
        "-DCMAKE_TOOLCHAIN_FILE=$ANDROID_NDK_ROOT/build/cmake/android.toolchain.cmake "
        "-DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29. "
        "Для превью на ПК смотри tools/preview (обычный gcc, без CMake).")
endif()

add_library(ds_game SHARED
    main.c
    runtime.c
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(ds_game PRIVATE
    .
    messenger
    ${ANDROID_NDK}/sources/android/native_app_glue)
target_link_libraries(ds_game PRIVATE android log m)
