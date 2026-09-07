/* Small fixed-pool particle system: dust, coin sparkles, bursts, lava. */
#include "platformium_internal.h"

static PmParticle pool[PM_MAX_PARTICLES];
static int next_slot;

void pm_particles_clear(void) {
    for (int i = 0; i < PM_MAX_PARTICLES; i++) pool[i].life = 0;
    next_slot = 0;
}

static void spawn(float x, float y, float vx, float vy, float life, float size,
                  uint32_t color, int gravity) {
    PmParticle *p = &pool[next_slot];
    next_slot = (next_slot + 1) % PM_MAX_PARTICLES;
    p->x = x; p->y = y; p->vx = vx; p->vy = vy;
    p->life = p->max_life = life;
    p->size = size; p->color = color; p->gravity = gravity;
}

static float frand(float lo, float hi) {
    return lo + (hi - lo) * ((float)(rand() & 0xfff) / 4096.0f);
}

void pm_particles_burst(float x, float y, int count, int kind) {
    for (int i = 0; i < count; i++) {
        switch (kind) {
            case PM_FX_DUST:
                spawn(x + frand(-.3f, .3f), y + frand(-.1f, .1f),
                      frand(-1.5f, 1.5f), frand(-1.8f, -.2f),
                      frand(.25f, .5f), frand(.10f, .22f), 0x998d7a63u, 1);
                break;
            case PM_FX_SPARK:
                spawn(x, y, frand(-2.5f, 2.5f), frand(-4.5f, -.5f),
                      frand(.3f, .6f), frand(.08f, .16f), 0xffe9b23au, 1);
                break;
            case PM_FX_BURST:
                spawn(x, y, frand(-4.5f, 4.5f), frand(-6.0f, 1.5f),
                      frand(.35f, .7f), frand(.12f, .26f),
                      (rand() & 1) ? 0xfff26f61u : 0xffc94f7cu, 1);
                break;
            case PM_FX_LAVA:
                spawn(x, y, frand(-.6f, .6f), frand(-3.5f, -1.2f),
                      frand(.4f, .8f), frand(.10f, .20f),
                      (rand() & 1) ? 0xffffa53bu : 0xffff5f2eu, 1);
                break;
            default: break;
        }
    }
}

void pm_particles_update(float d) {
    for (int i = 0; i < PM_MAX_PARTICLES; i++) {
        PmParticle *p = &pool[i];
        if (p->life <= 0) continue;
        p->life -= d;
        if (p->gravity) p->vy += 18.0f * d;
        p->x += p->vx * d;
        p->y += p->vy * d;
    }
}

void pm_particles_draw(void) {
    float tpx = pm_tile_px();
    for (int i = 0; i < PM_MAX_PARTICLES; i++) {
        PmParticle *p = &pool[i];
        if (p->life <= 0) continue;
        float sx, sy;
        pm_world_to_screen(p->x, p->y, &sx, &sy);
        float fade = p->life / p->max_life;
        float r = p->size * tpx * (.4f + .6f * fade);
        uint32_t c = p->color;
        int alpha = (int)(255 * fade) * (int)((c >> 24) & 0xff) / 255;
        c = (c & 0x00ffffffu) | ((uint32_t)alpha << 24);
        circle(sx, sy, r, c);
    }
}
