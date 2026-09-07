/* 2D world rendering: parallax skies, textured/tinted tiles, springs,
 * spikes, lava, coins, checkpoints and the goal flag. Falls back to flat
 * colors when a texture asset is missing (e.g. unit tests). */
#include "platformium_internal.h"

typedef struct {
    uint32_t sky_top, sky_bottom, hill_far, hill_near, sun;
    int cave;
} PmTheme;

static const PmTheme themes[PM_THEME_COUNT] = {
    { 0xff6fc4ffu, 0xffd9f1ffu, 0xff4f9a52u, 0xff397743u, 0xffffedb0u, 0 },
    { 0xff14161fu, 0xff332839u, 0xff221d2cu, 0xff191521u, 0xffb8c4d8u, 1 },
    { 0xff8a5aa0u, 0xffffc98au, 0xffd99a56u, 0xffb87a44u, 0xffff9d5cu, 0 },
};

enum { TEX_GRASS_TOP, TEX_DIRT, TEX_STONE, TEX_SAND, TEX_LOG, TEX_COUNT };
static Image textures[TEX_COUNT];
static int tex_ok[TEX_COUNT];

void pm_textures_load(AAssetManager *assets) {
    static const char *paths[TEX_COUNT] = {
        "textures/grass_top.png", "textures/dirt.png", "textures/stone.png",
        "textures/sand.png", "textures/log_side.png",
    };
    for (int i = 0; i < TEX_COUNT; i++) {
        tex_ok[i] = image_load(assets, paths[i], &textures[i]);
    }
}

static const PmTheme *theme(void) {
    PmLevelDef *level = pm_level_live();
    int t = level ? level->theme : PM_THEME_GRASS;
    if (t < 0 || t >= PM_THEME_COUNT) t = PM_THEME_GRASS;
    return &themes[t];
}

static uint32_t mix32(uint32_t a, uint32_t b, float t) {
    uint32_t out = 0xff000000u;
    for (int shift = 16; shift >= 0; shift -= 8) {
        int ca = (a >> shift) & 0xff, cb = (b >> shift) & 0xff;
        int c = (int)(ca + (cb - ca) * t);
        out |= (uint32_t)c << shift;
    }
    return out;
}

static float hash11(float n) {
    float s = sinf(n * 127.1f) * 43758.5453f;
    return s - floorf(s);
}

void pm_render_background(float time) {
    const PmTheme *t = theme();
    /* sky gradient */
    int bands = 9;
    for (int i = 0; i < bands; i++) {
        float y0 = screen_h * i / (float)bands;
        float y1 = screen_h * (i + 1) / (float)bands + 1;
        rect(0, y0, (float)screen_w, y1 - y0, mix32(t->sky_top, t->sky_bottom, i / (float)(bands - 1)));
    }
    float camx = 0, camy = 0;
    pm_camera_center(&camx, &camy);
    float tpx = pm_tile_px();
    if (!t->cave) {
        /* sun */
        circle(screen_w * .80f, screen_h * .20f, screen_h * .075f, t->sun);
        ring(screen_w * .80f, screen_h * .20f, screen_h * .095f, screen_h * .012f, 0x40ffffffu);
        /* drifting clouds */
        float span = screen_w * 1.6f;
        for (int i = 0; i < 4; i++) {
            float cx = fmodf(i * span * .27f - camx * tpx * .12f + time * 6.0f, span) - screen_w * .3f;
            float cy = screen_h * (.12f + .16f * hash11((float)i));
            float r = screen_h * (.045f + .03f * hash11((float)i + 9));
            circle(cx, cy, r, 0xb8ffffffu);
            circle(cx + r * 1.1f, cy + r * .25f, r * .8f, 0xb8ffffffu);
            circle(cx - r * 1.1f, cy + r * .3f, r * .7f, 0xb8ffffffu);
        }
    } else {
        /* faint floating dust in the cave */
        for (int i = 0; i < 24; i++) {
            float px = fmodf(hash11((float)i) * screen_w * 1.7f - camx * tpx * .1f, screen_w * 1.2f);
            float py = fmodf(hash11((float)i + 40) * screen_h + time * 8.0f, (float)screen_h);
            circle(px, py, 1.6f, 0x28d0d8e8u);
        }
    }
    /* two hill layers */
    float horizon = screen_h * .80f + (camy * tpx - screen_h * .5f) * .06f;
    for (int layer = 0; layer < 2; layer++) {
        float p = layer ? .30f : .14f;
        float r = screen_h * (layer ? .16f : .24f);
        float spacing = r * 1.9f;
        uint32_t c = layer ? t->hill_near : t->hill_far;
        float shift = fmodf(camx * tpx * p, spacing);
        for (float x = -shift - spacing; x < screen_w + spacing; x += spacing) {
            float bump = r * (.8f + .35f * hash11(floorf((x + shift) / spacing) + layer * 57));
            circle(x, horizon + bump * .55f, bump, c);
        }
        rect(0, horizon + r * .5f, (float)screen_w, screen_h - horizon, c);
    }
    if (t->cave) {
        /* stalactites hanging from the ceiling */
        float spacing = screen_h * .22f;
        float shift = fmodf(camx * tpx * .22f, spacing);
        for (float x = -shift; x < screen_w + spacing; x += spacing) {
            float len = screen_h * (.10f + .12f * hash11(floorf((x + shift) / spacing) + 17));
            float w = spacing * .30f;
            for (int s = 0; s < 6; s++) {
                float k = s / 6.0f;
                rect(x - w * .5f * (1 - k), 0, w * (1 - k), len * (1 - k * .85f), 0xff100d17u);
            }
        }
    }
}

static void draw_solid_tile(int tx, int ty, float x, float y, float s) {
    const PmTheme *t = theme();
    PmLevelDef *level = pm_level_live();
    int above = pm_tile(tx, ty - 1);
    int is_top = above != PM_SOLID && above != PM_SPRING;
    const Image *img = NULL;
    if (level->theme == PM_THEME_GRASS) img = is_top && tex_ok[TEX_GRASS_TOP] ? &textures[TEX_GRASS_TOP] : tex_ok[TEX_DIRT] ? &textures[TEX_DIRT] : NULL;
    else if (level->theme == PM_THEME_STONE) img = tex_ok[TEX_STONE] ? &textures[TEX_STONE] : NULL;
    else img = tex_ok[TEX_SAND] ? &textures[TEX_SAND] : NULL;
    if (img) image_draw(img, x, y, s, s);
    else {
        uint32_t c = level->theme == PM_THEME_STONE ? 0xff5c6270u
                   : level->theme == PM_THEME_SAND ? 0xffd8b06cu : 0xff7a5233u;
        if (is_top && level->theme == PM_THEME_GRASS) c = 0xff58a838u;
        rect(x, y, s, s, c);
    }
    /* bevel */
    if (is_top) rect(x, y, s, s * .10f, 0x30ffffffu);
    int right_open = !pm_tile_solid(tx + 1, ty), left_open = !pm_tile_solid(tx - 1, ty);
    if (right_open) rect(x + s * .92f, y, s * .08f, s, 0x28000000u);
    if (left_open) rect(x, y, s * .08f, s, 0x18ffffffu);
    rect(x, y + s * .90f, s, s * .10f, 0x28000000u);
    if (t->cave) rect(x, y, s, s, 0x1c000010u);
}

static void draw_oneway(int tx, float x, float y, float s) {
    float h = s * .30f;
    if (tex_ok[TEX_LOG]) image_draw(&textures[TEX_LOG], x, y, s, h);
    else rect(x, y, s, h, 0xff9a6a3du);
    rect(x, y, s, h * .25f, 0x30ffffffu);
    rect(x, y + h * .8f, s, h * .2f, 0x38000000u);
    rect(x + s * .06f, y + h, s * .08f, s * .12f, 0xff6a4527u);
    rect(x + s * .86f, y + h, s * .08f, s * .12f, 0xff6a4527u);
    (void)tx;
}

static void draw_spikes(float x, float y, float s) {
    for (int spike = 0; spike < 2; spike++) {
        float bx = x + s * .5f * spike;
        for (int layer = 0; layer < 5; layer++) {
            float k = layer / 5.0f;
            float w = s * .5f * (1 - k);
            rect(bx + (s * .25f - w * .5f), y + s - s * .8f * (k + .2f), w, s * .18f,
                 layer == 4 ? 0xffcfd6deu : 0xff98a2aeu);
        }
        rect(bx, y + s * .92f, s * .5f, s * .08f, 0xff5a626cu);
    }
}

static void draw_lava(int tx, int ty, float x, float y, float s, float time) {
    rect(x, y, s, s, 0xffc33e18u);
    rect(x, y, s, s * .45f, 0xffe25822u);
    float wob = sinf(time * 3.0f + tx * 1.7f) * s * .06f;
    circle(x + s * .25f, y + s * .18f + wob, s * .22f, 0xffffa53bu);
    circle(x + s * .75f, y + s * .14f - wob, s * .22f, 0xffffa53bu);
    if (pm_tile(tx, ty - 1) != PM_LAVA) rect(x, y, s, s * .08f, 0xffffd98au);
}

static void draw_coin(int tx, int ty, float x, float y, float s, float time) {
    if (pm_tile_taken(tx, ty)) return;
    float bob = sinf(time * 3.2f + (tx + ty) * .9f) * s * .07f;
    float cx = x + s * .5f, cy = y + s * .5f + bob;
    float r = s * .30f;
    float spin = fabsf(sinf(time * 4.0f + tx));
    circle(cx, cy, r, 0xffe9b23au);
    circle(cx, cy, r * .72f, 0xffffd25eu);
    rect(cx - r * .18f * spin, cy - r * .5f, r * .36f * spin + .5f, r, 0xffe9b23au);
}

static void draw_spring(int tx, int ty, float x, float y, float s) {
    float fx, fy, amount = 0;
    pm_spring_fx(&fx, &fy, &amount);
    int squashing = amount > 0 && fabsf(fx - (tx + .5f)) < .1f && fabsf(fy - (ty + .5f)) < .1f;
    float compress = squashing ? .45f + .55f * (1 - amount) : 1.0f;
    float base_h = s * .22f;
    float coil_h = s * .42f * compress;
    float plate_h = s * .16f;
    float y0 = y + s - base_h;
    rect(x + s * .1f, y0, s * .8f, base_h, 0xff3c434du);
    for (int c = 0; c < 3; c++) {
        float cy = y0 - coil_h * (c + .5f) / 3.0f;
        rect(x + s * .18f, cy, s * .64f, s * .07f, 0xff8f9aa8u);
    }
    rect(x + s * .06f, y0 - coil_h - plate_h, s * .88f, plate_h, 0xffe15f81u);
    rect(x + s * .06f, y0 - coil_h - plate_h, s * .88f, plate_h * .4f, 0xffff8faau);
}

static void draw_checkpoint(int tx, int ty, float x, float y, float s) {
    int active = pm_tile_taken(tx, ty);
    rect(x + s * .46f, y - s * 1.1f, s * .08f, s * 2.1f, 0xff5a626cu);
    float fw = s * .55f, fh = s * .36f;
    uint32_t c = active ? 0xff2ee6a8u : 0xff9aa3adu;
    rect(x + s * .54f, y - s * 1.05f, fw, fh, c);
    if (active) circle(x + s * .5f, y - s * 1.1f, s * .10f, 0xffffd25eu);
}

void pm_render_flag(float time) {
    PmLevelDef *level = pm_level_live();
    if (!level || level->goal_tx < 0) return;
    float tpx = pm_tile_px();
    float sx, sy;
    pm_world_to_screen(level->goal_tx + .5f, level->goal_ty + .5f, &sx, &sy);
    float s = tpx;
    /* pole and base */
    rect(sx - s * .05f, sy - s * 3.2f, s * .10f, s * 3.7f, 0xffd8dde4u);
    circle(sx, sy - s * 3.25f, s * .14f, 0xffffd25eu);
    rect(sx - s * .4f, sy + s * .4f, s * .8f, s * .12f, 0xff5a626cu);
    /* waving flag */
    float wave = sinf(time * 5.0f) * s * .10f;
    float fw = s * 1.1f, fh = s * .6f;
    rect(sx + s * .05f, sy - s * 3.05f + wave * .3f, fw, fh * .5f, 0xff2ee6a8u);
    rect(sx + s * .05f, sy - s * 3.05f + fh * .5f + wave * .6f, fw * .82f, fh * .5f, 0xff25c48fu);
    circle(sx + s * .45f, sy - s * 2.75f + wave * .4f, s * .14f, 0xffffffffu);
}

void pm_render_tiles(float time) {
    PmLevelDef *level = pm_level_live();
    if (!level) return;
    float tpx = pm_tile_px();
    float cx, cy;
    pm_camera_center(&cx, &cy);
    int x0 = (int)floorf(cx - screen_w / (2 * tpx)) - 1;
    int x1 = (int)ceilf(cx + screen_w / (2 * tpx)) + 1;
    int y0 = (int)floorf(cy - screen_h / (2 * tpx)) - 1;
    int y1 = (int)ceilf(cy + screen_h / (2 * tpx)) + 1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > level->w - 1) x1 = level->w - 1;
    if (y1 > level->h - 1) y1 = level->h - 1;
    for (int ty = y0; ty <= y1; ty++) {
        for (int tx = x0; tx <= x1; tx++) {
            int t = level->tiles[(size_t)ty * PM_MAX_W + tx];
            if (t == PM_EMPTY) continue;
            float sx, sy;
            pm_world_to_screen(tx, ty, &sx, &sy);
            switch (t) {
                case PM_SOLID: draw_solid_tile(tx, ty, sx, sy, tpx); break;
                case PM_ONEWAY: draw_oneway(tx, sx, sy, tpx); break;
                case PM_SPIKE: draw_spikes(sx, sy, tpx); break;
                case PM_LAVA: draw_lava(tx, ty, sx, sy, tpx, time); break;
                case PM_COIN: draw_coin(tx, ty, sx, sy, tpx, time); break;
                case PM_SPRING: draw_solid_tile(tx, ty, sx, sy, tpx); draw_spring(tx, ty, sx, sy, tpx); break;
                case PM_CHECK: draw_checkpoint(tx, ty, sx, sy, tpx); break;
                case PM_GOAL: break; /* drawn by pm_render_flag */
                default: break;
            }
        }
    }
}
