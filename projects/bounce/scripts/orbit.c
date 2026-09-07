/* Physics Playground camera: sweep slowly around the arena so the scene and
 * the moving ball stay in view. Mirrors the demo's orbiting camera. */
#include <math.h>
#include "eng_api.h"

void eng_script_process(EngNode *self, float dt) {
    (void)dt;
    float t = (float)(eng_time() * 0.10);
    const float r = 11.0f;
    eng_node3d_set_position(self, sinf(t) * r, 4.0f, cosf(t) * r);
    /* look back at the centre: forward is (sin yaw, 0, cos yaw) */
    eng_node3d_set_rotation(self, t + 3.14159265f, -0.3f, 0);
}
