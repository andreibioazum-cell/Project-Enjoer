/* rbx/rbx_scene.c — отрисовка 3D-сцены: камера от третьего лица,
 * мир, монеты, боты и «нуб» игрока.
 *
 * Антипикселизация: 3D рендерится в полном разрешении, а на больших
 * экранах (где полный кадр софтверным рендером дорог) — в половинном
 * с последующим плавным апскейлом (см. rbx3d_end/upscale_smooth). */
#include "rbx_internal.h"
#include <math.h>

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

/* Полный кадр — до этого количества пикселей; выше рендерим в половинном
 * разрешении и плавно апскейлим (производительность софтверного рендера). */
#define RBX_FULLRES_MAX_PIXELS 1500000L

static int render_scale(void) {
    long total = (long)screen_w * (long)screen_h;
    return total > RBX_FULLRES_MAX_PIXELS ? 2 : 1;
}

void rbx_scene_draw(Buffer *buffer) {
    float px, py, pz, cyaw, cpitch;
    rbx_player_pos(&px, &py, &pz, NULL, NULL);
    rbx_camera_angles(&cyaw, &cpitch);

    float lookx = px, looky = py + 3.2f, lookz = pz;
    float dist = 11.0f;
    float cp = cosf(cpitch), sp = sinf(cpitch);
    float camx = lookx - sinf(cyaw) * cp * dist;
    float camy = looky - sp * dist;
    float camz = lookz - cosf(cyaw) * cp * dist;
    if (camy < 1.2f) camy = 1.2f;

    if (!rbx3d_begin(buffer, render_scale(), camx, camy, camz, cyaw, cpitch, 72.0f)) return;
    rbx3d_sky(0xFF6EC5F7u, 0xFFB3E5FCu);

    int nbox = 0;
    const RbxBox *boxes = rbx_world_boxes(&nbox);
    for (int i = 0; i < nbox; i++) {
        const RbxBox *b = &boxes[i];
        rbx3d_box(b->x, b->y, b->z, b->hx, b->hy, b->hz, 0, b->color);
    }
    float spin = (float)rbx_t_abs * 2.4f;
    int ncoin = 0;
    const RbxCoin *coins = rbx_world_coins(&ncoin);
    for (int i = 0; i < ncoin; i++) {
        if (coins[i].taken) continue;
        float bob = sinf((float)rbx_t_abs * 3.0f + i) * 0.25f;
        rbx3d_box(coins[i].x, coins[i].y + bob, coins[i].z, 0.42f, 0.42f, 0.12f, spin, 0xFFFFD600u);
    }
    const RbxBot *bots = rbx_world_bots();
    for (int i = 0; i < MAX_BOT; i++) {
        const RbxBot *b = &bots[i];
        avatar(b->x, b->y, b->z, b->yaw, b->phase, b->head, b->torso, b->pants, b->head);
    }
    /* классический нуб */
    float pyaw, walk;
    rbx_player_pos(NULL, NULL, NULL, &pyaw, &walk);
    avatar(px, py, pz, pyaw, walk, 0xFFF5CD30u, 0xFF0D69ACu, 0xFF4B974Bu, 0xFFF5CD30u);
    rbx3d_end();
}
