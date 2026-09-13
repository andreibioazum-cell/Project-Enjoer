/* Small platform state and recoverable error boundary shared by C and C++. */
#define _POSIX_C_SOURCE 200809L
#include "engine.h"
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <jni.h>
static jobject activity;
static JavaVM *java_vm;
#else
static void *activity;
static void *java_vm;
#endif

int screen_w;
int screen_h;
double dt;

static jmp_buf error_jump;
static int handler_active;
static int failed;
static char last_error[768];

void app_set_activity(void *value) { activity = value; }
void app_set_java_vm(void *value) { java_vm = value; }

void app_fail(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(last_error, sizeof(last_error), format, args);
    va_end(args);
    app_log_error("%s", last_error);
    failed = 1;
    if (handler_active) longjmp(error_jump, 1);
}

int app_call(AppCallback callback, void *arg, const char *label) {
    if (!callback) {
        app_fail("Missing callback: %s", label ? label : "unknown");
        return 0;
    }
    handler_active = 1;
    if (setjmp(error_jump) == 0) callback(arg);
    handler_active = 0;
    return !failed;
}

const char *app_error(void) { return last_error[0] ? last_error : "Unknown engine error"; }
int app_failed(void) { return failed; }
void app_clear_error(void) { failed = 0; last_error[0] = '\0'; }

void app_quit(void) {
#ifdef __ANDROID__
    if (java_vm && activity) {
        JNIEnv *env = NULL;
        int attached = 0;
        jint status = (*java_vm)->GetEnv(java_vm, (void **)&env, JNI_VERSION_1_6);
        if (status != JNI_OK) {
            status = (*java_vm)->AttachCurrentThread(java_vm, &env, NULL);
            attached = status == JNI_OK;
        }
        if (status == JNI_OK && env) {
            jclass cls = (*env)->GetObjectClass(env, activity);
            jmethodID finish = cls ? (*env)->GetMethodID(env, cls, "finish", "()V") : NULL;
            if (finish) (*env)->CallVoidMethod(env, activity, finish);
            if (cls) (*env)->DeleteLocalRef(env, cls);
            if (attached) (*java_vm)->DetachCurrentThread(java_vm);
            return;
        }
    }
#endif
    _exit(0);
}
