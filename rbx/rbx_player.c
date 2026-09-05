/* Ходьба/прыжок по вокселям и отдельно включаемый полёт от первого лица. */
#include "rbx_internal.h"
#include <math.h>
#include <string.h>

#define WALK_SPEED 4.6f
#define FLY_SPEED 8.0f
#define GRAVITY 22.0f
#define JUMP_SPEED 7.5f
#define PITCH_LIMIT 1.45f
#define TAU 6.28318530718f

static float px, py, pz, pvy, cyaw, cpitch, walk;
static int flying, grounded, touch_jump, jump_latch;
static int k_w, k_a, k_s, k_d, k_space, k_sink, k_f;
static int k_left, k_right, k_up, k_down;

void rbx_player_spawn(void) {
    px = 8.5f; pz = 8.5f;
    py = rbx_terrain_height(8, 8) + 1.002f;
    pvy = walk = 0; flying = 0; grounded = 1; touch_jump = jump_latch = 0;
    cyaw = .7f; cpitch = -.16f;
}
void rbx_player_pos(float *x, float *y, float *z, float *yaw, float *phase) {
    if (x) *x = px;
    if (y) *y = py;
    if (z) *z = pz;
    if (yaw) *yaw = cyaw;
    if (phase) *phase = walk;
}
int rbx_player_flying(void) { return flying; }
int rbx_player_grounded(void) { return grounded; }
void rbx_player_toggle_flight(void) {
    flying = !flying;
    pvy = 0;
    grounded = 0;
    touch_jump = 0;
    jump_latch = k_space; /* удержание прыжка не превращается в новый прыжок */
}
void rbx_player_jump(int down) {
    touch_jump = down && !flying;
    if (!touch_jump && !k_space) jump_latch = 0;
}
void rbx_camera_angles(float *yaw, float *pitch) {
    if (yaw) *yaw = cyaw;
    if (pitch) *pitch = cpitch;
}
static void clamp_angles(void) {
    cyaw = remainderf(cyaw, TAU);
    cpitch = fmaxf(-PITCH_LIMIT, fminf(PITCH_LIMIT, cpitch));
}
void rbx_camera_look(float dx, float dy) {
    if (screen_w <= 0 || screen_h <= 0 || !isfinite(dx + dy)) return;
    float size = fminf((float)screen_w, (float)screen_h);
    cyaw += dx / size * 2.7f;
    cpitch -= dy / size * 2.4f;
    clamp_angles();
}
void rbx_key_reset(void) {
    k_w = k_a = k_s = k_d = k_space = k_sink = k_f = 0;
    k_left = k_right = k_up = k_down = jump_latch = 0;
}
void rbx_key_state(const char *name, int down) {
    if (!name) return;
    int d = down != 0;
    if (!strcmp(name,"w") || !strcmp(name,"W")) k_w = d;
    else if (!strcmp(name,"a") || !strcmp(name,"A")) k_a = d;
    else if (!strcmp(name,"s") || !strcmp(name,"S")) k_s = d;
    else if (!strcmp(name,"d") || !strcmp(name,"D")) k_d = d;
    else if (!strcmp(name,"space") || !strcmp(name," ")) { k_space = d; if (!d && !touch_jump) jump_latch = 0; }
    else if (!strcmp(name,"Shift")) k_sink = d;
    else if (!strcmp(name,"ArrowLeft")) k_left = d;
    else if (!strcmp(name,"ArrowRight")) k_right = d;
    else if (!strcmp(name,"ArrowUp")) k_up = d;
    else if (!strcmp(name,"ArrowDown")) k_down = d;
    else if (!strcmp(name,"f") || !strcmp(name,"F")) { if (d && !k_f) rbx_player_toggle_flight(); k_f = d; }
}
static void move_axis(int axis, float delta) {
    if (delta == 0) return;
    float *pos = axis == 0 ? &px : axis == 1 ? &py : &pz;
    *pos += delta;
    const float r = RBX_PLAYER_RADIUS, h = RBX_PLAYER_HEIGHT, eps = .0001f;
    int x0 = (int)floorf(px-r+eps), x1 = (int)floorf(px+r-eps);
    int y0 = (int)floorf(py+eps), y1 = (int)floorf(py+h-eps);
    int z0 = (int)floorf(pz-r+eps), z1 = (int)floorf(pz+r-eps);
    for (int y = y0; y <= y1; y++) for (int z = z0; z <= z1; z++) for (int x = x0; x <= x1; x++) {
        if (!rbx_world_solid(x,y,z)) continue;
        if (px+r <= x+eps || px-r >= x+1-eps || py+h <= y+eps || py >= y+1-eps ||
            pz+r <= z+eps || pz-r >= z+1-eps) continue;
        if (axis == 0) px = delta > 0 ? x-r : x+1+r;
        else if (axis == 2) pz = delta > 0 ? z-r : z+1+r;
        else {
            py = delta > 0 ? y-h : y+1;
            if (delta < 0) grounded = 1;
            pvy = 0;
        }
    }
}
void rbx_player_update(float d) {
    if (!isfinite(d) || d <= 0) return;
    if (d > .05f) d = .05f;
    cyaw += (k_right-k_left)*1.7f*d; cpitch += (k_up-k_down)*1.4f*d;
    clamp_angles();
    float jx, jy;
    rbx_input_joy(&jx,&jy);
    float side = jx+k_d-k_a, forward = -jy+k_w-k_s;
    float cp = flying ? cosf(cpitch) : 1;
    float vx = sinf(cyaw)*cp*forward + cosf(cyaw)*side;
    float vz = cosf(cyaw)*cp*forward - sinf(cyaw)*side;
    float vy = flying ? sinf(cpitch)*forward + k_space-k_sink : 0;
    float length = sqrtf(vx*vx + vy*vy + vz*vz);
    if (length > 1) { vx/=length; vy/=length; vz/=length; length=1; }
    int water = rbx_world_block((int)floorf(px), (int)floorf(py+.6f), (int)floorf(pz)) == BLOCK_WATER;
    float speed = flying ? FLY_SPEED : water ? 2.8f : WALK_SPEED;
    vx *= speed; vz *= speed;
    if (flying) pvy = vy*speed;
    else {
        int jump = k_space || touch_jump;
        if (jump && !jump_latch && grounded) { pvy=JUMP_SPEED; grounded=0; snd_play("send.wav"); }
        jump_latch = jump;
        pvy -= (water ? 6 : GRAVITY)*d;
        if (water && jump) pvy = 4.5f; /* плавание, не полёт над водой */
        float terminal = water ? -2.5f : -24;
        if (pvy < terminal) pvy = terminal;
    }
    float fastest = fmaxf(fabsf(pvy), fmaxf(fabsf(vx),fabsf(vz)));
    int steps = (int)ceilf(fastest*d/.20f);
    if (steps < 1) steps = 1;
    float step = d/steps;
    grounded = 0;
    for (int i=0; i<steps; i++) {
        move_axis(0,vx*step); move_axis(2,vz*step); move_axis(1,pvy*step);
    }
    walk += d*8*length;
    if (py < -8 || !isfinite(px+py+pz)) { rbx_player_spawn(); rbx_cancel_input(); }
}
