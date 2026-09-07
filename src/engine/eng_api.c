/* Small runtime services exposed to C scripts. */
#include "eng_internal.h"
#include "eng_api.h"

/* Engine bootstrap: textures for voxel-style meshes, no world or player. */
int eng_init(AAssetManager *assets) {
    if (!geometrium_materials_load(assets)) {
        app_fail("Engine: could not load materials");
        return 0;
    }
    eng_physics_init();
    eng_input_reset();
    return 1;
}

static double time_now, time_delta, time_origin = -1;

void eng_print(const char *message) { app_log("%s", message ? message : ""); }

void eng_time_internal(double now, double dt) {
    if (time_origin < 0) time_origin = now;
    time_now = now - time_origin;
    time_delta = dt;
}
double eng_time(void) { return time_now; }
float eng_delta(void) { return (float)time_delta; }
