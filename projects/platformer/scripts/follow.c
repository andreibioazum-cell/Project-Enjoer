/* Side-view follow camera: tracks the Hero on the X axis while keeping the
 * height and distance the camera was authored with in the scene file.
 * Frame-rate independent smoothing (exponential ease). */
#include "eng_api.h"
#include <math.h>

void eng_script_ready(EngNode *self) {
    float x, y, z;
    eng_node3d_get_position(self, &x, &y, &z);
    eng_node_udata_set(self, 0, y);   /* authored height */
    eng_node_udata_set(self, 1, z);   /* authored distance */
    (void)x;
}

void eng_script_process(EngNode *self, float dt) {
    EngNode *target = eng_node_find("Main/Hero");
    if (!target) return;
    float tx, ty, tz;
    eng_node3d_get_position(target, &tx, &ty, &tz);
    (void)ty; (void)tz;

    float cx, cy, cz;
    eng_node3d_get_position(self, &cx, &cy, &cz);
    (void)cz;
    float k = 1.0f - expf(-6.0f * dt);   /* framerate-independent lerp */
    eng_node3d_set_position(self, cx + (tx - cx) * k,
                            eng_node_udata_get(self, 0),
                            eng_node_udata_get(self, 1));
}
