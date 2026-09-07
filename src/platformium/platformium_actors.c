/* Enemies (patrolling walkers, bobbing flyers) and moving lifts.
 * Walkers turn around at walls and ledges and stay close to their spawn;
 * lifts patrol a fixed segment and carry the player while she rides. */
#include "platformium_internal.h"

#define PM_ACTOR_HW .38f
#define PM_ACTOR_HH .36f
#define PM_LIFT_RANGE 3.0f
#define PM_LIFT_PERIOD 4.6f
#define PM_LIFT_VRANGE 2.5f

static PmEnemy enemies[PM_MAX_ENEMIES];
static int enemy_count;
static PmLift lifts[PM_MAX_PLATFORMS];
static int lift_count;

const PmLift *pm_lift_list(int *count) {
    if (count) *count = lift_count;
    return lifts;
}

void pm_actors_reset(void) {
    PmLevelDef *level = pm_level_live();
    enemy_count = lift_count = 0;
    if (!level) return;
    for (int i = 0; i < level->enemy_count && enemy_count < PM_MAX_ENEMIES; i++) {
        PmEnemy *e = &enemies[enemy_count++];
        memset(e, 0, sizeof(*e));
        e->kind = level->enemies[i].kind;
        e->x = level->enemies[i].tx + .5f;
        e->y = level->enemies[i].ty + .5f;
        e->px = e->x; e->py = e->y;
        e->dir = 1;
        e->alive = 1;
        /* walkers keep px as their patrol anchor, flyers keep py as the
         * bobbing center */
        if (e->kind == PM_ENEMY_FLYER) e->t = (float)(i % 5) * .7f;
    }
    for (int i = 0; i < level->lift_count && lift_count < PM_MAX_PLATFORMS; i++) {
        PmLift *l = &lifts[lift_count++];
        memset(l, 0, sizeof(*l));
        l->x = l->px = level->lifts[i].tx + .5f;
        l->y = l->py = level->lifts[i].ty + .5f;
        l->w = 2.2f;
        l->h = .45f;
        /* start at a patrol extreme so lifts begin nearly still */
        l->t = PM_LIFT_PERIOD * .25f + (float)(i % 3) * (PM_LIFT_PERIOD / 3.0f);
        l->period = PM_LIFT_PERIOD;
        if (level->lifts[i].vertical) {
            l->ax = l->bx = l->x;
            l->ay = l->y - PM_LIFT_VRANGE;
            l->by = l->y + PM_LIFT_VRANGE;
        } else {
            l->ay = l->by = l->y;
            l->ax = l->x - PM_LIFT_RANGE;
            l->bx = l->x + PM_LIFT_RANGE;
        }
    }
}

static int walker_ground_ahead(const PmEnemy *e) {
    float fx = e->x + e->dir * (PM_ACTOR_HW + .12f);
    int tx = (int)floorf(fx);
    int ty = (int)floorf(e->y + PM_ACTOR_HH + .12f);
    return pm_tile_solid(tx, ty);
}

static int walker_wall_ahead(const PmEnemy *e) {
    float fx = e->x + e->dir * (PM_ACTOR_HW + .12f);
    int tx = (int)floorf(fx);
    int ty = (int)floorf(e->y);
    return pm_tile_solid(tx, ty);
}

static void walker_update(PmEnemy *e, float d, float spawn_x) {
    const float speed = 1.2f;
    int blocked = walker_wall_ahead(e) || !walker_ground_ahead(e);
    int out_of_range = (e->x - spawn_x) * e->dir > .8f;
    if (blocked || out_of_range) e->dir = -e->dir;
    e->x += e->dir * speed * d;
    /* snap feet to the ground row */
    PmLevelDef *level = pm_level_live();
    int max_ty = level ? level->h : PM_MAX_H;
    int ty = (int)floorf(e->y + PM_ACTOR_HH + .5f);
    while (ty < max_ty && !pm_tile_solid((int)floorf(e->x), ty)) ty++;
    e->y = (float)ty - PM_ACTOR_HH - .001f;
}

static void flyer_update(PmEnemy *e, float d) {
    e->t += d;
    e->y = e->py + sinf(e->t * 2.6f) * 1.4f;
}

void pm_actors_update(float d) {
    PmLevelDef *level = pm_level_live();
    if (!level || d <= 0) return;

    /* lifts first so the player can ride them this frame */
    for (int i = 0; i < lift_count; i++) {
        PmLift *l = &lifts[i];
        l->px = l->x; l->py = l->y;
        l->t += d;
        float phase = sinf(l->t * 6.2831853f / l->period);
        if (l->ax != l->bx) {
            float mid = (l->ax + l->bx) * .5f, amp = (l->bx - l->ax) * .5f;
            l->x = mid + phase * amp;
        }
        if (l->ay != l->by) {
            float mid = (l->ay + l->by) * .5f, amp = (l->by - l->ay) * .5f;
            l->y = mid + phase * amp;
        }
        if (pm_player_rides(l)) pm_player_shift(l->x - l->px, l->y - l->py);
    }

    const PmPlayer *player = pm_player_state();
    for (int i = 0; i < enemy_count; i++) {
        PmEnemy *e = &enemies[i];
        if (!e->alive) continue;
        if (e->kind == PM_ENEMY_WALKER) walker_update(e, d, e->px);
        else flyer_update(e, d);
        /* touch the player: stomp from above, otherwise hurt */
        if (!player->alive || pm_state != PM_STATE_PLAY) continue;
        float px, py;
        pm_player_pos(&px, &py);
        float ox = PM_ACTOR_HW + PM_PLAYER_HW, oy = PM_ACTOR_HH + PM_PLAYER_HH;
        if (fabsf(px - e->x) < ox && fabsf(py - e->y) < oy) {
            extern PmPlayer *pm_player_mut(void);
            PmPlayer *mut = pm_player_mut();
            if (mut->vy > .5f && py < e->y - .05f) {
                e->alive = 0;
                mut->vy = pm_input_jump_held() ? -PM_STOMP_BOUNCE_HELD : -PM_STOMP_BOUNCE;
                mut->grounded = 0;
                pm_score_add(100);
                pm_camera_shake(.25f);
                pm_particles_burst(e->x, e->y, 12, PM_FX_BURST);
                snd_play("break.wav");
            } else if (mut->invuln <= 0) {
                pm_player_kill();
            }
        }
    }
}

void pm_actors_draw(void) {
    float tpx = pm_tile_px();
    for (int i = 0; i < lift_count; i++) {
        PmLift *l = &lifts[i];
        float sx, sy;
        pm_world_to_screen(l->x, l->y, &sx, &sy);
        float w = l->w * tpx, h = l->h * tpx;
        roundrect(sx - w * .5f, sy - h * .5f, w, h, h * .4f, 0xff8a5a33u);
        rect(sx - w * .5f, sy - h * .5f, w, h * .35f, 0xffb07948u);
        circle(sx - w * .32f, sy, h * .14f, 0xff3c2716u);
        circle(sx + w * .32f, sy, h * .14f, 0xff3c2716u);
    }
    for (int i = 0; i < enemy_count; i++) {
        PmEnemy *e = &enemies[i];
        if (!e->alive) continue;
        float sx, sy;
        pm_world_to_screen(e->x, e->y, &sx, &sy);
        if (sx < -2 * tpx || sx > screen_w + 2 * tpx || sy < -2 * tpx || sy > screen_h + 2 * tpx) continue;
        if (e->kind == PM_ENEMY_WALKER) {
            float w = PM_ACTOR_HW * 2 * tpx, h = PM_ACTOR_HH * 2 * tpx;
            float waddle = sinf(pm_time * 10 + i) * .06f * tpx;
            roundrect(sx - w * .5f, sy - h * .5f + waddle * .3f, w, h, w * .35f, 0xffc94f7cu);
            roundrect(sx - w * .5f, sy + h * .1f, w, h * .4f, w * .3f, 0xff9c3560u);
            /* angry eyes facing the walking direction */
            float ex = sx + e->dir * .08f * tpx;
            circle(ex - .09f * tpx, sy - .08f * tpx, .09f * tpx, 0xffffffffu);
            circle(ex + .13f * tpx, sy - .08f * tpx, .09f * tpx, 0xffffffffu);
            circle(ex - .09f * tpx + e->dir * .03f * tpx, sy - .08f * tpx, .04f * tpx, 0xff241018u);
            circle(ex + .13f * tpx + e->dir * .03f * tpx, sy - .08f * tpx, .04f * tpx, 0xff241018u);
        } else {
            float flap = sinf(pm_time * 14 + i * 2) * .5f;
            float r = .34f * tpx;
            /* wings */
            line(sx - r, sy, sx - r * 2.1f, sy - r * flap, .10f * tpx, 0xffe6b34bu);
            line(sx + r, sy, sx + r * 2.1f, sy - r * flap, .10f * tpx, 0xffe6b34bu);
            circle(sx, sy, r, 0xfff26f61u);
            circle(sx, sy + r * .25f, r * .62f, 0xffd94f43u);
            circle(sx - .11f * tpx, sy - .06f * tpx, .08f * tpx, 0xffffffffu);
            circle(sx + .11f * tpx, sy - .06f * tpx, .08f * tpx, 0xffffffffu);
            circle(sx - .11f * tpx, sy - .04f * tpx, .035f * tpx, 0xff241018u);
            circle(sx + .11f * tpx, sy - .04f * tpx, .035f * tpx, 0xff241018u);
        }
    }
}

int pm_actors_count(void) { return enemy_count; }

int pm_actors_info(int i, float *x, float *y, int *kind, int *alive) {
    if (i < 0 || i >= enemy_count) return 0;
    if (x) *x = enemies[i].x;
    if (y) *y = enemies[i].y;
    if (kind) *kind = enemies[i].kind;
    if (alive) *alive = enemies[i].alive;
    return 1;
}

int pm_actors_alive(void) {
    int alive = 0;
    for (int i = 0; i < enemy_count; i++) alive += enemies[i].alive;
    return alive;
}
