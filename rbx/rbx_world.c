/* rbx/rbx_world.c — постройка мира (база, дом, обби, башня...),
 * монеты, боты-игроки и их обновление. */
#include "rbx_internal.h"
#include <math.h>
#include <string.h>

static RbxBox boxes[MAX_BOX];
static int nbox;
static RbxCoin coins[MAX_COIN];
static int ncoin, ncaught;
static RbxBot bots[MAX_BOT];

static void add(float x, float y, float z, float hx, float hy, float hz, uint32_t col, int mat) {
    if (nbox >= MAX_BOX) return;
    RbxBox *b = &boxes[nbox++];
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

void rbx_world_build(void) {
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

const RbxBox *rbx_world_boxes(int *count) {
    if (count) *count = nbox;
    return boxes;
}

const RbxCoin *rbx_world_coins(int *count) {
    if (count) *count = ncoin;
    return coins;
}

const RbxBot *rbx_world_bots(void) { return bots; }
int rbx_world_caught(void) { return ncaught; }
int rbx_world_coin_count(void) { return ncoin; }

void rbx_world_collect_reset(void) { ncaught = 0; }

void rbx_world_collect(float px, float py, float pz) {
    for (int i = 0; i < ncoin; i++) {
        if (coins[i].taken) continue;
        float dx = px - coins[i].x, dy = (py + 2.5f) - coins[i].y, dz = pz - coins[i].z;
        if (dx * dx + dy * dy + dz * dz < 2.8f) {
            coins[i].taken = 1;
            ncaught++;
            snd_play("notify.wav");
        }
    }
}

void rbx_bots_update(float d) {
    for (int i = 0; i < MAX_BOT; i++) {
        RbxBot *b = &bots[i];
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
