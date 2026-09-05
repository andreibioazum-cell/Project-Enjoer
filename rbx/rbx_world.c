/* Скользящий кэш чанков: память ограничена, мир продолжается при движении.
 * В меш попадают только открытые грани — внутренних кубов в рендере нет. */
#include "rbx_internal.h"
#include <limits.h>
#include <math.h>
#include <stdlib.h>

enum { CACHE_SIDE = WORLD_RADIUS * 2 + 1, CACHE_COUNT = CACHE_SIDE * CACHE_SIDE };
typedef struct { unsigned char x, y, z, face, block; } Face;
typedef struct {
    int cx, cz, valid, ready, min_y, max_y;
    unsigned char blocks[CHUNK_SIZE * CHUNK_SIZE * WORLD_HEIGHT];
    Face *faces;
    int count, capacity;
} Chunk;
static Chunk chunks[CACHE_COUNT];
static int center_x = INT_MIN, center_z = INT_MIN;
static int floor_chunk(int x) { return x / CHUNK_SIZE - (x < 0 && x % CHUNK_SIZE != 0); }
static int wrap(int x) { int n = x % CACHE_SIDE; return n < 0 ? n + CACHE_SIDE : n; }
static Chunk *slot(int cx, int cz) { return &chunks[wrap(cz) * CACHE_SIDE + wrap(cx)]; }

int rbx_world_block(int x, int y, int z) {
    if (y < 0) return BLOCK_STONE;
    if (y >= WORLD_HEIGHT) return BLOCK_AIR;
    int cx = floor_chunk(x), cz = floor_chunk(z);
    Chunk *c = slot(cx, cz);
    if (!c->valid || c->cx != cx || c->cz != cz) return rbx_terrain_block(x, y, z);
    return c->blocks[(y * CHUNK_SIZE + z - cz * CHUNK_SIZE) * CHUNK_SIZE + x - cx * CHUNK_SIZE];
}
int rbx_world_solid(int x, int y, int z) {
    int b = rbx_world_block(x, y, z);
    return b != BLOCK_AIR && b != BLOCK_WATER;
}
static void add_face(Chunk *c, int x, int y, int z, int face, int block) {
    if (c->count == c->capacity) {
        int cap = c->capacity ? c->capacity * 2 : 512;
        Face *f = realloc(c->faces, (size_t)cap * sizeof(*f));
        if (!f) { ds_runtime_error("Недостаточно памяти для чанка"); return; }
        c->faces = f; c->capacity = cap;
    }
    Face f = {(unsigned char)x, (unsigned char)y, (unsigned char)z, (unsigned char)face, (unsigned char)block};
    c->faces[c->count++] = f;
    if (y < c->min_y) c->min_y = y;
    if (y + 1 > c->max_y) c->max_y = y + 1;
}
static void mesh(Chunk *c) {
    static const int offsets[6][3] = {{0,1,0},{0,-1,0},{0,0,1},{0,0,-1},{1,0,0},{-1,0,0}};
    c->count = 0; c->min_y = WORLD_HEIGHT; c->max_y = 0;
    int ox = c->cx * CHUNK_SIZE, oz = c->cz * CHUNK_SIZE;
    for (int y = 0; y < WORLD_HEIGHT; y++) {
        for (int z = 0; z < CHUNK_SIZE; z++) {
            for (int x = 0; x < CHUNK_SIZE; x++) {
                int b = c->blocks[(y * CHUNK_SIZE + z) * CHUNK_SIZE + x];
                if (!b) continue;
                for (int face = 0; face < 6; face++) {
                    int neighbor = rbx_world_block(ox + x + offsets[face][0], y + offsets[face][1], oz + z + offsets[face][2]);
                    if (neighbor == BLOCK_AIR || (neighbor == BLOCK_WATER && b != BLOCK_WATER))
                        add_face(c, x, y, z, face, b);
                }
            }
        }
    }
    c->ready = 1;
}
void rbx_world_update(float x, float z) {
    int cx = (int)floorf(x / CHUNK_SIZE), cz = (int)floorf(z / CHUNK_SIZE);
    if (cx == center_x && cz == center_z) return;
    center_x = cx; center_z = cz;
    /* Сначала данные всех соседей, затем открытые грани. */
    for (int dz = -WORLD_RADIUS; dz <= WORLD_RADIUS; dz++) {
        for (int dx = -WORLD_RADIUS; dx <= WORLD_RADIUS; dx++) {
            Chunk *c = slot(cx + dx, cz + dz);
            if (c->valid && c->cx == cx + dx && c->cz == cz + dz) continue;
            c->cx = cx + dx; c->cz = cz + dz; c->valid = 1; c->ready = 0;
            rbx_terrain_chunk(c->cx, c->cz, c->blocks);
        }
    }
    for (int i = 0; i < CACHE_COUNT; i++) if (!chunks[i].ready) mesh(&chunks[i]);
}
void rbx_world_build(uint32_t seed) {
    rbx_terrain_seed(seed);
    center_x = center_z = INT_MIN;
    for (int i = 0; i < CACHE_COUNT; i++) chunks[i].valid = chunks[i].ready = 0;
    rbx_world_update(8.5f, 8.5f);
}
void rbx_world_draw(void) {
    /* Ближние чанки закрывают дальние в z-буфере до выборки текстур. */
    for (int ring = 0; ring <= WORLD_RADIUS; ring++) {
        for (int dz = -ring; dz <= ring; dz++) {
            for (int dx = -ring; dx <= ring; dx++) {
                if (abs(dx) != ring && abs(dz) != ring) continue;
                Chunk *c = slot(center_x + dx, center_z + dz);
                if (!c->ready || !c->count) continue;
                float ox = c->cx * CHUNK_SIZE, oz = c->cz * CHUNK_SIZE;
                float hy = (c->max_y - c->min_y) * .5f;
                if (!rbx3d_visible(ox + 8, c->min_y + hy, oz + 8, 8, hy, 8)) continue;
                for (int i = 0; i < c->count; i++) {
                    const Face *f = &c->faces[i];
                    rbx3d_block_face(ox + f->x, f->y, oz + f->z, f->face, f->block);
                }
            }
        }
    }
}
void rbx_world_stats(int *loaded, int *faces) {
    int n = 0, f = 0;
    for (int i = 0; i < CACHE_COUNT; i++) { n += chunks[i].valid; f += chunks[i].count; }
    if (loaded) *loaded = n;
    if (faces) *faces = f;
}
