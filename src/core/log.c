/* Android logcat on the device and stderr in the preview. */
#include "engine.h"
#include <stdarg.h>

#ifdef __ANDROID__
#include <android/log.h>
#else
#include <stdio.h>
#endif

void app_log(const char *format, ...) {
    va_list args;
    va_start(args, format);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_INFO, "Enjoer", format, args);
#else
    fputs("[Enjoer] ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
#endif
    va_end(args);
}

void app_log_error(const char *format, ...) {
    va_list args;
    va_start(args, format);
#ifdef __ANDROID__
    __android_log_vprint(ANDROID_LOG_ERROR, "Enjoer", format, args);
#else
    fputs("[Enjoer:error] ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
#endif
    va_end(args);
}
