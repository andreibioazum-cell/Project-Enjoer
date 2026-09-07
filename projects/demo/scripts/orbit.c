/* Demo script: slowly orbit the camera around the scene centre. */
#include <math.h>
#include "eng_api.h"

void eng_script_process(EngNode *self, float dt) {
    (void)dt;
    float t = (float)(eng_time() * 0.15);
    const float r = 9.0f;
    eng_node3d_set_position(self, sinf(t) * r, 3.2f, cosf(t) * r);
    /* look back at the centre: forward is (sin yaw, 0, cos yaw) */
    eng_node3d_set_rotation(self, t + 3.14159265f, -0.2f, 0);
}
