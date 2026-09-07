/* Player ball controller for the Physics Playground.
 *
 * Polls the engine input API every frame: WASD / arrows apply a horizontal
 * force (clamped to a top speed), Space jumps when grounded, and a pointer
 * press (mouse / touch) gives the ball a little pop. Uses the StaticBody3D /
 * RigidBody3D impulse API in eng_api.h.
 */
#include "eng_api.h"
#include <math.h>

#define ACCEL  42.0f   /* horizontal acceleration, world units / s^2 */
#define MAX_SPEED 8.0f
#define JUMP_SPEED 8.0f

static int prev_space, prev_pointer;

void eng_script_ready(EngNode *self) {
    prev_space = 0;
    prev_pointer = 0;
}

void eng_script_process(EngNode *self, float dt) {
    /* horizontal drive from WASD / arrows */
    float dx = 0.0f, dz = 0.0f;
    if (eng_input_is_pressed(ENG_KEY_LEFT))  dx -= 1.0f;
    if (eng_input_is_pressed(ENG_KEY_RIGHT)) dx += 1.0f;
    if (eng_input_is_pressed(ENG_KEY_UP))    dz -= 1.0f;
    if (eng_input_is_pressed(ENG_KEY_DOWN))  dz += 1.0f;
    if (dx != 0.0f || dz != 0.0f) {
        float len = sqrtf(dx * dx + dz * dz);
        eng_body_apply_impulse(self, dx / len * ACCEL * dt,
                                       0.0f,
                                       dz / len * ACCEL * dt);
    }

    /* cap horizontal speed so the ball does not run away */
    float vx, vy, vz;
    eng_body_get_linear_velocity(self, &vx, &vy, &vz);
    float h = sqrtf(vx * vx + vz * vz);
    if (h > MAX_SPEED) {
        float k = MAX_SPEED / h;
        eng_body_set_linear_velocity(self, vx * k, vy, vz * k);
    }

    /* jump on the rising edge of Space (only when grounded) */
    int space = eng_input_is_pressed(ENG_KEY_SPACE);
    if (space && !prev_space && eng_body_is_grounded(self)) {
        eng_body_get_linear_velocity(self, &vx, &vy, &vz);
        eng_body_set_linear_velocity(self, vx, JUMP_SPEED, vz);
    }
    prev_space = space;

    /* a pointer press pops the ball up and forward */
    float px, py; int down;
    eng_input_pointer(&px, &py, &down);
    if (down && !prev_pointer && eng_body_is_grounded(self)) {
        eng_body_get_linear_velocity(self, &vx, &vy, &vz);
        eng_body_set_linear_velocity(self, vx, 6.0f, vz - 4.0f);
    }
    prev_pointer = down;
    (void)self;
}
