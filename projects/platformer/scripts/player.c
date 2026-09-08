/* Platformer player controller for a CharacterBody3D.
 *
 * Polls the engine input API, integrates gravity into the character's
 * velocity and calls eng_character_move_and_slide once a frame. Run with
 * A/D or the arrow keys, jump with Space (coyote time + jump buffer + a
 * shorter hop when Space is tapped). Touching a coin under Main/Coins frees
 * it; reaching the Goal — or falling below y = -12 — returns to the spawn.
 */
#include "eng_api.h"

#define MOVE_SPEED  6.0f    /* top horizontal speed, world units / s */
#define ACCEL       70.0f   /* how fast the speed approaches the input target */
#define GRAVITY     22.0f
#define JUMP_SPEED  9.5f
#define MAX_FALL    24.0f   /* terminal velocity */
#define COYOTE      0.12f   /* seconds after leaving a ledge you can still jump */
#define JUMP_BUFFER 0.12f   /* seconds an early Space press stays valid */

static int prev_space;

void eng_script_ready(EngNode *self) {
    float x, y, z;
    eng_node3d_get_position(self, &x, &y, &z);
    eng_node_udata_set(self, 0, x);   /* spawn point */
    eng_node_udata_set(self, 1, y);
    eng_node_udata_set(self, 2, z);
    eng_node_udata_set(self, 3, 0);   /* coyote timer */
    eng_node_udata_set(self, 4, 0);   /* jump buffer */
    prev_space = 0;
}

static void respawn(EngNode *self) {
    eng_node3d_set_position(self, eng_node_udata_get(self, 0),
                            eng_node_udata_get(self, 1),
                            eng_node_udata_get(self, 2));
    eng_character_set_velocity(self, 0, 0, 0);
    eng_node_udata_set(self, 3, 0);
    eng_node_udata_set(self, 4, 0);
}

void eng_script_process(EngNode *self, float dt) {
    /* horizontal input: A/D and the arrow keys both work */
    float move = 0;
    if (eng_input_is_pressed(ENG_KEY_A))     move -= 1;
    if (eng_input_is_pressed(ENG_KEY_D))     move += 1;
    if (eng_input_is_pressed(ENG_KEY_LEFT))  move -= 1;
    if (eng_input_is_pressed(ENG_KEY_RIGHT)) move += 1;

    float vx, vy, vz;
    eng_character_get_velocity(self, &vx, &vy, &vz);

    /* smooth horizontal acceleration toward the input target */
    float target = move * MOVE_SPEED;
    float rate = ACCEL * dt;
    if (vx < target) vx = vx + rate < target ? vx + rate : target;
    else if (vx > target) vx = vx - rate > target ? vx - rate : target;

    /* coyote time: briefly keep the jump after walking off a ledge */
    float coyote = eng_node_udata_get(self, 3);
    if (eng_character_is_grounded(self)) coyote = COYOTE;
    else if (coyote > 0) coyote -= dt;
    eng_node_udata_set(self, 3, coyote);

    /* jump buffer: pressing Space a hair before landing still jumps */
    float buffer = eng_node_udata_get(self, 4);
    int space = eng_input_is_pressed(ENG_KEY_SPACE);
    if (space && !prev_space) buffer = JUMP_BUFFER;
    else if (buffer > 0) buffer -= dt;
    prev_space = space;

    if (buffer > 0 && coyote > 0) {
        vy = JUMP_SPEED;
        coyote = 0;
        buffer = 0;
        eng_node_udata_set(self, 3, coyote);
        eng_play_sound("jump.wav");
    }
    eng_node_udata_set(self, 4, buffer);

    /* gravity; releasing Space early cuts the rise for a shorter hop */
    vy -= GRAVITY * dt;
    if (vy < -MAX_FALL) vy = -MAX_FALL;
    if (vy > 0 && !space) vy -= GRAVITY * 1.5f * dt;

    eng_character_set_velocity(self, vx, vy, vz);
    eng_character_move_and_slide(self, dt);

    /* collect every coin under Main/Coins the Hero touches */
    EngNode *coins = eng_node_find("Main/Coins");
    if (coins) {
        float px, py, pz;
        eng_node3d_get_position(self, &px, &py, &pz);
        EngNode *coin = eng_node_first_child(coins);
        while (coin) {
            EngNode *next = eng_node_next_sibling(coin);
            float cx, cy, cz;
            eng_node3d_get_position(coin, &cx, &cy, &cz);
            float dx = px - cx, dy = py - cy, dz = pz - cz;
            if (dx * dx + dy * dy + dz * dz < 1.4f * 1.4f) {
                eng_node_free(coin);
                eng_play_sound("break.wav");
                eng_print("coin collected!");
            }
            coin = next;
        }
    }

    /* goal reached or fell off the world: back to the start */
    float px, py, pz;
    eng_node3d_get_position(self, &px, &py, &pz);
    EngNode *goal = eng_node_find("Main/Goal");
    int at_goal = 0;
    if (goal) {
        float gx, gy, gz;
        eng_node3d_get_position(goal, &gx, &gy, &gz);
        float dx = px - gx, dy = py - gy, dz = pz - gz;
        if (dx * dx + dy * dy + dz * dz < 2.4f * 2.4f) at_goal = 1;
    }
    if (at_goal) {
        eng_print("GOAL! back to the start.");
        respawn(self);
    } else if (py < -12.0f) {
        eng_print("fell off — respawn.");
        respawn(self);
    }
}
