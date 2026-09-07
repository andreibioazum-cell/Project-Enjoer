/* Demo script: spin the node around the vertical axis. */
#include "eng_api.h"

void eng_script_process(EngNode *self, float dt) {
    float yaw, pitch, roll;
    eng_node3d_get_rotation(self, &yaw, &pitch, &roll);
    eng_node3d_set_rotation(self, yaw + dt * 0.9f, pitch, roll);
}
