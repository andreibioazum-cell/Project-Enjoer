/* Детерминированная генерация: высоты, слои земли, вода и деревья.
 * Один seed + мировые координаты дают одинаковые блоки на границах чанков. */
#include "rbx_internal.h"
#include <math.h>
#include <string.h>

static uint32_t world_seed = RBX_WORLD_SEED;
static uint32_t hash(int x, int z, uint32_t salt) {
    uint32_t h = (uint32_t)x * 0x8da6b343u ^ (uint32_t)z * 0xd8163841u ^ world_seed ^ salt;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu;
    return h ^ (h >> 16);
}
static int floor_div(int x, int size) { return x / size - (x < 0 && x % size != 0); }
static float smooth(float x) { return x * x * (3 - 2 * x); }
static float noise(float x, float z, uint32_t salt) {
    int ix = (int)floorf(x), iz = (int)floorf(z);
    float u = smooth(x - ix), v = smooth(z - iz);
    float a = (hash(ix, iz, salt) & 65535) / 32767.5f - 1;
    float b = (hash(ix + 1, iz, salt) & 65535) / 32767.5f - 1;
    float c = (hash(ix, iz + 1, salt) & 65535) / 32767.5f - 1;
    float d = (hash(ix + 1, iz + 1, salt) & 65535) / 32767.5f - 1;
    return (a + (b - a) * u) * (1 - v) + (c + (d - c) * u) * v;
}
void rbx_terrain_seed(uint32_t seed) { world_seed = seed; }

int rbx_terrain_height(int x, int z) {
    float h = 10 + 7 * noise(x * .025f, z * .025f, 11) +
              3.5f * noise(x * .066f, z * .066f, 83) + 1.3f * noise(x * .17f, z * .17f, 191);
    /* Небольшая безопасная поляна на старте, дальше — только генератор. */
    float dx = x - 8.0f, dz = z - 8.0f, r = sqrtf(dx * dx + dz * dz);
    if (r < 10) { float t = smooth(fmaxf(0, (r - 4) / 6)); h = 12 + (h - 12) * t; }
    int result = (int)floorf(h);
    return result < 3 ? 3 : result > 25 ? 25 : result;
}

typedef struct { int x, z, ground, height, valid; } Tree;
static Tree tree_at(int gx, int gz) {
    uint32_t h = hash(gx, gz, 701);
    Tree t = {gx * 8 + 2 + (int)(h % 4), gz * 8 + 2 + (int)((h >> 8) % 4), 0, 4 + (int)((h >> 16) & 1), 0};
    if (h % 100 > 62) return t;
    float dx = (float)t.x - 8, dz = (float)t.z - 8;
    if (dx * dx + dz * dz < 64) return t;
    t.ground = rbx_terrain_height(t.x, t.z);
    t.valid = t.ground > WATER_LEVEL + 1 && noise(gx * .12f, gz * .12f, 97) > -.45f;
    return t;
}
static int tree_block(const Tree *t, int x, int y, int z) {
    if (!t->valid) return BLOCK_AIR;
    int dx = abs(x - t->x), dz = abs(z - t->z), top = t->ground + t->height;
    if (dx == 0 && dz == 0 && y > t->ground && y <= top) return BLOCK_LOG;
    if (y < top - 1 || y > top + 2) return BLOCK_AIR;
    int radius = y == top + 2 ? 1 : 2;
    if (dx > radius || dz > radius || (dx == 2 && dz == 2)) return BLOCK_AIR;
    if (y == top + 2 && dx == 1 && dz == 1) return BLOCK_AIR;
    return BLOCK_LEAVES;
}
static int ground_block(int x, int y, int z, int height) {
    if (y < 0) return BLOCK_STONE;
    if (y > height) return y <= WATER_LEVEL ? BLOCK_WATER : BLOCK_AIR;
    if (height <= WATER_LEVEL + 1 && y >= height - 2) return BLOCK_SAND;
    if (y == height) return height > 20 ? BLOCK_STONE : BLOCK_GRASS;
    (void)x; (void)z;
    return y >= height - 3 ? BLOCK_DIRT : BLOCK_STONE;
}
int rbx_terrain_block(int x, int y, int z) {
    if (y > 32 || y >= WORLD_HEIGHT) return BLOCK_AIR; /* max generated canopy; edits can build higher */
    if (y < 0) return BLOCK_STONE;
    int b = ground_block(x, y, z, rbx_terrain_height(x, z));
    if (b != BLOCK_AIR) return b;
    int gx = floor_div(x, 8), gz = floor_div(z, 8), result = BLOCK_AIR;
    for (int iz = gz - 1; iz <= gz + 1; iz++) {
        for (int ix = gx - 1; ix <= gx + 1; ix++) {
            Tree t = tree_at(ix, iz);
            int tb = tree_block(&t, x, y, z);
            if (tb == BLOCK_LOG) return tb;
            if (tb == BLOCK_LEAVES) result = tb;
        }
    }
    return result;
}
static void put(unsigned char *out, int x, int y, int z, int block) {
    if (x < 0 || x >= CHUNK_SIZE || z < 0 || z >= CHUNK_SIZE || y < 0 || y >= WORLD_HEIGHT) return;
    int index = (y * CHUNK_SIZE + z) * CHUNK_SIZE + x;
    if (block == BLOCK_LOG || out[index] == BLOCK_AIR) out[index] = (unsigned char)block;
}
void rbx_terrain_chunk(int cx, int cz, unsigned char *out) {
    int ox = cx * CHUNK_SIZE, oz = cz * CHUNK_SIZE;
    memset(out, 0, CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT);
    for (int z = 0; z < CHUNK_SIZE; z++) {
        for (int x = 0; x < CHUNK_SIZE; x++) {
            int height = rbx_terrain_height(ox + x, oz + z);
            int top = height > WATER_LEVEL ? height : WATER_LEVEL;
            for (int y = 0; y <= top; y++)
                out[(y * CHUNK_SIZE + z) * CHUNK_SIZE + x] = (unsigned char)ground_block(ox + x, y, oz + z, height);
        }
    }
    for (int gz = floor_div(oz - 2, 8); gz <= floor_div(oz + CHUNK_SIZE + 1, 8); gz++) {
        for (int gx = floor_div(ox - 2, 8); gx <= floor_div(ox + CHUNK_SIZE + 1, 8); gx++) {
            Tree t = tree_at(gx, gz);
            if (!t.valid) continue;
            for (int y = t.ground + 1; y <= t.ground + t.height + 2; y++) {
                for (int z = t.z - 2; z <= t.z + 2; z++) {
                    for (int x = t.x - 2; x <= t.x + 2; x++) {
                        int b = tree_block(&t, x, y, z);
                        if (b) put(out, x - ox, y, z - oz, b);
                    }
                }
            }
        }
    }
}
