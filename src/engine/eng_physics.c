/* Physics: StaticBody3D colliders and RigidBody3D dynamics for engine scenes.
 *
 * This is a compact, deterministic impulse-integrator tuned to a blocky voxel
 * aesthetic, not a general rigid-body solver. A RigidBody3D is a dynamic
 * sphere whose radius matches its unit sphere mesh (size * gscale / 2), so the
 * drawn ball and its physics body coincide. A StaticBody3D is a solid box by
 * default but can also be a sphere. Rigid bodies collide with static bodies
 * (the main way they are used); rigid/rigid contact is left out on purpose so
 * the solver stays predictable. Bodies are simulated in world space, so a body
 * must hang from the root or a plain container Node (no transform parent).
 *
 * The step applies gravity, integrates velocity, then resolves penetration
 * against static bodies, marking the body `grounded` when it rests on a
 * support. A still, grounded body falls asleep and is skipped until a script
 * wakes it with an impulse, keeping resting bodies rock-stable and cheap.
 */
#include "eng_internal.h"
#include <math.h>
#include <string.h>

#define ENG_GRAVITY 20.0f        /* world units / s^2 (snappy, blocky feel) */
#define ENG_SLEEP_EPS 1.5f       /* squared speed below which a grounded body rests */
#define ENG_SLEEP_FRAMES 6.0f
#define ENG_GROUND_DOT 0.6f

int eng_body_dynamic(const EngNode *n) { return n && n->type == ENG_RIGID_BODY; }

float eng_body_world_size(const EngNode *n) { return n->size * n->gscale; }
static float sphere_radius(const EngNode *n) { return 0.5f * n->size * n->gscale; }
static float box_half(const EngNode *n) { return 0.5f * n->size * n->gscale; }

/* A body is free only when its whole parent chain is a plain container at the
 * origin, so local `pos` == world `gpos`. */
static int body_free(const EngNode *n) {
    const EngNode *p = n->parent;
    if (!p) return 1;
    if (p->type != ENG_NODE) return 0;
    return p->gpos.x == 0 && p->gpos.y == 0 && p->gpos.z == 0 &&
           p->gscale == 1 && p->basis.m[0] == 1 && p->basis.m[4] == 1 && p->basis.m[8] == 1;
}

static void wake(EngNode *n) { if (n->type == ENG_RIGID_BODY) { n->sleep_t = 0; n->asleep = 0; } }

void eng_body_apply_impulse(EngNode *n, float x, float y, float z) {
    if (!eng_body_dynamic(n) || n->mass <= 0) return;
    n->vel[0] += x / n->mass;
    n->vel[1] += y / n->mass;
    n->vel[2] += z / n->mass;
    wake(n);
}
void eng_body_set_linear_velocity(EngNode *n, float x, float y, float z) {
    if (!eng_body_dynamic(n)) return;
    n->vel[0] = x; n->vel[1] = y; n->vel[2] = z;
    wake(n);
}
void eng_body_get_linear_velocity(EngNode *n, float *x, float *y, float *z) {
    if (x) *x = eng_body_dynamic(n) ? n->vel[0] : 0;
    if (y) *y = eng_body_dynamic(n) ? n->vel[1] : 0;
    if (z) *z = eng_body_dynamic(n) ? n->vel[2] : 0;
}
int eng_body_is_grounded(EngNode *n) {
    return n && n->type == ENG_RIGID_BODY && n->grounded;
}
void eng_body_set_mass(EngNode *n, float m) {
    if (eng_body_dynamic(n) && m > 0) n->mass = m;
}
void eng_body_set_gravity_scale(EngNode *n, float s) {
    if (n && (n->type == ENG_RIGID_BODY || n->type == ENG_STATIC_BODY)) n->gravity_scale = s;
}
void eng_body_set_shape(EngNode *n, const char *shape) {
    if (!n || (n->type != ENG_RIGID_BODY && n->type != ENG_STATIC_BODY)) return;
    int s = eng_shape_from_name(shape);
    if (s != ENG_SHAPE_NONE) n->shape = s;
}

void eng_physics_init(void) {}

/* Scene load: wake every rigid body from its placed pose. */
static void fresh_body(EngNode *n) {
    if (n->type == ENG_RIGID_BODY) { n->grounded = 0; n->sleep_t = 0; n->asleep = 0; }
    for (EngNode *c = n->child; c; c = c->next) fresh_body(c);
}
void eng_physics_reset(void) { if (eng_node_root()) fresh_body(eng_node_root()); }
void eng_body_sync_scene(void) {}

/* ── collision primitive: sphere vs axis-aligned box ───────────────────── */

/* Returns contact normal (pointing from the box toward the sphere centre)
 * and penetration depth; 0 = no contact. */
static int box_contact(const EngNode *dyn, const EngNode *st,
                       float *nx, float *ny, float *nz, float *pen) {
    float h = box_half(st);
    float px = dyn->gpos.x, py = dyn->gpos.y, pz = dyn->gpos.z;
    float cx = st->gpos.x, cy = st->gpos.y, cz = st->gpos.z;
    float r = sphere_radius(dyn);
    float qx = fminf(fmaxf(px, cx - h), cx + h);   /* closest point on the box */
    float qy = fminf(fmaxf(py, cy - h), cy + h);
    float qz = fminf(fmaxf(pz, cz - h), cz + h);
    float dx = px - qx, dy = py - qy, dz = pz - qz;
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 >= r * r) return 0;
    if (d2 > 1e-9f) {
        float d = sqrtf(d2);
        *pen = r - d; *nx = dx / d; *ny = dy / d; *nz = dz / d;
    } else {                     /* centre inside the box: exit the nearest face */
        float ex = fminf(px - (cx - h), (cx + h) - px);
        float ey = fminf(py - (cy - h), (cy + h) - py);
        float ez = fminf(pz - (cz - h), (cz + h) - pz);
        if (ex <= ey && ex <= ez) { *nx = px < cx ? -1 : 1; *ny = 0; *nz = 0; *pen = r + ex; }
        else if (ey <= ez)         { *nx = 0; *ny = py < cy ? -1 : 1; *nz = 0; *pen = r + ey; }
        else                       { *nx = 0; *ny = 0; *nz = pz < cz ? -1 : 1; *pen = r + ez; }
    }
    return 1;
}

/* Collect static colliders (boxes and spheres) into `st` (fixed capacity). */
static int collect_statics(EngNode *stack_out[256], EngNode *st[128]) {
    int count = 0, sp = 0;
    if (eng_node_root()) stack_out[sp++] = eng_node_root();
    while (sp) {
        EngNode *n = stack_out[--sp];
        if (n->type == ENG_STATIC_BODY && n->shape != ENG_SHAPE_NONE && count < 128)
            st[count++] = n;
        for (EngNode *c = n->child; c; c = c->next) if (sp < 256) stack_out[sp++] = c;
    }
    return count;
}

static void resolve_static(EngNode *dyn, const EngNode *st) {
    float nx, ny, nz, pen;
    if (st->shape == ENG_SHAPE_SPHERE) {
        float dx = dyn->gpos.x - st->gpos.x, dy = dyn->gpos.y - st->gpos.y, dz = dyn->gpos.z - st->gpos.z;
        float rr = sphere_radius(dyn) + sphere_radius(st);
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 >= rr * rr) return;
        if (d2 > 1e-9f) { float d = sqrtf(d2); pen = rr - d; nx = dx / d; ny = dy / d; nz = dz / d; }
        else { pen = rr; nx = 0; ny = 1; nz = 0; }
    } else {
        if (!box_contact(dyn, st, &nx, &ny, &nz, &pen)) return;
    }
    dyn->gpos.x += nx * pen; dyn->gpos.y += ny * pen; dyn->gpos.z += nz * pen;
    float vn = dyn->vel[0] * nx + dyn->vel[1] * ny + dyn->vel[2] * nz;
    if (vn < 0) {
        float rest = fminf(1.0f, fmaxf(0.0f, st->restitution * dyn->restitution));
        float j = -(1 + rest) * vn;
        dyn->vel[0] += j * nx; dyn->vel[1] += j * ny; dyn->vel[2] += j * nz;
    }
    /* surface classification for grounding / ceiling */
    if (ny > ENG_GROUND_DOT && vn < 0) { dyn->grounded = 1; if (dyn->vel[1] < 0) dyn->vel[1] = 0; }
    else if (ny < -ENG_GROUND_DOT && vn < 0 && dyn->vel[1] > 0) dyn->vel[1] = 0;
}

void eng_physics_step(float dt) {
    if (!eng_node_root() || dt <= 0) return;
    eng_node_update_transforms();

    /* Gather dynamic bodies that are free to move. */
    EngNode *stack[256]; int sp = 0, nb = 0;
    EngNode *bodies[128];
    if (eng_node_root()) stack[sp++] = eng_node_root();
    while (sp && nb < 128) {
        EngNode *n = stack[--sp];
        if (n->type == ENG_RIGID_BODY && body_free(n)) bodies[nb++] = n;
        for (EngNode *c = n->child; c; c = c->next) if (sp < 256) stack[sp++] = c;
    }
    if (!nb) return;

    EngNode *statics[128];
    int ns = collect_statics(stack, statics);

    /* gravity + integrate the velocity of every awake body */
    for (int i = 0; i < nb; i++) {
        EngNode *b = bodies[i];
        if (b->asleep) continue;            /* fully resting: never move again */
        b->grounded = 0;
        b->vel[1] -= ENG_GRAVITY * b->gravity_scale * dt;
        b->gpos.x += b->vel[0] * dt;
        b->gpos.y += b->vel[1] * dt;
        b->gpos.z += b->vel[2] * dt;
    }

    /* two solver passes for stability on a body resting across box seams */
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < nb; i++) {
            EngNode *b = bodies[i];
            if (b->asleep) continue;
            for (int s = 0; s < ns; s++) resolve_static(b, statics[s]);
        }
        for (int i = 0; i < nb; i++) {
            bodies[i]->pos = bodies[i]->gpos;   /* keep local pos in sync */
            bodies[i]->dirty = 1;
        }
        eng_node_update_transforms();
    }

    /* put still, grounded bodies to sleep so they never jitter */
    for (int i = 0; i < nb; i++) {
        EngNode *b = bodies[i];
        if (b->asleep) continue;
        float sq = b->vel[0] * b->vel[0] + b->vel[1] * b->vel[1] + b->vel[2] * b->vel[2];
        if (b->grounded && sq < ENG_SLEEP_EPS) {
            b->sleep_t += 1;
            if (b->sleep_t >= ENG_SLEEP_FRAMES) {
                b->asleep = 1;
                b->vel[0] = b->vel[1] = b->vel[2] = 0;
            }
        } else b->sleep_t = 0;
        b->pos = b->gpos; b->dirty = 1;
    }
    eng_node_update_transforms();
}
