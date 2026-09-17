/* src/vk/vk_text.c — the block texture array and text.
 *
 * The block textures come straight from the game's PNG assets (the same
 * `GeometriumMaterial` images the software renderer quantises), packed into one
 * 2D array so the whole world is drawn with a single texture binding: a quad
 * only carries its layer index. Mip levels are generated here, so distant
 * blocks do not shimmer.
 *
 * Text reuses the shared font atlas (gfx_font.c bakes the glyphs); this file
 * only uploads the coverage bitmap and turns strings into textured quads with
 * exactly the pen/baseline math the software text renderer uses. */
#include "vk_internal.h"

enum { VK_TILE_SIZE = 32, VK_TILE_MIPS = 6 };

static uint32_t tile_layers;

static uint32_t white_layer(void) { return tile_layers - 1; }

/* Layer index of the flat white tile: flat-coloured geometry (the hand's arm)
 * samples it so it can share the chunk pipeline. */
int vk_text_white_layer(void) { return tile_layers ? (int)tile_layers - 1 : 0; }

/* ── texture array ───────────────────────────────────────────────────── */

static void average_half(const unsigned char *source, int size, unsigned char *destination) {
    int next = size / 2;
    for (int y = 0; y < next; y++) for (int x = 0; x < next; x++) {
        for (int channel = 0; channel < 4; channel++) {
            int total = 0;
            for (int dy = 0; dy < 2; dy++) for (int dx = 0; dx < 2; dx++)
                total += source[((y * 2 + dy) * size + x * 2 + dx) * 4 + channel];
            destination[(y * next + x) * 4 + channel] = (unsigned char)(total / 4);
        }
    }
}

static int create_tiles(void) {
    int materials = geometrium_material_count();
    if (materials <= 0) return vk_set_error("block textures are not loaded"), 0;
    tile_layers = (uint32_t)materials + 1;   /* + the flat white layer */

    size_t layer_bytes = 0, level_offset[VK_TILE_MIPS];
    for (int level = 0, size = VK_TILE_SIZE; level < VK_TILE_MIPS; level++, size /= 2) {
        level_offset[level] = layer_bytes;
        layer_bytes += (size_t)size * size * 4;
    }
    size_t total = layer_bytes * tile_layers;
    unsigned char *pixels = malloc(total);
    if (!pixels) return vk_set_error("out of memory for the texture array"), 0;
    memset(pixels, 0xff, total);
    for (uint32_t layer = 0; layer < tile_layers; layer++) {
        unsigned char *base = pixels + layer_bytes * layer;
        if (layer == white_layer()) continue;   /* stays white */
        const Image *image = geometrium_material_image((int)layer);
        if (!image || image->width != VK_TILE_SIZE || image->height != VK_TILE_SIZE) {
            free(pixels);
            return vk_set_error("block texture %u is not %dx%d", layer, VK_TILE_SIZE, VK_TILE_SIZE), 0;
        }
        memcpy(base, image->pixels, VK_TILE_SIZE * VK_TILE_SIZE * 4);
        for (int level = 1; level < VK_TILE_MIPS; level++)
            average_half(base + level_offset[level - 1], VK_TILE_SIZE >> (level - 1), base + level_offset[level]);
    }

    VkImageCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent.width = VK_TILE_SIZE;
    info.extent.height = VK_TILE_SIZE;
    info.extent.depth = 1;
    info.mipLevels = VK_TILE_MIPS;
    info.arrayLayers = tile_layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk.device, &info, NULL, &vk.tiles.image) != VK_SUCCESS) {
        free(pixels);
        return vk_set_error("texture array creation failed"), 0;
    }
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk.device, vk.tiles.image, &requirements);
    VkMemoryAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount; i++)
        if (requirements.memoryTypeBits & (1u << i)) { type = i; break; }
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(vk.device, &allocate, NULL, &vk.tiles.memory) != VK_SUCCESS ||
        vkBindImageMemory(vk.device, vk.tiles.image, vk.tiles.memory, 0) != VK_SUCCESS) {
        free(pixels);
        return vk_set_error("texture array memory failed"), 0;
    }
    VkImageViewCreateInfo view = {0};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = vk.tiles.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = VK_TILE_MIPS;
    view.subresourceRange.layerCount = tile_layers;
    if (vkCreateImageView(vk.device, &view, NULL, &vk.tiles.view) != VK_SUCCESS) {
        free(pixels);
        return vk_set_error("texture array view failed"), 0;
    }

    VkGpuBuffer staging;
    if (!vk_create_buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging)) {
        free(pixels);
        return 0;
    }
    if (staging.mapped) memcpy(staging.mapped, pixels, total);
    free(pixels);

    VkCommandBuffer cmd = vk_begin_once();
    if (cmd == VK_NULL_HANDLE) { vk_free_buffer(&staging); return 0; }
    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vk.tiles.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = VK_TILE_MIPS;
    barrier.subresourceRange.layerCount = tile_layers;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);

    for (uint32_t layer = 0; layer < tile_layers; layer++)
        for (uint32_t level = 0; level < VK_TILE_MIPS; level++) {
            uint32_t size = (uint32_t)(VK_TILE_SIZE >> level);
            VkBufferImageCopy region = {0};
            region.bufferOffset = (VkDeviceSize)(layer * layer_bytes + level_offset[level]);
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = level;
            region.imageSubresource.baseArrayLayer = layer;
            region.imageSubresource.layerCount = 1;
            region.imageExtent.width = size;
            region.imageExtent.height = size;
            region.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(cmd, staging.buffer, vk.tiles.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        }
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    vk_end_once(cmd);
    vk_free_buffer(&staging);

    vk.tiles.width = VK_TILE_SIZE;
    vk.tiles.height = VK_TILE_SIZE;
    vk.tiles.layers = (int)tile_layers;
    vk.tiles.mips = VK_TILE_MIPS;
    vk.tiles.format = VK_FORMAT_R8G8B8A8_UNORM;
    return 1;
}

/* ── font atlas ──────────────────────────────────────────────────────── */

void vk_font_upload(void) {
    const Font *font = gfx_font();
    if (!font || vk.glyphs.image) return;
    int width = font_aw(font), height = font_ah(font);
    const uint8_t *alpha = font_alpha(font);
    if (width <= 0 || height <= 0 || !alpha) return;

    VkImageCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8_UNORM;
    info.extent.width = (uint32_t)width;
    info.extent.height = (uint32_t)height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk.device, &info, NULL, &vk.glyphs.image) != VK_SUCCESS) { vk_set_error("font atlas image failed"); return; }
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk.device, vk.glyphs.image, &requirements);
    VkMemoryAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = 0;
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount; i++)
        if (requirements.memoryTypeBits & (1u << i)) { allocate.memoryTypeIndex = i; break; }
    if (vkAllocateMemory(vk.device, &allocate, NULL, &vk.glyphs.memory) != VK_SUCCESS ||
        vkBindImageMemory(vk.device, vk.glyphs.image, vk.glyphs.memory, 0) != VK_SUCCESS) {
        vk_set_error("font atlas memory failed");
        vkDestroyImage(vk.device, vk.glyphs.image, NULL);
        vk.glyphs.image = VK_NULL_HANDLE;
        return;
    }
    VkImageViewCreateInfo view = {0};
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = vk.glyphs.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vk.device, &view, NULL, &vk.glyphs.view) != VK_SUCCESS) { vk_set_error("font atlas view failed"); return; }

    size_t bytes = (size_t)width * height;
    VkGpuBuffer staging;
    if (!vk_create_buffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging)) return;
    if (staging.mapped) memcpy(staging.mapped, alpha, bytes);
    VkCommandBuffer cmd = vk_begin_once();
    if (cmd == VK_NULL_HANDLE) { vk_free_buffer(&staging); return; }
    VkImageMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = vk.glyphs.image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)width;
    region.imageExtent.height = (uint32_t)height;
    region.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(cmd, staging.buffer, vk.glyphs.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    vk_end_once(cmd);
    vk_free_buffer(&staging);

    vk.glyphs.width = width;
    vk.glyphs.height = height;
    vk.glyphs.layers = 1;
    vk.glyphs.mips = 1;
    vk.glyphs.format = VK_FORMAT_R8_UNORM;

    VkDescriptorImageInfo image = {0};
    image.imageView = vk.glyphs.view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.sampler = vk.sampler_glyphs;
    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = vk.set_glyphs;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
}

int vk_textures_init(AAssetManager *assets) {
    if (!geometrium_materials_load(assets)) return 0;
    if (!create_tiles()) return 0;
    if (gfx_font_load(assets)) vk_font_upload();

    VkDescriptorImageInfo image = {0};
    image.imageView = vk.tiles.view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.sampler = vk.sampler_tiles;
    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = vk.set_tiles;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
    return 1;
}

void vk_textures_shutdown(void) {
    if (vk.device == VK_NULL_HANDLE) return;
    if (vk.tiles.view) vkDestroyImageView(vk.device, vk.tiles.view, NULL);
    if (vk.tiles.image) vkDestroyImage(vk.device, vk.tiles.image, NULL);
    if (vk.tiles.memory) vkFreeMemory(vk.device, vk.tiles.memory, NULL);
    if (vk.glyphs.view) vkDestroyImageView(vk.device, vk.glyphs.view, NULL);
    if (vk.glyphs.image) vkDestroyImage(vk.device, vk.glyphs.image, NULL);
    if (vk.glyphs.memory) vkFreeMemory(vk.device, vk.glyphs.memory, NULL);
    memset(&vk.tiles, 0, sizeof(vk.tiles));
    memset(&vk.glyphs, 0, sizeof(vk.glyphs));
}

/* ── text ────────────────────────────────────────────────────────────── */

void vk_text_draw(const char *string, float x, float y, uint32_t color, float scale) {
    const Font *font = gfx_font();
    if (!font || !string || !isfinite(x + y + scale) || scale <= 0) return;
    if (!vk.glyphs.image) { vk_font_upload(); if (!vk.glyphs.image) return; }
    int aw = font_aw(font), ah = font_ah(font);
    float ascent = font_ascent(font), left_bearing = 0;
    const FontGlyph *reference = font_glyph(font, 'S');
    if (reference) { ascent = reference->bearing_top; left_bearing = reference->bearing_x; }
    float pen = x - left_bearing * scale, base = y + ascent * scale;
    float red = ((color >> 16) & 0xff) / 255.0f;
    float green = ((color >> 8) & 0xff) / 255.0f;
    float blue = (color & 0xff) / 255.0f;
    float alpha = ((color >> 24) & 0xff) / 255.0f;
    if (color >> 24 == 0) alpha = 1.0f;   /* gfx_pack() treats a zero alpha as opaque */

    for (const char *cursor = string; *cursor;) {
        int codepoint = gfx_utf8_decode(&cursor);
        if (codepoint == '\n') { pen = x - left_bearing * scale; base += font_lineh(font) * scale; continue; }
        const FontGlyph *glyph = font_glyph(font, (uint32_t)codepoint);
        if (!glyph) continue;
        if (glyph->width <= 0 || glyph->height <= 0) { pen += glyph->advance * scale; continue; }
        float left = floorf(pen + glyph->bearing_x * scale);
        float top = floorf(base - glyph->bearing_top * scale);
        float right = left + ceilf(glyph->width * scale);
        float bottom = top + ceilf(glyph->height * scale);
        /* Half texel inset keeps neighbouring glyphs out of the filter. */
        float u0 = (glyph->u0 * aw + 0.5f) / aw, v0 = (glyph->v0 * ah + 0.5f) / ah;
        float u1 = (glyph->u1 * aw - 0.5f) / aw, v1 = (glyph->v1 * ah - 0.5f) / ah;
        VkUiVertex quad[4] = {
            { left,  top,    red, green, blue, alpha, u0, v0, 0.0f },
            { right, top,    red, green, blue, alpha, u1, v0, 0.0f },
            { right, bottom, red, green, blue, alpha, u1, v1, 0.0f },
            { left,  bottom, red, green, blue, alpha, u0, v1, 0.0f },
        };
        vk_ui_quad(quad, 1);
        pen += glyph->advance * scale;
    }
}
