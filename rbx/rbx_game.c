/* Мир, физика, аватар «нуб» и сразу 3D — как заход в плейс Roblox. */
#include "rbx/rbx.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { MAT_SOLID = 0, MAT_LAVA = 1, MAT_WATER = 2, MAT_BOUNCE = 3, MAT_SKIP = 4 };
enum { MAX_BOX = 240, MAX_COIN = 12, MAX_BOT = 3, MAX_PTR = 8 };

typedef struct {
    float x, y, z, hx, hy, hz;
    uint32_t color;
    int mat;
} Box;

typedef struct {
    float x, y, z;
    int taken;
} Coin;

typedef struct {
    float x, y, z, yaw, phase;
    float wp[4][2];
    int nwp, i, dir;
    uint32_t head, torso, pants;
} Bot;

/* из rbx_render.c */
int rbx3d_begin(Buffer *b, int sc, float cx, float cy, float cz, float yaw, float pitch, float fov_deg);
void rbx3d_sky(uint32_t top, uint32_t bot);
void rbx3d_box(float x, float y, float z, float hx, float hy, float hz, float yaw, uint32_t color);
void rbx3d_end(void);
int rbx3d_project(float x, float y, float z, float *sx, float *sy);

static Box boxes[MAX_BOX];
static int nbox;
static Coin coins[MAX_COIN];
static int ncoin, ncaught;
static Bot bots[MAX_BOT];

static float px, py, pz, pvy, pyaw, walk;
static int on_ground, jumped;
static float cyaw, cpitch;
static double t_abs, join_t;
static int k_w, k_a, k_s, k_d, k_sp, k_left, k_right, k_up, k_down;

static int p_on[MAX_PTR];
static float p_x[MAX_PTR], p_y[MAX_PTR];
static int joy_id, look_id;
static float jx, jy, look_lx, look_ly;
static float joy_cx, joy_cy, joy_r, jmp_cx, jmp_cy, jmp_r;

static void add(float x, float y, float z, float hx, float hy, float hz, uint32_t col, int mat) {
    if (nbox >= MAX_BOX) return;
    Box *b = &boxes[nbox++];
    b->x = x; b->y = y; b->z = z;
    b->hx = hx; b->hy = hy; b->hz = hz;
    b->color = col; b->mat = mat;
}

static void coin_at(float x, float y, float z) {
    if (ncoin >= MAX_COIN) return;
    coins[ncoin].x = x; coins[ncoin].y = y; coins[ncoin].z = z; coins[ncoin].taken = 0;
    ncoin++;
}

static void tree(float x, float z) {
    add(x, 1.15f, z, 0.38f, 1.15f, 0.38f, 0xFF6D4C41u, MAT_SOLID);
    add(x, 3.15f, z, 1.55f, 1.05f, 1.55f, 0xFF2E7D32u, MAT_SOLID);
    add(x, 4.55f, z, 1.05f, 0.75f, 1.05f, 0xFF43A047u, MAT_SOLID);
}

static void build_world(void) {
    nbox = 0; ncoin = 0;
    /* зелёная baseplate, как стартовый плейс (клетки, чтобы не рвать z) */
    for (int i = -2; i < 2; i++) {
        for (int j = -2; j < 2; j++) {
            uint32_t c = ((i + j) & 1) ? 0xFF3D9E44u : 0xFF4CAF50u;
            add(i * 16.0f + 8.0f, -0.5f, j * 16.0f + 8.0f, 8.0f, 0.5f, 8.0f, c, MAT_SOLID);
        }
    }

    /* спавн */
    add(0, 0.14f, 0, 4.2f, 0.14f, 4.2f, 0xFFBDBDBDu, MAT_SOLID);
    add(0, 0.30f, 0, 1.15f, 0.04f, 1.15f, 0xFFFFD54Fu, MAT_SKIP);

    /* дорожка к дому */
    for (int i = 0; i < 7; i++)
        add(-3.5f - i * 1.7f, 0.06f, 4.0f + i * 0.9f, 1.1f, 0.06f, 1.1f, 0xFFD7CCC8u, MAT_SOLID);

    /* домик */
    add(-18, 0.22f, 12, 5.0f, 0.22f, 5.0f, 0xFF8D6E63u, MAT_SOLID);
    add(-18, 3.0f, 7.25f, 5.0f, 3.0f, 0.32f, 0xFFC62828u, MAT_SOLID);
    add(-22.7f, 3.0f, 12, 0.32f, 3.0f, 5.0f, 0xFFD32F2Fu, MAT_SOLID);
    add(-13.3f, 3.0f, 12, 0.32f, 3.0f, 5.0f, 0xFFD32F2Fu, MAT_SOLID);
    add(-20.5f, 3.0f, 16.75f, 2.3f, 3.0f, 0.32f, 0xFFC62828u, MAT_SOLID);
    add(-15.5f, 3.0f, 16.75f, 2.3f, 3.0f, 0.32f, 0xFFC62828u, MAT_SOLID);
    add(-18, 5.35f, 16.75f, 1.3f, 0.7f, 0.32f, 0xFFB71C1Cu, MAT_SOLID);
    add(-18, 6.45f, 12, 5.5f, 0.38f, 5.5f, 0xFF5D4037u, MAT_SOLID);
    add(-18, 7.05f, 12, 4.0f, 0.32f, 4.0f, 0xFF4E342Eu, MAT_SOLID);
    add(-18, 7.55f, 12, 2.2f, 0.28f, 2.2f, 0xFF3E2723u, MAT_SOLID);
    add(-15.2f, 8.3f, 10.2f, 0.5f, 1.1f, 0.5f, 0xFF6D4C41u, MAT_SOLID);

    /* радужный обби */
    static const uint32_t rb[] = {
        0xFFE53935u, 0xFFFB8C00u, 0xFFFDD835u, 0xFF43A047u,
        0xFF1E88E5u, 0xFF8E24AAu, 0xFFEC407Au
    };
    for (int i = 0; i < 7; i++) {
        float h = 0.45f + i * 1.55f;
        float z = -5.0f + i * 5.4f;
        add(16.0f, h, z, 2.15f, 0.38f, 2.15f, rb[i], MAT_SOLID);
        if (i == 1 || i == 3 || i == 6) coin_at(16.0f, h + 1.6f, z);
    }
    add(16.0f, 11.4f, 34.0f, 3.4f, 0.45f, 3.4f, 0xFFFFD54Fu, MAT_SOLID);
    coin_at(16.0f, 13.0f, 34.0f);

    /* башня */
    for (int i = 0; i < 6; i++) {
        uint32_t c = (i & 1) ? 0xFF64B5F6u : 0xFF1E88E5u;
        add(-14.0f, 1.4f + i * 2.8f, -18.0f, 2.3f, 1.4f, 2.3f, c, MAT_SOLID);
        add(-14.0f, 2.75f + i * 2.8f, -18.0f, 3.0f, 0.18f, 3.0f, 0xFF1565C0u, MAT_SOLID);
    }
    coin_at(-14.0f, 18.2f, -18.0f);

    /* деревья */
    tree(8, -14); tree(-8, -12); tree(-24, 4); tree(24, 8); tree(10, 24); tree(-6, 22);

    /* бассейн */
    add(18, -0.05f, 18, 5.5f, 0.55f, 4.5f, 0xFF1E88E5u, MAT_WATER);
    add(12.2f, 0.45f, 18, 0.4f, 0.5f, 4.9f, 0xFFBDBDBDu, MAT_SOLID);
    add(23.8f, 0.45f, 18, 0.4f, 0.5f, 4.9f, 0xFFBDBDBDu, MAT_SOLID);
    add(18, 0.45f, 13.2f, 6.0f, 0.5f, 0.4f, 0xFFBDBDBDu, MAT_SOLID);
    add(18, 0.45f, 22.8f, 6.0f, 0.5f, 0.4f, 0xFFBDBDBDu, MAT_SOLID);

    /* лава */
    add(-22, -0.12f, -6, 4.0f, 0.4f, 4.0f, 0xFFFF5722u, MAT_LAVA);
    add(-22, 0.18f, -6, 3.1f, 0.12f, 3.1f, 0xFFFFC107u, MAT_LAVA);

    /* батут */
    add(8, 0.22f, 9, 2.1f, 0.22f, 2.1f, 0xFF7E57C2u, MAT_BOUNCE);
    add(8, 0.48f, 9, 1.65f, 0.12f, 1.65f, 0xFFE040FBu, MAT_BOUNCE);

    /* ящики и горка-кубы */
    add(5.5f, 0.7f, -8, 0.7f, 0.7f, 0.7f, 0xFF8D6E63u, MAT_SOLID);
    add(6.6f, 0.5f, -7.2f, 0.5f, 0.5f, 0.5f, 0xFFA1887Fu, MAT_SOLID);
    add(-5, 0.9f, 8, 0.9f, 0.9f, 0.9f, 0xFF5C6BC0u, MAT_SOLID);
    add(-4.2f, 2.1f, 8.6f, 0.55f, 0.55f, 0.55f, 0xFF7986CBu, MAT_SOLID);

    /* парящие платформы */
    add(22, 6.2f, 2, 2.4f, 0.4f, 2.4f, 0xFF9CCC65u, MAT_SOLID);
    add(26, 8.8f, -4, 2.0f, 0.35f, 2.0f, 0xFFAED581u, MAT_SOLID);
    coin_at(26, 10.4f, -4);

    /* монумент из кубов у спавна */
    add(0, 1.1f, -8, 1.1f, 1.1f, 1.1f, 0xFFE2231Au, MAT_SOLID);
    add(0, 2.5f, -8, 0.55f, 0.45f, 0.55f, 0xFFFFFFFFu, MAT_SKIP);

    coin_at(-18, 2.2f, 12);
    coin_at(8, 2.4f, 9);
    coin_at(18, 1.6f, 18);

    /* боты гуляют вокруг */
    bots[0].nwp = 4;
    bots[0].wp[0][0] = 4;  bots[0].wp[0][1] = 6;
    bots[0].wp[1][0] = 10; bots[0].wp[1][1] = 2;
    bots[0].wp[2][0] = 6;  bots[0].wp[2][1] = -6;
    bots[0].wp[3][0] = -2; bots[0].wp[3][1] = 2;
    bots[0].head = 0xFFFFCC80u; bots[0].torso = 0xFFE91E63u; bots[0].pants = 0xFF212121u;
    bots[1].nwp = 3;
    bots[1].wp[0][0] = -6; bots[1].wp[0][1] = -4;
    bots[1].wp[1][0] = -12; bots[1].wp[1][1] = 2;
    bots[1].wp[2][0] = -4; bots[1].wp[2][1] = 10;
    bots[1].head = 0xFF8D6E63u; bots[1].torso = 0xFF00BCD4u; bots[1].pants = 0xFF37474Fu;
    bots[2].nwp = 2;
    bots[2].wp[0][0] = 12; bots[2].wp[0][1] = 12;
    bots[2].wp[1][0] = 20; bots[2].wp[1][1] = 10;
    bots[2].head = 0xFFFFF3E0u; bots[2].torso = 0xFFFF9800u; bots[2].pants = 0xFF4E342Eu;
    for (int i = 0; i < MAX_BOT; i++) {
        bots[i].x = bots[i].wp[0][0];
        bots[i].z = bots[i].wp[0][1];
        bots[i].y = 0;
        bots[i].yaw = 0; bots[i].phase = (float)i;
        bots[i].i = 0; bots[i].dir = 1;
    }
}

static void spawn_player(void) {
    px = 0; py = 0.32f; pz = 1.2f;
    pvy = 0; pyaw = 0.55f; walk = 0;
    on_ground = 1;
    cyaw = 0.48f; cpitch = -0.20f;
}

static void layout(void) {
    float s = (float)screen_w / 400.0f;
    if (s < 0.75f) s = 0.75f;
    joy_r = 64.0f * s;
    joy_cx = 86.0f * s;
    joy_cy = (float)screen_h - 90.0f * s;
    jmp_r = 40.0f * s;
    jmp_cx = (float)screen_w - 74.0f * s;
    jmp_cy = (float)screen_h - 100.0f * s;
}

static void part(float x, float y, float z, float yaw,
                 float lx, float ly, float lz,
                 float hx, float hy, float hz, uint32_t col) {
    float c = cosf(yaw), s = sinf(yaw);
    rbx3d_box(x + lx * c + lz * s, y + ly, z - lx * s + lz * c, hx, hy, hz, yaw, col);
}

static void avatar(float x, float y, float z, float yaw, float phase,
                   uint32_t head, uint32_t torso, uint32_t pants, uint32_t arms) {
    float sw = sinf(phase);
    part(x, y, z, yaw, 0, 3.05f, 0, 1.00f, 1.05f, 0.50f, torso);
    part(x, y, z, yaw, 0, 4.60f, 0, 0.62f, 0.52f, 0.62f, head);
    part(x, y, z, yaw, -0.52f, 1.05f,  sw * 0.42f, 0.38f, 1.05f, 0.38f, pants);
    part(x, y, z, yaw,  0.52f, 1.05f, -sw * 0.42f, 0.38f, 1.05f, 0.38f, pants);
    part(x, y, z, yaw, -1.42f, 3.05f, -sw * 0.35f, 0.32f, 1.00f, 0.32f, arms);
    part(x, y, z, yaw,  1.42f, 3.05f,  sw * 0.35f, 0.32f, 1.00f, 0.32f, arms);
}

static int aabb(float ax, float ay, float az, float ahx, float ahy, float ahz, const Box *b) {
    return fabsf(ax - b->x) < ahx + b->hx &&
           fabsf(ay - b->y) < ahy + b->hy &&
           fabsf(az - b->z) < ahz + b->hz;
}

static void resolve_axis(int axis, float *px_, float *py_, float *pz_, float *pvy_, int *ground, int *lava, int *water, int *bounce) {
    const float hx = 0.82f, hy = 2.45f, hz = 0.82f;
    float cx = *px_, cy = *py_ + hy, cz = *pz_;
    for (int i = 0; i < nbox; i++) {
        Box *b = &boxes[i];
        if (b->mat == MAT_SKIP) continue;
        if (b->mat == MAT_WATER) {
            if (aabb(cx, cy, cz, hx, hy, hz, b)) *water = 1;
            continue;
        }
        if (b->mat == MAT_LAVA) {
            if (fabsf(*px_ - b->x) < b->hx && fabsf(*pz_ - b->z) < b->hz &&
                *py_ < b->y + b->hy + 1.8f && *py_ > b->y - b->hy - 1.0f)
                *lava = 1;
            continue;
        }
        if (!aabb(cx, cy, cz, hx, hy, hz, b)) continue;
        if (b->mat == MAT_BOUNCE && axis == 1) *bounce = 1;
        if (axis == 0) {
            float pen = hx + b->hx - fabsf(cx - b->x);
            *px_ += (cx > b->x) ? pen : -pen;
            cx = *px_;
        } else if (axis == 2) {
            float pen = hz + b->hz - fabsf(cz - b->z);
            *pz_ += (cz > b->z) ? pen : -pen;
            cz = *pz_;
        } else {
            float pen = hy + b->hy - fabsf(cy - b->y);
            if (cy >= b->y) {
                *py_ += pen;
                if (*pvy_ < 0) *pvy_ = 0;
                *ground = 1;
            } else {
                *py_ -= pen;
                if (*pvy_ > 0) *pvy_ = 0;
            }
            cy = *py_ + hy;
        }
    }
}

void rbx_key(const char *name, int down) {
    int d = down ? 1 : 0;
    if (!name || !name[0]) return;
    if (!strcmp(name, "w") || !strcmp(name, "W")) k_w = d;
    else if (!strcmp(name, "s") || !strcmp(name, "S")) k_s = d;
    else if (!strcmp(name, "a") || !strcmp(name, "A")) k_a = d;
    else if (!strcmp(name, "d") || !strcmp(name, "D")) k_d = d;
    else if (!strcmp(name, "ArrowLeft")) k_left = d;
    else if (!strcmp(name, "ArrowRight")) k_right = d;
    else if (!strcmp(name, "ArrowUp")) k_up = d;
    else if (!strcmp(name, "ArrowDown")) k_down = d;
    else if (!strcmp(name, "space") || !strcmp(name, " ")) k_sp = d;
}

static void do_jump(void) {
    if (on_ground && !jumped) {
        pvy = 16.5f;
        on_ground = 0;
        jumped = 1;
        snd_play("send.wav");
    }
}

void init(AAssetManager *assets) {
    (void)assets;
    snd_load("send.wav");
    snd_load("notify.wav");
    build_world();
    spawn_player();
    ncaught = 0;
    join_t = 0;
    t_abs = 0;
    joy_id = look_id = -1;
    jx = jy = 0;
    memset(p_on, 0, sizeof(p_on));
    ds_log("Enjoer: 3D-плейс, заход сразу в мир");
}

void reset(void) {
    build_world();
    spawn_player();
    ncaught = 0;
    join_t = 0;
    joy_id = look_id = -1;
    jx = jy = 0;
}

void update(void) {
    float d = (float)dt;
    if (d > 0.05f) d = 0.05f;
    t_abs += d;
    join_t += d;
    layout();

    if (k_left) cyaw -= 1.7f * d;
    if (k_right) cyaw += 1.7f * d;
    if (k_up) cpitch += 1.1f * d;
    if (k_down) cpitch -= 1.1f * d;
    if (cpitch < -1.15f) cpitch = -1.15f;
    if (cpitch > 0.55f) cpitch = 0.55f;

    float mx = jx, mz = -jy;
    if (k_w) mz += 1;
    if (k_s) mz -= 1;
    if (k_a) mx -= 1;
    if (k_d) mx += 1;
    float ml = sqrtf(mx * mx + mz * mz);
    if (ml > 1.0f) { mx /= ml; mz /= ml; ml = 1; }

    float fs = sinf(cyaw), fc = cosf(cyaw);
    float wishx = fs * mz + fc * mx;
    float wishz = fc * mz + (-fs) * mx;

    int water = 0, lava = 0, bounce = 0;
    float speed = water ? 6.0f : 13.5f;
    /* water flag from previous frame roughly — resolve after move */

    if (k_sp) do_jump();
    else jumped = 0;

    pvy += -48.0f * d;
    if (pvy < -42.0f) pvy = -42.0f;
    py += pvy * d;
    on_ground = 0;
    resolve_axis(1, &px, &py, &pz, &pvy, &on_ground, &lava, &water, &bounce);
    if (water) {
        pvy += 28.0f * d; /* выталкивание */
        if (pvy > 4.0f) pvy = 4.0f;
        if (pvy < -8.0f) pvy = -8.0f;
        speed = 6.5f;
    }
    if (bounce && pvy <= 0.5f) {
        pvy = 24.0f;
        on_ground = 0;
        bounce = 0;
    }

    px += wishx * speed * d;
    resolve_axis(0, &px, &py, &pz, &pvy, &on_ground, &lava, &water, &bounce);
    pz += wishz * speed * d;
    resolve_axis(2, &px, &py, &pz, &pvy, &on_ground, &lava, &water, &bounce);

    if (ml > 0.15f) {
        pyaw = atan2f(wishx, wishz);
        walk += d * 9.0f * ml;
    } else {
        walk += d * 0.8f;
    }

    if (lava || py < -6.0f) {
        spawn_player();
        snd_play("notify.wav");
    }

    for (int i = 0; i < ncoin; i++) {
        if (coins[i].taken) continue;
        float dx = px - coins[i].x, dy = (py + 2.5f) - coins[i].y, dz = pz - coins[i].z;
        if (dx * dx + dy * dy + dz * dz < 2.8f) {
            coins[i].taken = 1;
            ncaught++;
            snd_play("notify.wav");
        }
    }

    for (int i = 0; i < MAX_BOT; i++) {
        Bot *b = &bots[i];
        int ni = b->i + b->dir;
        if (ni < 0 || ni >= b->nwp) { b->dir = -b->dir; ni = b->i + b->dir; }
        float tx = b->wp[ni][0], tz = b->wp[ni][1];
        float dx = tx - b->x, dz = tz - b->z;
        float L = sqrtf(dx * dx + dz * dz);
        float sp = 3.6f * d;
        if (L < 0.3f) b->i = ni;
        else {
            b->x += dx / L * sp;
            b->z += dz / L * sp;
            b->yaw = atan2f(dx, dz);
            b->phase += d * 8.0f;
        }
        b->y = 0;
    }
}

static void draw_world(Buffer *buffer) {
    float lookx = px, looky = py + 3.2f, lookz = pz;
    float dist = 11.0f;
    float cp = cosf(cpitch), sp = sinf(cpitch);
    float camx = lookx - sinf(cyaw) * cp * dist;
    float camy = looky - sp * dist;
    float camz = lookz - cosf(cyaw) * cp * dist;
    if (camy < 1.2f) camy = 1.2f;

    int sc = 2;
    if (screen_w <= 360) sc = 2;
    if (!rbx3d_begin(buffer, sc, camx, camy, camz, cyaw, cpitch, 72.0f)) return;
    rbx3d_sky(0xFF6EC5F7u, 0xFFB3E5FCu);

    for (int i = 0; i < nbox; i++) {
        Box *b = &boxes[i];
        rbx3d_box(b->x, b->y, b->z, b->hx, b->hy, b->hz, 0, b->color);
    }
    float spin = (float)t_abs * 2.4f;
    for (int i = 0; i < ncoin; i++) {
        if (coins[i].taken) continue;
        float bob = sinf((float)t_abs * 3.0f + i) * 0.25f;
        rbx3d_box(coins[i].x, coins[i].y + bob, coins[i].z, 0.42f, 0.42f, 0.12f, spin, 0xFFFFD600u);
    }
    for (int i = 0; i < MAX_BOT; i++) {
        Bot *b = &bots[i];
        avatar(b->x, b->y, b->z, b->yaw, b->phase, b->head, b->torso, b->pants, b->head);
    }
    /* классический нуб */
    avatar(px, py, pz, pyaw, walk, 0xFFF5CD30u, 0xFF0D69ACu, 0xFF4B974Bu, 0xFFF5CD30u);
    rbx3d_end();
}

static void text_c(const char *s, float cx, float cy, float sc, uint32_t col) {
    int w = text_width(s);
    text_scaled(s, cx - (w * sc) * 0.5f, cy, col, sc);
}

static void draw_hud(void) {
    float W = (float)screen_w, H = (float)screen_h;
    float u = W / 420.0f;
    if (u < 0.7f) u = 0.7f;

    /* верхняя панель как у Roblox */
    rect(0, 0, W, 50.0f * u, 0xE8232327u);
    roundrect(10 * u, 9 * u, 32 * u, 32 * u, 7 * u, 0xFFE2231Au);
    roundrect(17 * u, 16 * u, 18 * u, 18 * u, 4 * u, 0xFFFFFFFFu);
    text_scaled("Obby Park", 50 * u, 14 * u, 0xFFFFFFFFu, 0.62f * u);

    char buf[40];
    snprintf(buf, sizeof(buf), "монеты  %d/%d", ncaught, ncoin);
    int tw = text_width(buf);
    text_scaled(buf, W - tw * 0.55f * u - 14 * u, 16 * u, 0xFFFFF59Du, 0.55f * u);

    /* джойстик */
    ring(joy_cx, joy_cy, joy_r, 4.0f, 0x66FFFFFFu);
    circle(joy_cx, joy_cy, joy_r, 0x33000000u);
    circle(joy_cx + jx * (joy_r * 0.48f), joy_cy + jy * (joy_r * 0.48f), joy_r * 0.38f, 0xCCFFFFFFu);

    /* прыжок */
    circle(jmp_cx, jmp_cy, jmp_r, 0xCC43A047u);
    ring(jmp_cx, jmp_cy, jmp_r, 3.0f, 0xAAFFFFFFu);
    text_c("прыжок", jmp_cx, jmp_cy - 8 * u, 0.42f * u, 0xFFFFFFFFu);

    /* имена над головами */
    float sx, sy;
    if (rbx3d_project(px, py + 5.4f, pz, &sx, &sy))
        text_c("Ты", sx, sy - 10, 0.45f * u, 0xFFFFFFFFu);
    static const char *names[MAX_BOT] = { "Mila", "Rob", "Alex" };
    for (int i = 0; i < MAX_BOT; i++) {
        if (rbx3d_project(bots[i].x, bots[i].y + 5.4f, bots[i].z, &sx, &sy))
            text_c(names[i], sx, sy - 8, 0.40f * u, 0xFFE0E0E0u);
    }

    /* короткая плашка «зашли» — мир уже 3D под ней */
    if (join_t < 1.6) {
        float a = join_t < 0.9 ? 1.0f : (float)((1.6 - join_t) / 0.7);
        if (a < 0) a = 0;
        uint32_t al = (uint32_t)(a * 200 + 20);
        float bw = 260 * u, bh = 54 * u;
        roundrect(W * 0.5f - bw * 0.5f, 64 * u, bw, bh, 12 * u, (al << 24) | 0x001A1A1Eu);
        text_c("Зашли в Obby Park", W * 0.5f, 78 * u, 0.55f * u, 0xFFFFFFFFu);
    }
}

void draw(Buffer *buffer) {
    draw_world(buffer);
    draw_hud();
}

static int hit_circ(float x, float y, float cx, float cy, float r) {
    float dx = x - cx, dy = y - cy;
    return dx * dx + dy * dy <= r * r;
}

void touch(float x, float y, int action, int pointer_id) {
    int id = pointer_id;
    if (id < 0 || id >= MAX_PTR) id = 0;
    if (action == 0) { /* down */
        p_on[id] = 1; p_x[id] = x; p_y[id] = y;
        if (hit_circ(x, y, jmp_cx, jmp_cy, jmp_r * 1.25f)) {
            do_jump();
            jumped = 1;
            return;
        }
        if (x < (float)screen_w * 0.46f && y > (float)screen_h * 0.55f && joy_id < 0) {
            joy_id = id;
            jx = (x - joy_cx) / joy_r;
            jy = (y - joy_cy) / joy_r;
            float L = sqrtf(jx * jx + jy * jy);
            if (L > 1) { jx /= L; jy /= L; }
        } else if (look_id < 0) {
            look_id = id;
            look_lx = x; look_ly = y;
        }
    } else if (action == 2) { /* move */
        p_x[id] = x; p_y[id] = y;
        if (id == joy_id) {
            jx = (x - joy_cx) / joy_r;
            jy = (y - joy_cy) / joy_r;
            float L = sqrtf(jx * jx + jy * jy);
            if (L > 1) { jx /= L; jy /= L; }
        } else if (id == look_id) {
            float dx = x - look_lx, dy = y - look_ly;
            cyaw += dx / (float)screen_w * 3.4f;
            cpitch -= dy / (float)screen_h * 2.2f;
            if (cpitch < -1.15f) cpitch = -1.15f;
            if (cpitch > 0.55f) cpitch = 0.55f;
            look_lx = x; look_ly = y;
        }
    } else if (action == 1) { /* up */
        p_on[id] = 0;
        if (id == joy_id) { joy_id = -1; jx = 0; jy = 0; }
        if (id == look_id) look_id = -1;
        jumped = 0;
    }
}
