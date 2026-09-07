/* The character controller: AABB movement against the tile grid with
 * coyote time, jump buffering, variable jump height, springs, one-way
 * platforms and lift rides; plus hazards, coins, checkpoints and the goal. */
#include "platformium_internal.h"

static PmPlayer player;
static int jump_cut_done;
static float spring_fx_time = -10, spring_fx_x, spring_fx_y;
float pm_respawn_x, pm_respawn_y;     /* checkpoint (game.c respawns here) */
int pm_checkpoint_active;

const PmPlayer *pm_player_state(void) { return &player; }
PmPlayer *pm_player_mut(void) { return &player; }

void pm_player_spawn(int tx, int ty) {
    memset(&player, 0, sizeof(player));
    player.x = tx + .5f;
    player.y = ty + .5f;
    player.dir = 1;
    player.alive = 1;
    jump_cut_done = 0;
    pm_respawn_x = player.x;
    pm_respawn_y = player.y;
    pm_checkpoint_active = 0;
    spring_fx_time = -10;
}

void pm_player_revive(float x, float y) {
    player.x = x;
    player.y = y;
    player.vx = player.vy = 0;
    player.alive = 1;
    player.grounded = 0;
    player.squash = 0;
    player.invuln = 1.6f;
    jump_cut_done = 1;
    pm_camera_reset(x, y);
}

void pm_player_pos(float *x, float *y) {
    if (x) *x = player.x;
    if (y) *y = player.y;
}

void pm_player_shift(float dx, float dy) {
    player.x += dx;
    player.y += dy;
}

int pm_player_rides(const PmLift *lift) {
    if (!lift || !player.grounded) return 0;
    float top = lift->y - lift->h * .5f;
    float bottom = player.y + PM_PLAYER_HH;
    if (fabsf(bottom - top) > .18f) return 0;
    return fabsf(player.x - lift->x) < lift->w * .5f + PM_PLAYER_HW;
}

/* Does any solid tile block row `ty` under a box centered at x? One-way
 * tiles only catch a box whose previous bottom stayed above the tile top. */
static int row_blocked(float x, int ty, float prev_bottom) {
    int x0 = (int)floorf(x - PM_PLAYER_HW), x1 = (int)floorf(x + PM_PLAYER_HW);
    for (int tx = x0; tx <= x1; tx++) {
        int t = pm_tile(tx, ty);
        if (t == PM_SOLID || t == PM_SPRING) return 1;
        if (t == PM_ONEWAY && prev_bottom <= (float)ty + .001f) return 1;
    }
    return 0;
}

static void move_x(float dx) {
    if (dx == 0) return;
    float nx = player.x + dx;
    if (pm_box_hits(nx, player.y, PM_PLAYER_HW, PM_PLAYER_HH, 0, 0)) {
        if (dx > 0) nx = floorf(nx + PM_PLAYER_HW) - PM_PLAYER_HW - .001f;
        else nx = floorf(nx - PM_PLAYER_HW) + 1 + PM_PLAYER_HW + .001f;
        player.vx = 0;
    }
    player.x = nx;
}

/* Returns 1 when the vertical move landed on something (tile or handled by
 * the caller for lifts); oneway tiles only catch from above. */
static int move_y(float dy) {
    if (dy == 0) return 0;
    float prev_bottom = player.y + PM_PLAYER_HH;
    float ny = player.y + dy;
    if (dy > 0) {
        int t0 = (int)floorf(prev_bottom), t1 = (int)floorf(ny + PM_PLAYER_HH);
        int landed = -1;
        for (int ty = t0; ty <= t1; ty++) {
            if (row_blocked(player.x, ty, prev_bottom)) { landed = ty; break; }
        }
        if (landed >= 0) {
            float hard_land = player.vy;
            ny = (float)landed - PM_PLAYER_HH - .001f;
            player.vy = 0;
            player.grounded = 1;
            /* spring tiles bounce immediately */
            int x0 = (int)floorf(player.x - PM_PLAYER_HW), x1 = (int)floorf(player.x + PM_PLAYER_HW);
            for (int tx = x0; tx <= x1; tx++) {
                if (pm_tile(tx, landed) == PM_SPRING) {
                    player.vy = -PM_SPRING_VEL;
                    player.grounded = 0;
                    jump_cut_done = 1;      /* full spring height either way */
                    spring_fx_time = (float)pm_time;
                    spring_fx_x = tx + .5f;
                    spring_fx_y = landed + .5f;
                    pm_particles_burst(tx + .5f, landed, 8, PM_FX_DUST);
                    pm_camera_shake(.18f);
                    snd_play("jump.wav");
                    break;
                }
            }
            if (player.grounded && hard_land > 9.0f) {
                pm_particles_burst(player.x, player.y + PM_PLAYER_HH, 6, PM_FX_DUST);
                player.squash = .22f;
            }
            player.y = ny;
            return 1;
        }
        player.grounded = 0;
    } else {
        if (pm_box_hits(player.x, ny, PM_PLAYER_HW, PM_PLAYER_HH, 0, 0)) {
            ny = floorf(ny - PM_PLAYER_HH) + 1 + PM_PLAYER_HH + .001f;
            player.vy = 0;
        }
    }
    player.y = ny;
    return 0;
}

/* Landing on moving lifts: catch a falling box whose previous bottom stayed
 * above the lift top. */
static void lift_catch(float prev_bottom, float ny_bottom) {
    extern const PmLift *pm_lift_list(int *count);
    int count;
    const PmLift *lifts = pm_lift_list(&count);
    for (int i = 0; i < count; i++) {
        const PmLift *lift = &lifts[i];
        float top = lift->y - lift->h * .5f;
        if (player.x + PM_PLAYER_HW < lift->x - lift->w * .5f) continue;
        if (player.x - PM_PLAYER_HW > lift->x + lift->w * .5f) continue;
        if (prev_bottom <= top + .05f && ny_bottom >= top && player.vy >= 0) {
            player.y = top - PM_PLAYER_HH - .001f;
            player.vy = 0;
            player.grounded = 1;
        }
    }
}

void pm_player_kill(void);

static void die_if_hazard(void) {
    if (player.invuln > 0) return;
    PmLevelDef *level = pm_level_live();
    float left = player.x - PM_PLAYER_HW, right = player.x + PM_PLAYER_HW;
    float top = player.y - PM_PLAYER_HH, bottom = player.y + PM_PLAYER_HH;
    int x0 = (int)floorf(left), x1 = (int)floorf(right);
    int y0 = (int)floorf(top), y1 = (int)floorf(bottom);
    for (int ty = y0; ty <= y1; ty++) {
        for (int tx = x0; tx <= x1; tx++) {
            int t = pm_tile(tx, ty);
            if (t == PM_SPIKE) {
                /* forgiving hitbox: only the spiked lower half of the tile */
                if (bottom > ty + .45f && right > tx + .18f && left < tx + .82f) {
                    pm_player_kill();
                    return;
                }
            } else if (t == PM_LAVA) {
                if (bottom > ty + .35f) {
                    pm_particles_burst(player.x, ty + .3f, 14, PM_FX_LAVA);
                    pm_player_kill();
                    return;
                }
            }
        }
    }
    if (level && player.y > level->h + 2.5f) pm_player_kill();
}

static void collect_tiles(void) {
    PmLevelDef *level = pm_level_live();
    if (!level) return;
    float left = player.x - PM_PLAYER_HW, right = player.x + PM_PLAYER_HW;
    float top = player.y - PM_PLAYER_HH, bottom = player.y + PM_PLAYER_HH;
    int x0 = (int)floorf(left), x1 = (int)floorf(right);
    int y0 = (int)floorf(top), y1 = (int)floorf(bottom);
    for (int ty = y0; ty <= y1; ty++) {
        for (int tx = x0; tx <= x1; tx++) {
            int t = pm_tile(tx, ty);
            if (t == PM_COIN && !pm_tile_taken(tx, ty)) {
                pm_tile_take(tx, ty);
                pm_score_add(50);
                pm_coins++;
                pm_particles_burst(tx + .5f, ty + .5f, 8, PM_FX_SPARK);
                snd_play("place.wav");
            } else if (t == PM_CHECK && !pm_tile_taken(tx, ty)) {
                pm_tile_take(tx, ty);
                pm_checkpoint_active = 1;
                pm_respawn_x = tx + .5f;
                pm_respawn_y = ty + .5f;
                pm_particles_burst(tx + .5f, ty, 12, PM_FX_SPARK);
                snd_play("place.wav");
            } else if (t == PM_GOAL) {
                extern void pm_level_clear(void);
                pm_level_clear();
                return;
            }
        }
    }
}

void pm_player_update(float d) {
    if (!player.alive || d <= 0) return;
    if (player.invuln > 0) player.invuln -= d;
    player.anim += d * (1.0f + fabsf(player.vx) * .35f);
    if (player.squash > 0) player.squash = fmaxf(0, player.squash - 1.6f * d);

    float move = pm_input_move();
    float accel = player.grounded ? PM_RUN_ACCEL : PM_AIR_ACCEL;
    if (move != 0) {
        player.vx += move * accel * d;
        if (player.vx > PM_RUN_SPEED) player.vx = PM_RUN_SPEED;
        if (player.vx < -PM_RUN_SPEED) player.vx = -PM_RUN_SPEED;
        player.dir = move > 0 ? 1 : -1;
        if (player.grounded && fabsf(player.vx) > 4.5f && (rand() & 31) == 0)
            pm_particles_burst(player.x - player.dir * .3f, player.y + PM_PLAYER_HH, 1, PM_FX_DUST);
    } else {
        float fr = (player.grounded ? PM_FRICTION : PM_AIR_DRAG) * d;
        if (player.vx > fr) player.vx -= fr;
        else if (player.vx < -fr) player.vx += fr;
        else player.vx = 0;
    }

    if (player.grounded) player.coyote = PM_COYOTE_TIME;
    else player.coyote -= d;
    if (pm_input_jump_take_press()) player.buffer = PM_JUMP_BUFFER;
    else player.buffer -= d;
    if (player.buffer > 0 && player.coyote > 0) {
        player.vy = -PM_JUMP_VEL;
        player.coyote = 0;
        player.buffer = 0;
        player.grounded = 0;
        jump_cut_done = 0;
        player.squash = -.18f;   /* stretch */
        pm_particles_burst(player.x, player.y + PM_PLAYER_HH, 4, PM_FX_DUST);
        snd_play("jump.wav");
    }
    if (!jump_cut_done && !pm_input_jump_held() && player.vy < -2.0f) {
        player.vy *= PM_JUMP_CUT;
        jump_cut_done = 1;
    }

    player.vy += PM_GRAVITY * d;
    if (player.vy > PM_MAX_FALL) player.vy = PM_MAX_FALL;

    /* substep so fast falls never tunnel through a tile */
    float speed = fmaxf(fabsf(player.vx), fabsf(player.vy));
    int steps = (int)ceilf(speed * d / .4f);
    if (steps < 1) steps = 1;
    if (steps > 6) steps = 6;
    float sd = d / (float)steps;
    int was_grounded = player.grounded;
    player.grounded = 0;
    for (int s = 0; s < steps; s++) {
        float prev_bottom = player.y + PM_PLAYER_HH;
        move_x(player.vx * sd);
        move_y(player.vy * sd);
        if (!player.grounded && player.vy >= 0) lift_catch(prev_bottom, player.y + PM_PLAYER_HH);
    }
    if (!was_grounded && player.grounded && player.squash <= 0) player.squash = .16f;

    collect_tiles();
    die_if_hazard();
}

void pm_player_kill(void) {
    extern void pm_player_died(void);
    if (!player.alive) return;
    player.alive = 0;
    player.vy = -13.0f;
    pm_camera_shake(.5f);
    pm_particles_burst(player.x, player.y, 16, PM_FX_BURST);
    snd_play("break.wav");
    pm_player_died();
}

void pm_player_draw(void) {
    if (!player.alive && pm_state != PM_STATE_DYING) return;
    if (player.invuln > 0 && fmod(pm_time, .24) < .12 && pm_state == PM_STATE_PLAY) return;
    float sx, sy, tpx = pm_tile_px();
    pm_world_to_screen(player.x, player.y, &sx, &sy);
    if (pm_state == PM_STATE_DYING) {
        /* little knock-up arc while the death panel counts in */
        float t = pm_state_time * 2.2f;
        sy -= sinf(t < 3.14159f ? t : 3.14159f) * tpx * 1.6f;
    }
    /* squash & stretch around the feet */
    float sq = player.squash;
    float w = PM_PLAYER_HW * 2 * tpx * (1 + sq * .9f);
    float h = PM_PLAYER_HH * 2 * tpx * (1 - sq);
    float feet = sy + PM_PLAYER_HH * tpx;
    float top = feet - h;
    /* feet: a small running cycle */
    float step = sinf(player.anim * 9.0f) * (fabsf(player.vx) > .5f ? .16f : 0);
    float fw = .16f * tpx;
    rect(sx - .18f * tpx * player.dir + step * tpx * player.dir - fw * .5f,
         feet - .10f * tpx, fw, .12f * tpx, 0xff17303bu);
    rect(sx + .18f * tpx * player.dir - step * tpx * player.dir - fw * .5f,
         feet - .10f * tpx, fw, .12f * tpx, 0xff17303bu);
    /* body */
    roundrect(sx - w * .5f, top, w, h, w * .32f, 0xff2ee6a8u);
    roundrect(sx - w * .5f, top, w, h * .45f, w * .32f, 0xff62f2c4u);
    /* antenna */
    line(sx, top, sx + player.dir * .10f * tpx, top - .22f * tpx, .06f * tpx, 0xff17303bu);
    circle(sx + player.dir * .10f * tpx, top - .26f * tpx, .09f * tpx, 0xffffd25eu);
    /* eyes look where the player runs */
    float ex = sx + player.dir * .10f * tpx, ey = top + h * .38f;
    circle(ex - .10f * tpx, ey, .10f * tpx, 0xffffffffu);
    circle(ex + .14f * tpx, ey, .10f * tpx, 0xffffffffu);
    circle(ex - .10f * tpx + player.dir * .03f * tpx, ey, .045f * tpx, 0xff17303bu);
    circle(ex + .14f * tpx + player.dir * .03f * tpx, ey, .045f * tpx, 0xff17303bu);
}

/* spring squash animation hook for the renderer */
void pm_spring_fx(float *x, float *y, float *amount) {
    float t = (float)pm_time - spring_fx_time;
    if (t < 0 || t > .35f) { if (amount) *amount = 0; return; }
    if (x) *x = spring_fx_x;
    if (y) *y = spring_fx_y;
    if (amount) *amount = 1.0f - t / .35f;
}
