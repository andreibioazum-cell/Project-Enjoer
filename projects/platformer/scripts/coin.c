/* Collectible coin: spins and bobs gently in place.
 *
 * The rest height and a per-coin phase offset are remembered in the node's
 * own udata slots, so any number of coins can share this one script without
 * stepping on each other. The `player` script removes coins it touches.
 */
#include "eng_api.h"
#include <math.h>

void eng_script_ready(EngNode *self) {
    float x, y, z;
    eng_node3d_get_position(self, &x, &y, &z);
    eng_node_udata_set(self, 0, y);                    /* rest height */
    eng_node_udata_set(self, 1, x * 0.71f + z * 1.37f); /* stable phase */
}

void eng_script_process(EngNode *self, float dt) {
    float yaw, pitch, roll;
    eng_node3d_get_rotation(self, &yaw, &pitch, &roll);
    eng_node3d_set_rotation(self, yaw + dt * 2.2f, pitch, roll);

    float base = eng_node_udata_get(self, 0);
    float phase = eng_node_udata_get(self, 1);
    float x, y, z;
    eng_node3d_get_position(self, &x, &y, &z);
    eng_node3d_set_position(self, x, base + 0.12f * sinf((float)eng_time() * 3.0f + phase), z);
}
