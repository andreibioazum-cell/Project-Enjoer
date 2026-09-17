/* src/vk/vk_pipelines.c — render passes, pipelines, descriptors and the
 * internal 3D target.
 *
 * Passes:
 *   pass3d  world + hand into the internal-resolution colour/depth images,
 *           cleared every frame and handed to the HUD pass as a sampled image;
 *   pass2d  the frame sized target: an upscale of the 3D image (linear
 *           filtering, which is exactly the software renderer's bilinear
 *           upscale) followed by the HUD geometry, blended on top.
 *
 * Winding note: the projection matrix keeps +Y up and flips Y for Vulkan's
 * inverted clip space, so the counter-clockwise quads the world builder emits
 * arrive as clockwise triangles — that is why frontFace is CLOCKWISE. */
#include "vk_internal.h"
#include "../geometrium/geometrium_internal.h"
#include "../graphics/gfx_font.h"

static VkShaderModule module(const char *name) {
    for (size_t i = 0; i < VK_SHADER_MODULE_COUNT; i++) {
        if (strcmp(vk_shader_modules[i].name, name) != 0) continue;
        VkShaderModuleCreateInfo info = {0};
        info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        info.codeSize = (size_t)vk_shader_modules[i].count * sizeof(uint32_t);
        info.pCode = vk_shader_modules[i].words;
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vkCreateShaderModule(vk.device, &info, NULL, &shader) != VK_SUCCESS) {
            vk_set_error("shader module %s failed", name);
            return VK_NULL_HANDLE;
        }
        return shader;
    }
    vk_set_error("shader %s is missing from the embedded SPIR-V", name);
    return VK_NULL_HANDLE;
}

static VkFormat pick_depth_format(void) {
    const VkFormat candidates[3] = { VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM };
    for (int i = 0; i < 3; i++) {
        VkFormatProperties properties;
        vkGetPhysicalDeviceFormatProperties(vk.physical, candidates[i], &properties);
        if (properties.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) return candidates[i];
    }
    return VK_FORMAT_D32_SFLOAT;
}

static int create_render_passes(VkFormat target_format) {
    VkAttachmentDescription color = {0};
    color.format = VK_FORMAT_R8G8B8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentDescription depth = {0};
    depth.format = pick_depth_format();
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference color_reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depth_reference = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {0};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_reference;
    subpass.pDepthStencilAttachment = &depth_reference;
    VkSubpassDependency dependencies[2] = {{0}};
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkAttachmentDescription attachments[2] = { color, depth };
    VkRenderPassCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    if (vkCreateRenderPass(vk.device, &info, NULL, &vk.pass3d) != VK_SUCCESS)
        return vk_set_error("3D render pass creation failed"), 0;

    VkAttachmentDescription screen = {0};
    screen.format = target_format;
    screen.samples = VK_SAMPLE_COUNT_1_BIT;
    screen.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    screen.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    screen.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    screen.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    screen.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    screen.finalLayout = vk.windowed ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference screen_reference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription screen_subpass = {0};
    screen_subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    screen_subpass.colorAttachmentCount = 1;
    screen_subpass.pColorAttachments = &screen_reference;
    VkSubpassDependency screen_dependency = {0};
    screen_dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    screen_dependency.dstSubpass = 0;
    screen_dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    screen_dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    screen_dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    screen_dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo screen_info = {0};
    screen_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    screen_info.attachmentCount = 1;
    screen_info.pAttachments = &screen;
    screen_info.subpassCount = 1;
    screen_info.pSubpasses = &screen_subpass;
    screen_info.dependencyCount = 1;
    screen_info.pDependencies = &screen_dependency;
    if (vkCreateRenderPass(vk.device, &screen_info, NULL, &vk.pass2d) != VK_SUCCESS)
        return vk_set_error("screen render pass creation failed"), 0;
    return 1;
}

static int create_layouts(void) {
    VkDescriptorSetLayoutBinding tiles = {0};
    tiles.binding = 0;
    tiles.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    tiles.descriptorCount = 1;
    tiles.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo tiles_info = {0};
    tiles_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    tiles_info.bindingCount = 1;
    tiles_info.pBindings = &tiles;
    if (vkCreateDescriptorSetLayout(vk.device, &tiles_info, NULL, &vk.layout_tiles) != VK_SUCCESS) return 0;
    if (vkCreateDescriptorSetLayout(vk.device, &tiles_info, NULL, &vk.layout_glyphs) != VK_SUCCESS) return 0;
    if (vkCreateDescriptorSetLayout(vk.device, &tiles_info, NULL, &vk.layout_scene) != VK_SUCCESS) return 0;

    VkDescriptorPoolSize sizes[1] = {{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 }};
    VkDescriptorPoolCreateInfo pool = {0};
    pool.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool.maxSets = 3;
    pool.poolSizeCount = 1;
    pool.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(vk.device, &pool, NULL, &vk.descriptor_pool) != VK_SUCCESS)
        return vk_set_error("descriptor pool creation failed"), 0;
    VkDescriptorSetLayout layouts[3] = { vk.layout_tiles, vk.layout_glyphs, vk.layout_scene };
    VkDescriptorSet sets[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE };
    VkDescriptorSetAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocate.descriptorPool = vk.descriptor_pool;
    allocate.descriptorSetCount = 3;
    allocate.pSetLayouts = layouts;
    if (vkAllocateDescriptorSets(vk.device, &allocate, sets) != VK_SUCCESS)
        return vk_set_error("descriptor set allocation failed"), 0;
    vk.set_tiles = sets[0];
    vk.set_glyphs = sets[1];
    vk.set_scene = sets[2];

    VkSamplerCreateInfo sampler = {0};
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler.maxLod = 16.0f;
    if (vkCreateSampler(vk.device, &sampler, NULL, &vk.sampler_tiles) != VK_SUCCESS) return 0;
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0.0f;
    if (vkCreateSampler(vk.device, &sampler, NULL, &vk.sampler_glyphs) != VK_SUCCESS) return 0;
    if (vkCreateSampler(vk.device, &sampler, NULL, &vk.sampler_scene) != VK_SUCCESS) return 0;
    return 1;
}

static int create_pipeline_layouts(void) {
    VkPushConstantRange range3d = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VkPush3D) };
    VkPushConstantRange range2d = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VkPush2D) };
    VkPushConstantRange range_sky = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VkPushSky) };
    VkPushConstantRange range_line = { VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(VkPush3D) };
    VkPushConstantRange range_present = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(VkPushPresent) };
    VkDescriptorSetLayout tiles[1] = { vk.layout_tiles };
    VkDescriptorSetLayout glyphs[1] = { vk.layout_glyphs };

    VkPipelineLayoutCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts = tiles;
    info.pushConstantRangeCount = 1;
    info.pPushConstantRanges = &range3d;
    if (vkCreatePipelineLayout(vk.device, &info, NULL, &vk.layout3d) != VK_SUCCESS) return 0;
    info.pSetLayouts = glyphs;
    info.pPushConstantRanges = &range2d;
    if (vkCreatePipelineLayout(vk.device, &info, NULL, &vk.layout2d) != VK_SUCCESS) return 0;
    info.setLayoutCount = 0;
    info.pPushConstantRanges = &range_sky;
    if (vkCreatePipelineLayout(vk.device, &info, NULL, &vk.layout_sky) != VK_SUCCESS) return 0;
    info.pPushConstantRanges = &range_line;
    if (vkCreatePipelineLayout(vk.device, &info, NULL, &vk.layout_line) != VK_SUCCESS) return 0;
    info.setLayoutCount = 1;
    info.pSetLayouts = tiles;
    info.pPushConstantRanges = &range_present;
    if (vkCreatePipelineLayout(vk.device, &info, NULL, &vk.layout_present) != VK_SUCCESS) return 0;
    return 1;
}

static VkPipeline build_graphics(VkRenderPass pass, VkPipelineLayout layout, VkShaderModule vertex, VkShaderModule fragment,
                                 const VkVertexInputBindingDescription *binding, uint32_t binding_count,
                                 const VkVertexInputAttributeDescription *attributes, uint32_t attribute_count,
                                 VkPrimitiveTopology topology, int blend, int depth_test, int depth_write,
                                 int cull_back) {
    VkPipelineShaderStageCreateInfo stages[2] = {{0}};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo input = {0};
    input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    input.vertexBindingDescriptionCount = binding_count;
    input.pVertexBindingDescriptions = binding;
    input.vertexAttributeDescriptionCount = attribute_count;
    input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly = {0};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = topology;
    VkPipelineViewportStateCreateInfo viewport_state = {0};
    viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport_state.viewportCount = 1;
    viewport_state.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster = {0};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = cull_back ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
    raster.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo multisample = {0};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth = {0};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = depth_test ? VK_TRUE : VK_FALSE;
    depth.depthWriteEnable = depth_write ? VK_TRUE : VK_FALSE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState blend_attachment = {0};
    blend_attachment.blendEnable = blend ? VK_TRUE : VK_FALSE;
    blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blend_attachment.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo blending = {0};
    blending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blending.attachmentCount = 1;
    blending.pAttachments = &blend_attachment;
    VkDynamicState dynamic[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state = {0};
    dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic_state.dynamicStateCount = 2;
    dynamic_state.pDynamicStates = dynamic;
    VkGraphicsPipelineCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport_state;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blending;
    info.pDynamicState = &dynamic_state;
    info.layout = layout;
    info.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(vk.device, VK_NULL_HANDLE, 1, &info, NULL, &pipeline) != VK_SUCCESS) {
        vk_set_error("pipeline creation failed");
        return VK_NULL_HANDLE;
    }
    return pipeline;
}

static int create_pipelines(void) {
    VkShaderModule world_vert = module("world_vert");
    VkShaderModule world_frag = module("world_frag");
    VkShaderModule sky_vert = module("sky_vert");
    VkShaderModule sky_frag = module("sky_frag");
    VkShaderModule line_vert = module("line_vert");
    VkShaderModule line_frag = module("line_frag");
    VkShaderModule ui_vert = module("ui_vert");
    VkShaderModule ui_frag = module("ui_frag");
    VkShaderModule present_vert = module("present_vert");
    VkShaderModule present_frag = module("present_frag");
    if (!world_vert || !world_frag || !sky_vert || !sky_frag || !line_vert || !line_frag ||
        !ui_vert || !ui_frag || !present_vert || !present_frag) return 0;

    VkVertexInputBindingDescription world_binding = {0, sizeof(VkWorldVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription world_attributes[5] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VkWorldVertex, x)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VkWorldVertex, u)},
        {2, 0, VK_FORMAT_R8G8B8A8_UNORM, offsetof(VkWorldVertex, r)},
        {3, 0, VK_FORMAT_R8_UNORM, offsetof(VkWorldVertex, shade)},
        {4, 0, VK_FORMAT_R8_UNORM, offsetof(VkWorldVertex, layer)},
    };
    vk.pipe_world_opaque = build_graphics(vk.pass3d, vk.layout3d, world_vert, world_frag,
                                          &world_binding, 1, world_attributes, 5,
                                          VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 1, 1, 0);
    vk.pipe_world_water = build_graphics(vk.pass3d, vk.layout3d, world_vert, world_frag,
                                         &world_binding, 1, world_attributes, 5,
                                         VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 1, 1, 0);
    vk.pipe_sky = build_graphics(vk.pass3d, vk.layout_sky, sky_vert, sky_frag,
                                 NULL, 0, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, 0, 0);
    VkVertexInputBindingDescription line_binding = {0, sizeof(VkLineVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription line_attributes[2] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(VkLineVertex, x)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VkLineVertex, r)},
    };
    vk.pipe_line = build_graphics(vk.pass3d, vk.layout_line, line_vert, line_frag,
                                  &line_binding, 1, line_attributes, 2,
                                  VK_PRIMITIVE_TOPOLOGY_LINE_LIST, 0, 1, 1, 0);
    VkVertexInputBindingDescription ui_binding = {0, sizeof(VkUiVertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription ui_attributes[4] = {
        {0, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VkUiVertex, x)},
        {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(VkUiVertex, r)},
        {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(VkUiVertex, u)},
        {3, 0, VK_FORMAT_R32_SFLOAT, offsetof(VkUiVertex, layer)},
    };
    vk.pipe_ui_solid = build_graphics(vk.pass2d, vk.layout2d, ui_vert, ui_frag, &ui_binding, 1, ui_attributes, 4,
                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 0, 0, 0);
    vk.pipe_ui_glyph = build_graphics(vk.pass2d, vk.layout2d, ui_vert, ui_frag, &ui_binding, 1, ui_attributes, 4,
                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 0, 0, 0);
    vk.pipe_ui_image = build_graphics(vk.pass2d, vk.layout2d, ui_vert, ui_frag, &ui_binding, 1, ui_attributes, 4,
                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 1, 0, 0, 0);
    vk.pipe_present = build_graphics(vk.pass2d, vk.layout_present, present_vert, present_frag,
                                     NULL, 0, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, 0, 0, 0, 0);

    vkDestroyShaderModule(vk.device, world_vert, NULL);
    vkDestroyShaderModule(vk.device, world_frag, NULL);
    vkDestroyShaderModule(vk.device, sky_vert, NULL);
    vkDestroyShaderModule(vk.device, sky_frag, NULL);
    vkDestroyShaderModule(vk.device, line_vert, NULL);
    vkDestroyShaderModule(vk.device, line_frag, NULL);
    vkDestroyShaderModule(vk.device, ui_vert, NULL);
    vkDestroyShaderModule(vk.device, ui_frag, NULL);
    vkDestroyShaderModule(vk.device, present_vert, NULL);
    vkDestroyShaderModule(vk.device, present_frag, NULL);
    return vk.pipe_world_opaque && vk.pipe_world_water && vk.pipe_sky && vk.pipe_line &&
           vk.pipe_ui_solid && vk.pipe_ui_image && vk.pipe_present;
}

int vk_pipelines_init(AAssetManager *assets) {
    (void)assets;
    if (!create_render_passes(VK_FORMAT_R8G8B8A8_UNORM)) return 0;
    if (!create_layouts()) return 0;
    if (!create_pipeline_layouts()) return 0;
    if (!create_pipelines()) return 0;
    return 1;
}

void vk_pipelines_shutdown(void) {
    if (vk.device == VK_NULL_HANDLE) return;
    if (vk.scene.framebuffer) vkDestroyFramebuffer(vk.device, vk.scene.framebuffer, NULL);
    if (vk.scene.color_view) vkDestroyImageView(vk.device, vk.scene.color_view, NULL);
    if (vk.scene.color) vkDestroyImage(vk.device, vk.scene.color, NULL);
    if (vk.scene.color_memory) vkFreeMemory(vk.device, vk.scene.color_memory, NULL);
    if (vk.scene.depth_view) vkDestroyImageView(vk.device, vk.scene.depth_view, NULL);
    if (vk.scene.depth) vkDestroyImage(vk.device, vk.scene.depth, NULL);
    if (vk.scene.depth_memory) vkFreeMemory(vk.device, vk.scene.depth_memory, NULL);
    memset(&vk.scene, 0, sizeof(vk.scene));
    if (vk.pipe_world_opaque) vkDestroyPipeline(vk.device, vk.pipe_world_opaque, NULL);
    if (vk.pipe_world_water) vkDestroyPipeline(vk.device, vk.pipe_world_water, NULL);
    if (vk.pipe_sky) vkDestroyPipeline(vk.device, vk.pipe_sky, NULL);
    if (vk.pipe_line) vkDestroyPipeline(vk.device, vk.pipe_line, NULL);
    if (vk.pipe_ui_solid) vkDestroyPipeline(vk.device, vk.pipe_ui_solid, NULL);
    if (vk.pipe_ui_glyph) vkDestroyPipeline(vk.device, vk.pipe_ui_glyph, NULL);
    if (vk.pipe_ui_image) vkDestroyPipeline(vk.device, vk.pipe_ui_image, NULL);
    if (vk.pipe_present) vkDestroyPipeline(vk.device, vk.pipe_present, NULL);
    if (vk.layout3d) vkDestroyPipelineLayout(vk.device, vk.layout3d, NULL);
    if (vk.layout2d) vkDestroyPipelineLayout(vk.device, vk.layout2d, NULL);
    if (vk.layout_sky) vkDestroyPipelineLayout(vk.device, vk.layout_sky, NULL);
    if (vk.layout_line) vkDestroyPipelineLayout(vk.device, vk.layout_line, NULL);
    if (vk.layout_present) vkDestroyPipelineLayout(vk.device, vk.layout_present, NULL);
    if (vk.layout_tiles) vkDestroyDescriptorSetLayout(vk.device, vk.layout_tiles, NULL);
    if (vk.layout_glyphs) vkDestroyDescriptorSetLayout(vk.device, vk.layout_glyphs, NULL);
    if (vk.layout_scene) vkDestroyDescriptorSetLayout(vk.device, vk.layout_scene, NULL);
    if (vk.descriptor_pool) vkDestroyDescriptorPool(vk.device, vk.descriptor_pool, NULL);
    if (vk.sampler_tiles) vkDestroySampler(vk.device, vk.sampler_tiles, NULL);
    if (vk.sampler_glyphs) vkDestroySampler(vk.device, vk.sampler_glyphs, NULL);
    if (vk.sampler_scene) vkDestroySampler(vk.device, vk.sampler_scene, NULL);
    if (vk.pass3d) vkDestroyRenderPass(vk.device, vk.pass3d, NULL);
    if (vk.pass2d) vkDestroyRenderPass(vk.device, vk.pass2d, NULL);
    vk.pass3d = vk.pass2d = VK_NULL_HANDLE;
    vk.pipe_world_opaque = vk.pipe_world_water = vk.pipe_sky = vk.pipe_line = VK_NULL_HANDLE;
    vk.pipe_ui_solid = vk.pipe_ui_glyph = vk.pipe_ui_image = vk.pipe_present = VK_NULL_HANDLE;
    vk.layout3d = vk.layout2d = vk.layout_sky = vk.layout_line = vk.layout_present = VK_NULL_HANDLE;
    vk.layout_tiles = vk.layout_glyphs = vk.layout_scene = VK_NULL_HANDLE;
    vk.descriptor_pool = VK_NULL_HANDLE;
    vk.sampler_tiles = vk.sampler_glyphs = vk.sampler_scene = VK_NULL_HANDLE;
}

/* ── internal 3D target ──────────────────────────────────────────────── */

int vk_scene_target_ensure(int width, int height) {
    if (vk.scene.width == width && vk.scene.height == height && vk.scene.color) return 1;
    vkDeviceWaitIdle(vk.device);
    if (vk.scene.framebuffer) { vkDestroyFramebuffer(vk.device, vk.scene.framebuffer, NULL); vk.scene.framebuffer = VK_NULL_HANDLE; }
    if (vk.scene.color_view) vkDestroyImageView(vk.device, vk.scene.color_view, NULL);
    if (vk.scene.color) vkDestroyImage(vk.device, vk.scene.color, NULL);
    if (vk.scene.color_memory) vkFreeMemory(vk.device, vk.scene.color_memory, NULL);
    if (vk.scene.depth_view) vkDestroyImageView(vk.device, vk.scene.depth_view, NULL);
    if (vk.scene.depth) vkDestroyImage(vk.device, vk.scene.depth, NULL);
    if (vk.scene.depth_memory) vkFreeMemory(vk.device, vk.scene.depth_memory, NULL);
    memset(&vk.scene, 0, sizeof(vk.scene));
    if (width < 8 || height < 8) return 0;
    if (!vk_create_color_image(width, height, VK_FORMAT_R8G8B8A8_UNORM,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                               &vk.scene.color, &vk.scene.color_view, &vk.scene.color_memory)) return 0;
    if (!vk_create_color_image(width, height, pick_depth_format(), VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                               &vk.scene.depth, &vk.scene.depth_view, &vk.scene.depth_memory)) return 0;
    vk.scene.width = width;
    vk.scene.height = height;

    VkImageView attachments[2] = { vk.scene.color_view, vk.scene.depth_view };
    VkFramebufferCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    info.renderPass = vk.pass3d;
    info.attachmentCount = 2;
    info.pAttachments = attachments;
    info.width = (uint32_t)width;
    info.height = (uint32_t)height;
    info.layers = 1;
    if (vkCreateFramebuffer(vk.device, &info, NULL, &vk.scene.framebuffer) != VK_SUCCESS)
        return vk_set_error("scene framebuffer creation failed"), 0;

    VkDescriptorImageInfo image = {0};
    image.imageView = vk.scene.color_view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image.sampler = vk.sampler_scene;
    VkWriteDescriptorSet write = {0};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = vk.set_scene;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(vk.device, 1, &write, 0, NULL);
    return 1;
}

void vk_begin_scene_pass(void) {
    VkClearValue clears[2];
    memset(clears, 0, sizeof(clears));
    clears[0].color.float32[0] = ((vk.sky_top >> 16) & 0xff) / 255.0f;
    clears[0].color.float32[1] = ((vk.sky_top >> 8) & 0xff) / 255.0f;
    clears[0].color.float32[2] = (vk.sky_top & 0xff) / 255.0f;
    clears[0].color.float32[3] = 1.0f;
    clears[1].depthStencil.depth = 1.0f;
    VkRenderPassBeginInfo pass = {0};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = vk.pass3d;
    pass.framebuffer = vk.scene.framebuffer;
    pass.renderArea.extent.width = (uint32_t)vk.scene.width;
    pass.renderArea.extent.height = (uint32_t)vk.scene.height;
    pass.clearValueCount = 2;
    pass.pClearValues = clears;
    vkCmdBeginRenderPass(vk.cmd[vk.frame_index], &pass, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = {0, 0, (float)vk.scene.width, (float)vk.scene.height, 0.0f, 1.0f};
    vkCmdSetViewport(vk.cmd[vk.frame_index], 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {(uint32_t)vk.scene.width, (uint32_t)vk.scene.height}};
    vkCmdSetScissor(vk.cmd[vk.frame_index], 0, 1, &scissor);
    vk.pass3d_active = 1;
    vk.scene_started = 1;
}

void vk_end_scene_pass(void) {
    if (!vk.pass3d_active) return;
    vkCmdEndRenderPass(vk.cmd[vk.frame_index]);
    vk.pass3d_active = 0;
}

void vk_bind_world(int water) {
    VkCommandBuffer cmd = vk.cmd[vk.frame_index];
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, water ? vk.pipe_world_water : vk.pipe_world_opaque);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, vk.layout3d, 0, 1, &vk.set_tiles, 0, NULL);
    VkViewport viewport = {0, 0, (float)vk.scene.width, (float)vk.scene.height, 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {(uint32_t)vk.scene.width, (uint32_t)vk.scene.height}};
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    /* Water is translucent: the fragment stage multiplies its alpha by this. */
    int water_layer = geometrium_material_layer(BLOCK_WATER, 0);
    vk_push3d(water ? (float)geometrium_material_alpha(water_layer) / 255.0f : 1.0f);
}

void vk_push3d(float alpha) {
    VkPush3D push;
    memset(&push, 0, sizeof(push));
    memcpy(push.viewProj, vk.viewmodel ? vk.proj_matrix : vk.view_matrix, sizeof(push.viewProj));
    push.camPosAlpha[0] = vk.camx;
    push.camPosAlpha[1] = vk.camy;
    push.camPosAlpha[2] = vk.camz;
    push.camPosAlpha[3] = alpha;
    push.fog[0] = vk.fog_start;
    push.fog[1] = vk.fog_end > vk.fog_start ? 1.0f / (vk.fog_end - vk.fog_start) : 0.0f;
    push.fog[2] = 1.0f;
    push.fogColor[0] = ((vk.fog_color >> 16) & 0xff) / 255.0f;
    push.fogColor[1] = ((vk.fog_color >> 8) & 0xff) / 255.0f;
    push.fogColor[2] = (vk.fog_color & 0xff) / 255.0f;
    push.flags[0] = vk.viewmodel ? 1.0f : 0.0f;
    vkCmdPushConstants(vk.cmd[vk.frame_index], vk.layout3d,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
}

/* The lines of the block outline share the 3D push block; only the depth bias
 * and the viewmodel flag matter to their shader. */
void vk_push_line(void) {
    VkPush3D push;
    memset(&push, 0, sizeof(push));
    memcpy(push.viewProj, vk.viewmodel ? vk.proj_matrix : vk.view_matrix, sizeof(push.viewProj));
    push.flags[0] = vk.viewmodel ? 1.0f : 0.0f;
    push.flags[1] = 0.0015f;   /* matches the software outline's depth bias */
    vkCmdPushConstants(vk.cmd[vk.frame_index], vk.layout_line, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), &push);
}

void vk_push_sky(void) {
    VkPushSky push;
    memset(&push, 0, sizeof(push));
    push.geometry[0] = vk.foc;
    push.geometry[1] = (float)vk.scene.height;
    push.geometry[2] = vk.pitch_s;
    push.geometry[3] = vk.pitch_c;
    push.top[0] = ((vk.sky_top >> 16) & 0xff) / 255.0f;
    push.top[1] = ((vk.sky_top >> 8) & 0xff) / 255.0f;
    push.top[2] = (vk.sky_top & 0xff) / 255.0f;
    push.top[3] = 1.0f;
    push.bottom[0] = ((vk.sky_bottom >> 16) & 0xff) / 255.0f;
    push.bottom[1] = ((vk.sky_bottom >> 8) & 0xff) / 255.0f;
    push.bottom[2] = (vk.sky_bottom & 0xff) / 255.0f;
    push.bottom[3] = 1.0f;
    vkCmdPushConstants(vk.cmd[vk.frame_index], vk.layout_sky, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
}

void vk_push2d(int mode, float alpha) {
    VkPush2D push;
    memset(&push, 0, sizeof(push));
    const Font *font = gfx_font();
    push.screen[0] = (float)vk.width;
    push.screen[1] = (float)vk.height;
    push.screen[2] = (float)(font ? font_aw(font) : FONT_ATLAS_W);
    push.screen[3] = (float)(font ? font_ah(font) : FONT_ATLAS_H);
    push.tint[0] = push.tint[1] = push.tint[2] = alpha;
    push.mode[0] = (float)mode;
    vkCmdPushConstants(vk.cmd[vk.frame_index], vk.layout2d,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
}

void vk_push_present(void) {
    VkPushPresent push;
    memset(&push, 0, sizeof(push));
    push.screen[0] = (float)vk.width;
    push.screen[1] = (float)vk.height;
    push.screen[2] = (float)vk.scene.width;
    push.screen[3] = (float)vk.scene.height;
    vkCmdPushConstants(vk.cmd[vk.frame_index], vk.layout_present,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
}
