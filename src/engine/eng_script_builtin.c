/* Scripts compiled into the engine binary.
 *
 * A phone has no C compiler and no writable, loadable .so next to the APK, so
 * the C files under a project's scripts/ folder cannot be dlopen'd there.
 * that ship with the sample projects also exist here, by the same name, and
 * are used whenever the compiled version is unavailable. On the PC the
 * own .so wins, so editing a project script file still takes effect.
 */
#include "eng_internal.h"
#include "eng_api.h"
#include <math.h>

/* ── spin: rotate the node around its vertical axis ───────────────────── */
static void spin_process(EngNode *self, float dt) {
    float yaw, pitch, roll;
    eng_node3d_get_rotation(self, &yaw, &pitch, &roll);
    eng_node3d_set_rotation(self, yaw + dt * 0.9f, pitch, roll);
}

/* ── orbit: sweep a camera around the point it was authored to look at ── */
/* Radius, height, tilt and phase all come from the scene file, so one
 * implementation serves every project that names a camera script "orbit". */
typedef struct { float radius, height, pitch, phase; } OrbitState;
static OrbitState orbit_state;

static void orbit_ready(EngNode *self) {
    float x, y, z;
    eng_node3d_get_position(self, &x, &y, &z);
    orbit_state.radius = sqrtf(x * x + z * z);
    if (orbit_state.radius < 1.0f) orbit_state.radius = 9.0f;
    orbit_state.height = y;
    eng_node3d_get_rotation(self, NULL, &orbit_state.pitch, NULL);
    orbit_state.phase = atan2f(x, z);
}

static void orbit_process(EngNode *self, float dt) {
    (void)dt;
    float t = (float)(eng_time() * 0.12) + orbit_state.phase;
    eng_node3d_set_position(self, sinf(t) * orbit_state.radius, orbit_state.height,
                            cosf(t) * orbit_state.radius);
    /* look back at the centre: forward is (sin yaw, 0, cos yaw) */
    eng_node3d_set_rotation(self, t + 3.14159265f, orbit_state.pitch, 0);
}

/* ── roller: drive a RigidBody3D ball with the engine input API ───────── */
#define ROLLER_ACCEL 42.0f      /* world units / s^2 */
#define ROLLER_MAX_SPEED 8.0f
#define ROLLER_JUMP_SPEED 8.0f

static int roller_prev_space, roller_prev_pointer;

static void roller_ready(EngNode *self) {
    (void)self;
    roller_prev_space = 0;
    roller_prev_pointer = 0;
}

static void roller_process(EngNode *self, float dt) {
    float dx = 0.0f, dz = 0.0f;
    float vx, vy, vz;
    if (eng_input_is_pressed(ENG_KEY_LEFT)) dx -= 1.0f;
    if (eng_input_is_pressed(ENG_KEY_RIGHT)) dx += 1.0f;
    if (eng_input_is_pressed(ENG_KEY_UP)) dz -= 1.0f;
    if (eng_input_is_pressed(ENG_KEY_DOWN)) dz += 1.0f;
    if (dx != 0.0f || dz != 0.0f) {
        float len = sqrtf(dx * dx + dz * dz);
        eng_body_apply_impulse(self, dx / len * ROLLER_ACCEL * dt, 0.0f,
                               dz / len * ROLLER_ACCEL * dt);
    }

    /* cap the horizontal speed so the ball cannot run away */
    eng_body_get_linear_velocity(self, &vx, &vy, &vz);
    float h = sqrtf(vx * vx + vz * vz);
    if (h > ROLLER_MAX_SPEED) {
        float k = ROLLER_MAX_SPEED / h;
        eng_body_set_linear_velocity(self, vx * k, vy, vz * k);
    }

    /* jump on the rising edge of Space, only while grounded */
    int space = eng_input_is_pressed(ENG_KEY_SPACE);
    if (space && !roller_prev_space && eng_body_is_grounded(self)) {
        eng_body_get_linear_velocity(self, &vx, &vy, &vz);
        eng_body_set_linear_velocity(self, vx, ROLLER_JUMP_SPEED, vz);
    }
    roller_prev_space = space;

    /* a fresh pointer press pops the ball up and forward */
    float px, py;
    int down = 0;
    eng_input_pointer(&px, &py, &down);
    if (down && !roller_prev_pointer && eng_body_is_grounded(self)) {
        eng_body_get_linear_velocity(self, &vx, &vy, &vz);
        eng_body_set_linear_velocity(self, vx, 6.0f, vz - 4.0f);
    }
    roller_prev_pointer = down;
}

static const EngBuiltinScript builtins[] = {
    { "spin", NULL, spin_process },
    { "orbit", orbit_ready, orbit_process },
    { "roller", roller_ready, roller_process }
};

const EngBuiltinScript *eng_script_builtin(const char *name) {
    if (!name || !name[0]) return NULL;
    for (unsigned i = 0; i < sizeof(builtins) / sizeof(builtins[0]); i++)
        if (!strcmp(builtins[i].name, name)) return &builtins[i];
    return NULL;
}
