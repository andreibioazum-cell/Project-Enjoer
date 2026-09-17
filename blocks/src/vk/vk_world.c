/* src/vk/vk_world.c — the voxel world on the GPU.
 *
 * One vertex/index buffer pair per chunk, uploaded only when the chunk mesh
 * gets a new serial (an edit, a water update or a freshly streamed chunk), so
 * the per-frame cost is a handful of draw calls. The index buffer holds the
 * opaque triangles first and the water triangles after them: the two passes of
 * the software renderer become two index ranges of the same buffer.
 *
 * Ring order, chunk culling and the near/fog culling math are the same as in
 * geometrium_world.c, so both backends draw the same chunks in the same order. */
#include "vk_internal.h"
#include "../geometrium/geometrium_world_internal.h"

#include <math.h>

static VkChunk *chunk_lookup(int cx, int cz, int create) {
    uint32_t hash = (uint32_t)(cx * 73856093) ^ (uint32_t)(cz * 19349663);
    uint32_t slot = hash & (VK_CHUNK_SLOTS - 1);
    for (int probe = 0; probe < VK_CHUNK_SLOTS; probe++) {
        VkChunk *candidate = &vk.chunks[slot];
        if (candidate->used && candidate->cx == cx && candidate->cz == cz) return candidate;
        if (!candidate->used) {
            if (!create) return NULL;
            memset(candidate, 0, sizeof(*candidate));
            candidate->cx = cx;
            candidate->cz = cz;
            candidate->used = 1;
            vk.chunk_count++;
            return candidate;
        }
        slot = (slot + 1) & (VK_CHUNK_SLOTS - 1);
    }
    return NULL;
}

/* Scratch buffers for mesh conversion: grown once, reused for every chunk. */
static VkWorldVertex *scratch_vertices;
static size_t scratch_vertex_capacity;
static uint32_t *scratch_indices;
static size_t scratch_index_capacity;

static int ensure_scratch(size_t vertices, size_t indices) {
    if (vertices > scratch_vertex_capacity) {
        VkWorldVertex *grown = realloc(scratch_vertices, vertices * sizeof(*grown));
        if (!grown) return 0;
        scratch_vertices = grown;
        scratch_vertex_capacity = vertices;
    }
    if (indices > scratch_index_capacity) {
        uint32_t *grown = realloc(scratch_indices, indices * sizeof(*grown));
        if (!grown) return 0;
        scratch_indices = grown;
        scratch_index_capacity = indices;
    }
    return 1;
}

/* Converts one chunk mesh into GPU vertices: white texels for textured quads,
 * a baked light byte per corner and the material layer of every face. Opaque
 * triangles are written from index 0, water triangles from the middle. */
static int build_mesh(const GeometriumChunkView *view, size_t *vertex_count, size_t *opaque_indices, size_t *water_indices) {
    size_t vertices = (size_t)view->count * 4;
    size_t indices = (size_t)view->count * 6;
    if (!ensure_scratch(vertices, indices)) return 0;
    size_t opaque = 0, water = (size_t)view->water_count * 6;
    for (int i = 0; i < view->count; i++) {
        const GeometriumQuad *quad = &view->quads[i];
        int sx = view->cx * CHUNK_SIZE * 2 + quad->x;
        int sz = view->cz * CHUNK_SIZE * 2 + quad->z;
        GeometriumVertex corners[4];
        unsigned char corner_light[4];
        float normal[3];
        geometrium_quad_build(sx, quad->y, sz, quad->u, quad->v, quad->face,
                              quad->light, corners, corner_light, normal);
        int layer = geometrium_material_layer(quad->block, quad->face);
        int is_water = geometrium_material_water(layer);
        uint32_t base = (uint32_t)(i * 4);
        for (int corner = 0; corner < 4; corner++) {
            float shade = geometrium_vertex_shade(normal[0], normal[1], normal[2], corner_light[corner]);
            if (!isfinite(shade)) shade = 0.0f;
            if (shade < 0.0f) shade = 0.0f;
            if (shade > 1.0f) shade = 1.0f;
            VkWorldVertex *out = &scratch_vertices[i * 4 + corner];
            out->x = corners[corner].x; out->y = corners[corner].y; out->z = corners[corner].z;
            out->u = corners[corner].u; out->v = corners[corner].v;
            out->r = out->g = out->b = out->a = 255;
            out->shade = (unsigned char)(shade * 255.0f + 0.5f);
            out->layer = (unsigned char)layer;
            out->pad0 = out->pad1 = 0;
        }
        uint32_t *target = is_water ? scratch_indices + water : scratch_indices + opaque;
        target[0] = base + 0; target[1] = base + 1; target[2] = base + 2;
        target[3] = base + 0; target[4] = base + 2; target[5] = base + 3;
        if (is_water) water += 6; else opaque += 6;
    }
    if (water > (size_t)view->water_count * 6) {
        /* Some chunks are queued for the water pass but no longer hold water:
         * compact the tail onto the end of the opaque block. */
        memmove(scratch_indices + opaque, scratch_indices + (size_t)view->count * 6,
                ((size_t)view->water_count * 6) * sizeof(uint32_t));
        water = (size_t)view->water_count * 6;
    }
    *vertex_count = vertices;
    *opaque_indices = opaque;
    *water_indices = water;
    return 1;
}

static int upload_chunk(VkChunk *chunk, const GeometriumChunkView *view) {
    size_t vertices = 0, opaque = 0, water = 0;
    if (!build_mesh(view, &vertices, &opaque, &water)) return 0;
    size_t index_count = opaque + water;
    if (index_count == 0) { chunk->serial = view->serial; chunk->index_count = 0; return 1; }

    VkDeviceSize vertex_bytes = (VkDeviceSize)vertices * sizeof(VkWorldVertex);
    VkDeviceSize index_bytes = (VkDeviceSize)index_count * sizeof(uint32_t);
    VkDeviceSize vertex_offset = 0;
    VkDeviceSize index_offset = (vertex_bytes + 3) & ~(VkDeviceSize)3;
    VkDeviceSize needed = index_offset + index_bytes;
    if (chunk->size < needed) {
        if (chunk->buffer) {
            vkDestroyBuffer(vk.device, chunk->buffer, NULL);
            vkFreeMemory(vk.device, chunk->memory, NULL);
            chunk->buffer = VK_NULL_HANDLE;
            chunk->memory = VK_NULL_HANDLE;
            chunk->size = 0;
        }
        VkDeviceSize capacity = needed + needed / 2 + 4096;
        VkGpuBuffer buffer;
        if (!vk_create_buffer(capacity, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                              VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &buffer)) return 0;
        chunk->buffer = buffer.buffer;
        chunk->memory = buffer.memory;
        chunk->size = buffer.size;
    }
    chunk->vertex_offset = vertex_offset;
    chunk->index_offset = index_offset;
    chunk->index_count = (uint32_t)index_count;
    chunk->opaque_index_count = (uint32_t)opaque;
    chunk->serial = view->serial;
    if (!vk_upload(chunk->buffer, 0, scratch_vertices, vertex_bytes)) return 0;
    if (!vk_upload(chunk->buffer, index_offset, scratch_indices, index_bytes)) return 0;
    return 1;
}

/* The two passes walk the same rings as geometrium_world.c's draw_pass(). */
void vk_world_draw(int center_x, int center_z, int radius) {
    if (!vk.ready || !vk.pass3d_active) return;
    VkCommandBuffer cmd = vk.cmd[vk.frame_index];
    int waited_for_previous = 0;
    for (int water = 0; water < 2; water++) {
        vk_bind_world(water);
        for (int ring = 0; ring <= radius; ring++)
            for (int dz = -ring; dz <= ring; dz++)
                for (int dx = -ring; dx <= ring; dx++) {
                    if (abs(dx) != ring && abs(dz) != ring) continue;
                    int cx = center_x + dx, cz = center_z + dz;
                    GeometriumChunkView view;
                    if (!geometrium_world_chunk_view(cx, cz, &view)) continue;
                    if (!view.ready || !view.count) continue;
                    if (water && !view.water_count) continue;
                    /* Same chunk bounds test as the software draw_pass(). */
                    float ox = (float)cx * CHUNK_SIZE, oz = (float)cz * CHUNK_SIZE;
                    float half_height = (view.max_y - view.min_y) * 0.25f;
                    if (!geometrium3d_visible(ox + 8.0f, view.min_y * 0.5f + half_height, oz + 8.0f,
                                              8.0f, half_height, 8.0f))
                        continue;
                    VkChunk *chunk = chunk_lookup(cx, cz, 1);
                    if (!chunk) continue;
                    if (chunk->serial != view.serial) {
                        /* Re-uploading a buffer the previous frame may still read
                         * needs that frame finished: the chunk buffers are shared
                         * between the two frames in flight. */
                        if (!waited_for_previous) {
                            vkWaitForFences(vk.device, 1, &vk.fence[(vk.frame_index + 1) % VK_FRAMES_IN_FLIGHT],
                                            VK_TRUE, 1000000000ull);
                            waited_for_previous = 1;
                        }
                        if (!upload_chunk(chunk, &view)) continue;
                    }
                    uint32_t first = water ? chunk->opaque_index_count : 0;
                    uint32_t count = water ? chunk->index_count - chunk->opaque_index_count
                                           : chunk->opaque_index_count;
                    if (count == 0) continue;
                    vkCmdBindVertexBuffers(cmd, 0, 1, &chunk->buffer, &chunk->vertex_offset);
                    vkCmdBindIndexBuffer(cmd, chunk->buffer, chunk->index_offset, VK_INDEX_TYPE_UINT32);
                    vkCmdDrawIndexed(cmd, count, 1, first, 0, 0);
                }
    }
}

void vk_world_shutdown(void) {
    for (int i = 0; i < VK_CHUNK_SLOTS; i++) {
        VkChunk *chunk = &vk.chunks[i];
        if (chunk->buffer) vkDestroyBuffer(vk.device, chunk->buffer, NULL);
        if (chunk->memory) vkFreeMemory(vk.device, chunk->memory, NULL);
        memset(chunk, 0, sizeof(*chunk));
    }
    vk.chunk_count = 0;
    free(scratch_vertices);
    free(scratch_indices);
    scratch_vertices = NULL;
    scratch_indices = NULL;
    scratch_vertex_capacity = scratch_index_capacity = 0;
}
