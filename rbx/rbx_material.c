/* Собственные маленькие текстуры блоков, без внешних ассетов.
 * Палитровые mip-уровни убирают рябь мелких деталей вдали. */
#include "rbx_render_internal.h"
#include <limits.h>

enum { GRASS_TOP, GRASS_SIDE, DIRT, STONE, SAND, WATER, BARK, RINGS, LEAVES, MATERIALS };
static RbxMaterial materials[MATERIALS];
static int initialized;
static uint32_t hash(unsigned x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; return x * 0x846ca68bu; }
static int clamp8(int x) { return x < 0 ? 0 : x > 255 ? 255 : x; }
static uint32_t tint(uint32_t c, int d) {
    int r = clamp8((int)((c >> 16) & 255) + d);
    int g = clamp8((int)((c >> 8) & 255) + d);
    int b = clamp8((int)(c & 255) + d);
    return 0xff000000u | (uint32_t)(r << 16) | (uint32_t)(g << 8) | (uint32_t)b;
}
static void build_mips(RbxMaterial *m) {
    int source = 0, dest = 256;
    for (int size = 16; size > 1; size /= 2) {
        int next = size / 2;
        for (int y = 0; y < next; y++) {
            for (int x = 0; x < next; x++) {
                int r = 0, g = 0, b = 0;
                for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++) {
                    uint32_t c = m->palette[m->mip[source + (y * 2 + dy) * size + x * 2 + dx]];
                    r += (c >> 16) & 255; g += (c >> 8) & 255; b += c & 255;
                }
                r /= 4; g /= 4; b /= 4;
                int best = 0, distance = INT_MAX;
                for (int i = 0; i < 16; i++) {
                    int dr = r - (int)((m->palette[i] >> 16) & 255);
                    int dg = g - (int)((m->palette[i] >> 8) & 255);
                    int db = b - (int)(m->palette[i] & 255);
                    int d = dr * dr + dg * dg + db * db;
                    if (d < distance) { distance = d; best = i; }
                }
                m->mip[dest + y * next + x] = (unsigned char)best;
            }
        }
        source = dest; dest += next * next;
    }
}
static void initialize(void) {
    static const uint32_t base[MATERIALS] = {0x8CBD53,0x89613F,0x89613F,0x91918A,0xDACC99,0x4C97C5,0x86663E,0xB28B52,0x629945};
    static const int delta[8] = {-22,-15,-9,-3,3,9,15,22};
    for (int t = 0; t < MATERIALS; t++) {
        RbxMaterial *m = &materials[t];
        for (int i = 0; i < 16; i++) {
            uint32_t c = t == GRASS_SIDE && i >= 8 ? base[GRASS_TOP] : base[t];
            int d = delta[i & 7];
            if (t == WATER || t == SAND) d /= 2;
            m->palette[i] = tint(c, d);
        }
        for (int y = 0; y < 16; y++) for (int x = 0; x < 16; x++) {
            uint32_t h = hash((unsigned)(x + y * 16 + t * 173));
            int index = 2 + (int)(h % 4);
            if (t == GRASS_TOP || t == LEAVES) index = (int)((hash((unsigned)(x / 2 + y / 2 * 8 + t * 49)) + (h & 1)) % 8);
            if (t == GRASS_SIDE && y < 2 + (int)(hash((unsigned)x) % 4)) index += 8;
            if (t == STONE && (x + y * 2) % 11 == 0) index = 1;
            if (t == WATER) index = (y + x / 5) % 7 == 0 ? 6 : 3 + (int)(h & 1);
            if (t == BARK) index = (x / 2) % 3 == 0 ? (int)(h % 3) : 4 + (int)(h % 3);
            if (t == RINGS) { int r = abs(x - 7) > abs(y - 7) ? abs(x - 7) : abs(y - 7); index = 2 + r % 4; }
            m->mip[y * 16 + x] = (unsigned char)index;
        }
        build_mips(m);
    }
    initialized = 1;
}
const RbxMaterial *rbx_material(int block, int face) {
    if (!initialized) initialize();
    int texture = STONE;
    switch (block) {
        case BLOCK_GRASS: texture = face == 0 ? GRASS_TOP : face == 1 ? DIRT : GRASS_SIDE; break;
        case BLOCK_DIRT: texture = DIRT; break;
        case BLOCK_SAND: texture = SAND; break;
        case BLOCK_WATER: texture = WATER; break;
        case BLOCK_LOG: texture = face < 2 ? RINGS : BARK; break;
        case BLOCK_LEAVES: texture = LEAVES; break;
        default: break;
    }
    return &materials[texture];
}
