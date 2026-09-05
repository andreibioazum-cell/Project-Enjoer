/* core/state.c — глобальное состояние кадра и обработка ошибок рантайма.
 * Ошибка скрипта не роняет приложение: вместо падения — экран с текстом
 * и консолью (см. graphics/gfx_text.c, ds_graphics_error_screen). */
#include "runtime.h"
#include <stdarg.h>
#include <stdio.h>

#define DS_ERROR_MESSAGE_SIZE 1024

/* Глобальное состояние кадра (объявлено в runtime.h). */
Joy joy = {0};
int screen_w = 0;
int screen_h = 0;
double dt = 0.0;
int mouse_clicked = 0;
double ds_mouse_x = 0.0;
double ds_mouse_y = 0.0;

static jmp_buf ds_error_jump;
static int ds_error_handler_active = 0;
static int ds_has_error = 0;
static int ds_restart_requested = 0;
static char ds_last_error[DS_ERROR_MESSAGE_SIZE] = {0};

void ds_console_add(const char *line, int is_error); /* core/log.c */

void ds_runtime_error(const char *format, ...) {
    char tmp[512];
    va_list args, copy;
    va_start(args, format);
    va_copy(copy, args);
    vsnprintf(ds_last_error, sizeof(ds_last_error), format, copy);
    va_end(copy);
    __android_log_vprint(ANDROID_LOG_ERROR, "DimScript", format, args);
    va_end(args);
    va_start(args, format);
    vsnprintf(tmp, sizeof(tmp), format, args);
    va_end(args);
    ds_console_add(tmp, 1);
    ds_has_error = 1;
    if (ds_error_handler_active) longjmp(ds_error_jump, 1);
}

int ds_call_protected(DSProtectedFunction function, void *userdata, const char *label) {
    int jumped;
    if (!function) {
        if (label && *label) ds_runtime_error("cannot call an empty script hook '%s'", label);
        else ds_runtime_error("cannot call an empty script hook");
        return 0;
    }
    if (ds_error_handler_active) {
        function(userdata);
        return !ds_has_error;
    }
    ds_error_handler_active = 1;
    jumped = setjmp(ds_error_jump);
    if (jumped == 0) {
        function(userdata);
        ds_error_handler_active = 0;
        return !ds_has_error;
    }
    ds_error_handler_active = 0;
    if (label && *label && ds_last_error[0] == '\0') {
        snprintf(ds_last_error, sizeof(ds_last_error), "script hook '%s' failed", label);
    }
    return 0;
}

const char *ds_runtime_error_message(void) { return ds_last_error[0] ? ds_last_error : "unknown DimScript runtime error"; }
int ds_script_has_error(void) { return ds_has_error; }
void ds_clear_runtime_error(void) { ds_has_error = 0; ds_last_error[0] = '\0'; }
void ds_request_script_restart(void) { ds_restart_requested = 1; }
int ds_script_restart_requested(void) { return ds_restart_requested; }
void ds_clear_script_restart(void) { ds_restart_requested = 0; }
