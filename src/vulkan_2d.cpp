/*
 * The C++ half of Enjoer: a deliberately tiny 2D Vulkan renderer.
 *
 * The Vulkan path is compiled when ENJOER_USE_VULKAN is enabled.  Keeping
 * the preview fallback in this translation unit is useful on a fresh
 * checkout: the C game can be tested on a host without a GPU, while
 * Android/device builds present the same triangle batch through Vulkan and
 * the SPIR-V shaders from src/shaders/.
 *
 * There is no 3D here on purpose.  DimScript is a language for 2D games:
 * scripts draw in screen pixels, the batch is presented as-is, and the only
 * matrix in the building maps those pixels onto the swapchain image.
 */
#include "renderer.h"
#include "ds_image.h"
#include "ds_ttf.h"
#include "surface_transform.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

#ifdef __ANDROID__
#include <android/native_window.h>
#endif

#ifndef ENJOER_USE_VULKAN
#define ENJOER_USE_VULKAN 0
#endif

#if ENJOER_USE_VULKAN
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>
#include "shaders/draw_spv.h"
#endif

namespace {

/* The 2D batch vertex: position in screen pixels, tint colour, texture
 * coordinate and array layer (negative = untextured shape). */
using Vertex = EnjoerVertex;

static uint32_t rgba(int r, int g, int b) {
    auto byte = [](int value) -> uint32_t {
        return static_cast<uint32_t>(std::max(0, std::min(255, value)));
    };
    return byte(r) | (byte(g) << 8) | (byte(b) << 16) | 0xff000000u;
}

/* Bilinear sample of a sprite sheet layer, clamped to the edge.  The GPU side
 * uses a linear filter with clamp-to-edge, so the two rasterizers agree. */
static void sample_image(const DsImage *image, float u, float v, int *red, int *green, int *blue,
                         int *alpha) {
    if (!image || !image->rgba || image->width < 1 || image->height < 1) {
        *red = *green = *blue = 0;
        *alpha = 0;
        return;
    }
    float x = u * (float)image->width - 0.5f;
    float y = v * (float)image->height - 0.5f;
    const int x0 = (int)std::floor(x);
    const int y0 = (int)std::floor(y);
    const float fx = x - (float)x0;
    const float fy = y - (float)y0;
    const auto texel = [&](int px, int py, int channel) -> float {
        if (px < 0) px = 0;
        if (py < 0) py = 0;
        if (px >= image->width) px = image->width - 1;
        if (py >= image->height) py = image->height - 1;
        return (float)image->rgba[((size_t)py * (size_t)image->width + (size_t)px) * 4 + channel] /
               255.0f;
    };
    const float w00 = (1.0f - fx) * (1.0f - fy);
    const float w10 = fx * (1.0f - fy);
    const float w01 = (1.0f - fx) * fy;
    const float w11 = fx * fy;
    const auto mix = [&](int channel) -> int {
        const float value = texel(x0, y0, channel) * w00 + texel(x0 + 1, y0, channel) * w10 +
                            texel(x0, y0 + 1, channel) * w01 + texel(x0 + 1, y0 + 1, channel) * w11;
        return (int)(value * 255.0f + 0.5f);
    };
    *red = mix(0);
    *green = mix(1);
    *blue = mix(2);
    *alpha = mix(3);
}

/* Rasterizes one triangle from the script batch.  The batch is in pixels, so
 * this is a plain screen-space walk with the same interpolation the fragment
 * shader does: tint colour, texture coordinate, array layer.  Triangles arrive
 * in script order and paint over each other — the painter's algorithm the GPU
 * pipeline mirrors with alpha blending. */
static void fill_pixel_triangle(Buffer *buffer, const EnjoerVertex &a, const EnjoerVertex &b,
                                const EnjoerVertex &c) {
    if (!buffer || !buffer->pixels) return;
    int left = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x})));
    int right = std::min(buffer->width, (int)std::ceil(std::max({a.x, b.x, c.x})));
    int top = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y})));
    int bottom = std::min(buffer->height, (int)std::ceil(std::max({a.y, b.y, c.y})));
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 0.000001f) return;

    const int tint_red = (int)(255.0f * a.r + 0.5f);
    const int tint_green = (int)(255.0f * a.g + 0.5f);
    const int tint_blue = (int)(255.0f * a.b + 0.5f);
    const int layer = static_cast<int>(std::lround(a.layer));
    const DsImage *image = layer >= 0 ? ds_image_at(layer) : nullptr;
    for (int py = top; py < bottom; ++py) {
        const float y = static_cast<float>(py) + 0.5f;
        uint32_t *row = buffer->pixels + static_cast<size_t>(py) * buffer->stride;
        for (int px = left; px < right; ++px) {
            const float x = static_cast<float>(px) + 0.5f;
            const float wa = ((b.x - x) * (c.y - y) - (b.y - y) * (c.x - x)) / area;
            const float wb = ((c.x - x) * (a.y - y) - (c.y - y) * (a.x - x)) / area;
            const float wc = 1.0f - wa - wb;
            if (wa < 0.0f || wb < 0.0f || wc < 0.0f) continue;
            int red = tint_red;
            int green = tint_green;
            int blue = tint_blue;
            int alpha = 255;
            if (image) {
                const float u = wa * a.u + wb * b.u + wc * c.u;
                const float v = wa * a.v + wb * b.v + wc * c.v;
                sample_image(image, u, v, &red, &green, &blue, &alpha);
                red = red * tint_red / 255;
                green = green * tint_green / 255;
                blue = blue * tint_blue / 255;
            }
            uint32_t *target = &row[px];
            if (alpha >= 255) {
                *target = rgba(red, green, blue);
            } else if (alpha > 0) {
                const uint32_t previous = *target;
                const auto blended = [&](int shift, int source) -> int {
                    const int under = (int)((previous >> shift) & 0xFFu);
                    return (source * alpha + under * (255 - alpha)) / 255;
                };
                *target = rgba(blended(0, red), blended(8, green), blended(16, blue));
            }
        }
    }
}

static void draw_batch(Buffer *buffer, const EnjoerFrame *frame) {
    if (!buffer || !frame || !frame->vertices) return;
    for (int index = 0; index + 2 < frame->vertex_count; index += 3)
        fill_pixel_triangle(buffer, frame->vertices[index], frame->vertices[index + 1],
                            frame->vertices[index + 2]);
}

static void render_fallback(Buffer *buffer, const EnjoerFrame *frame) {
    if (!buffer || !buffer->pixels || buffer->width < 1 || buffer->height < 1) return;
    const float clear_r = frame ? frame->clear_color[0] : 0.05f;
    const float clear_g = frame ? frame->clear_color[1] : 0.08f;
    const float clear_b = frame ? frame->clear_color[2] : 0.15f;
    const uint32_t background = rgba(static_cast<int>(255 * clear_r),
                                     static_cast<int>(255 * clear_g),
                                     static_cast<int>(255 * clear_b));
    for (int y = 0; y < buffer->height; ++y) {
        uint32_t *row = buffer->pixels + static_cast<size_t>(y) * buffer->stride;
        for (int x = 0; x < buffer->width; ++x) row[x] = background;
    }
    draw_batch(buffer, frame);
}

#if ENJOER_USE_VULKAN

/* A compact column-major 4 by 4 matrix, matching GLSL's mat4 layout. */
struct Mat4 { float value[16]{}; };

/* Zero-initialise a Vulkan struct and set its sType. */
template <class T> static T vk_struct(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

class VulkanRenderer {
public:
    bool init(void *native_window, int width, int height) {
        width_ = std::max(1, width);
        height_ = std::max(1, height);
        rotation_ = ENJOER_SURFACE_ROTATE_0;
        if (!create_instance()) return false;
        if (!create_surface(native_window)) return false;
        if (!pick_device()) return false;
        if (!create_device()) return false;
        if (!create_swapchain()) return false;
        if (!create_render_pass()) return false;
        if (!create_framebuffers()) return false;
        if (!create_pipeline()) return false;
        if (!create_sampler()) return false;
        if (!create_batch_buffer()) return false;
        if (!create_commands()) return false;
        if (!create_sync()) return false;
        initialized_ = true;
        return true;
    }

    void resize(int width, int height) {
        width_ = std::max(1, width);
        height_ = std::max(1, height);
        /* The question is never "did the numbers change" but "did the
         * framebuffer change": a phone turned upside down keeps its window
         * size and still needs new images, while a tap that lets the system
         * bars peek fires a resize of the size we already have — and
         * recreating a swapchain for nothing is a hitch the player sees as the
         * picture jumping under the finger. */
        if (initialized_ && surface_changed()) recreate_swapchain();
    }

    void render(const EnjoerFrame *frame) {
        if (!initialized_ || swapchain_ == VK_NULL_HANDLE) return;
        frame_ = frame;

        vkWaitForFences(device_, 1, &in_flight_, VK_TRUE, UINT64_MAX);
        uint32_t image = 0;
        VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                  image_available_, VK_NULL_HANDLE, &image);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return; }
        if (acquired == VK_SUBOPTIMAL_KHR && surface_changed()) {
            /* SUBOPTIMAL is how a driver says "I would rather you asked
             * differently".  Asking is only worth a frame of stutter when the
             * surface really moved: otherwise the next present says it again
             * and the swapchain is rebuilt every frame, which is what a game
             * that jitters under the finger looks like.  Recreating does
             * invalidate the image just acquired, so this frame is skipped —
             * and when nothing changed, the frame is presented as it is. */
            recreate_swapchain();
            return;
        }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return;
        vkResetFences(device_, 1, &in_flight_);

        upload_batch();
        refresh_textures();
        record(image);

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = vk_struct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &image_available_;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffers_[image];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_finished_;
        if (vkQueueSubmit(queue_, 1, &submit, in_flight_) != VK_SUCCESS) return;

        VkPresentInfoKHR present = vk_struct<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished_;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image;
        const VkResult presented = vkQueuePresentKHR(queue_, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR) recreate_swapchain();
        else if (presented == VK_SUBOPTIMAL_KHR && surface_changed()) recreate_swapchain();
    }

    void shutdown() {
        if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
        destroy_swapchain_objects();
        if (device_ != VK_NULL_HANDLE) {
            if (in_flight_) vkDestroyFence(device_, in_flight_, nullptr);
            if (render_finished_) vkDestroySemaphore(device_, render_finished_, nullptr);
            if (image_available_) vkDestroySemaphore(device_, image_available_, nullptr);
            if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
            if (batch_mapped_) vkUnmapMemory(device_, batch_memory_);
            if (batch_buffer_) vkDestroyBuffer(device_, batch_buffer_, nullptr);
            if (batch_memory_) vkFreeMemory(device_, batch_memory_, nullptr);
            destroy_textures();
            if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
            if (descriptor_layout_) vkDestroyDescriptorSetLayout(device_, descriptor_layout_, nullptr);
            if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
            if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
            if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        *this = VulkanRenderer{};
    }

private:
    bool create_instance() {
        VkApplicationInfo app = vk_struct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.pApplicationName = "Enjoer";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.pEngineName = "Enjoer 2D";
        app.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        app.apiVersion = VK_API_VERSION_1_0;
        std::vector<const char *> extensions = {VK_KHR_SURFACE_EXTENSION_NAME};
#ifdef __ANDROID__
        extensions.push_back(VK_KHR_ANDROID_SURFACE_EXTENSION_NAME);
#endif
        VkInstanceCreateInfo info = vk_struct<VkInstanceCreateInfo>(VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO);
        info.pApplicationInfo = &app;
        info.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        info.ppEnabledExtensionNames = extensions.data();
        return vkCreateInstance(&info, nullptr, &instance_) == VK_SUCCESS;
    }

    bool create_surface(void *native_window) {
#ifdef __ANDROID__
        VkAndroidSurfaceCreateInfoKHR info = vk_struct<VkAndroidSurfaceCreateInfoKHR>(VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR);
        info.window = static_cast<ANativeWindow *>(native_window);
        return vkCreateAndroidSurfaceKHR(instance_, &info, nullptr, &surface_) == VK_SUCCESS;
#else
        (void)native_window;
        return false;
#endif
    }

    bool pick_device() {
        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance_, &count, nullptr);
        if (!count) return false;
        std::vector<VkPhysicalDevice> devices(count);
        vkEnumeratePhysicalDevices(instance_, &count, devices.data());
        for (VkPhysicalDevice candidate : devices) {
            uint32_t families = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
            std::vector<VkQueueFamilyProperties> props(families);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, props.data());
            for (uint32_t i = 0; i < families; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(candidate, i, surface_, &present);
                if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    physical_ = candidate;
                    queue_family_ = i;
                    return true;
                }
            }
        }
        return false;
    }

    bool create_device() {
        const float priority = 1.0f;
        VkDeviceQueueCreateInfo queue = vk_struct<VkDeviceQueueCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO);
        queue.queueFamilyIndex = queue_family_;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;
        const char *extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
        VkDeviceCreateInfo info = vk_struct<VkDeviceCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO);
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        info.enabledExtensionCount = 1;
        info.ppEnabledExtensionNames = extensions;
        if (vkCreateDevice(physical_, &info, nullptr, &device_) != VK_SUCCESS) return false;
        vkGetDeviceQueue(device_, queue_family_, 0, &queue_);
        return true;
    }

    bool create_swapchain() {
        VkSurfaceCapabilitiesKHR caps{};
        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps);
        uint32_t format_count = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, nullptr);
        if (!format_count) return false;
        std::vector<VkSurfaceFormatKHR> formats(format_count);
        vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, formats.data());
        VkSurfaceFormatKHR chosen = formats[0];
        for (const VkSurfaceFormatKHR &format : formats)
            if (format.format == VK_FORMAT_R8G8B8A8_UNORM ||
                format.format == VK_FORMAT_B8G8R8A8_UNORM) { chosen = format; break; }
        if (chosen.format == VK_FORMAT_UNDEFINED) chosen.format = VK_FORMAT_R8G8B8A8_UNORM;
        surface_format_ = chosen.format;

        int rotation = ENJOER_SURFACE_ROTATE_0;
        swapchain_size(caps, &extent_, &rotation);
        if (extent_.width == 0 || extent_.height == 0) return false;
        rotation_ = rotation;

        uint32_t image_count = caps.minImageCount + 1;
        if (caps.maxImageCount && image_count > caps.maxImageCount)
            image_count = caps.maxImageCount;

        VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        if (!(caps.supportedCompositeAlpha & alpha))
            alpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;

        VkSwapchainCreateInfoKHR info = vk_struct<VkSwapchainCreateInfoKHR>(VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR);
        info.surface = surface_;
        info.minImageCount = image_count;
        info.imageFormat = chosen.format;
        info.imageColorSpace = chosen.colorSpace;
        info.imageExtent = extent_;
        info.imageArrayLayers = 1;
        info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        /* Telling the compositor the images are already rotated is what makes it
         * skip its own rotation — and it is a promise: the projection has to keep
         * it.  `rotation` is in degrees here and Vulkan indexes its transforms, so
         * the value goes through the translation, never through a cast. */
        info.preTransform =
            static_cast<VkSurfaceTransformFlagBitsKHR>(enjoer_surface_rotation_to_vk(rotation));
        info.compositeAlpha = alpha;
        info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        info.clipped = VK_TRUE;
        if (vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_) != VK_SUCCESS) return false;

        uint32_t count = 0;
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
        images_.resize(count);
        vkGetSwapchainImagesKHR(device_, swapchain_, &count, images_.data());
        image_views_.assign(count, VK_NULL_HANDLE);
        for (uint32_t i = 0; i < count; ++i) {
            VkImageViewCreateInfo view = vk_struct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
            view.image = images_[i];
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = chosen.format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (vkCreateImageView(device_, &view, nullptr, &image_views_[i]) != VK_SUCCESS)
                return false;
        }
        return true;
    }

    bool create_render_pass() {
        VkAttachmentDescription attachment{};
        attachment.format = surface_format_;
        attachment.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo info = vk_struct<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
        info.attachmentCount = 1;
        info.pAttachments = &attachment;
        info.subpassCount = 1;
        info.pSubpasses = &subpass;
        info.dependencyCount = 1;
        info.pDependencies = &dependency;
        return vkCreateRenderPass(device_, &info, nullptr, &render_pass_) == VK_SUCCESS;
    }

    uint32_t find_memory(uint32_t type_bits, VkMemoryPropertyFlags wanted) const {
        VkPhysicalDeviceMemoryProperties props{};
        vkGetPhysicalDeviceMemoryProperties(physical_, &props);
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
            if ((type_bits & (1u << i)) &&
                (props.memoryTypes[i].propertyFlags & wanted) == wanted) return i;
        return UINT32_MAX;
    }

    bool create_framebuffers() {
        framebuffers_.assign(image_views_.size(), VK_NULL_HANDLE);
        for (size_t i = 0; i < image_views_.size(); ++i) {
            VkFramebufferCreateInfo info = vk_struct<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            info.renderPass = render_pass_;
            info.attachmentCount = 1;
            info.pAttachments = &image_views_[i];
            info.width = extent_.width;
            info.height = extent_.height;
            info.layers = 1;
            if (vkCreateFramebuffer(device_, &info, nullptr, &framebuffers_[i]) != VK_SUCCESS)
                return false;
        }
        return true;
    }

    VkShaderModule make_shader(const uint32_t *code, size_t bytes) const {
        VkShaderModuleCreateInfo info = vk_struct<VkShaderModuleCreateInfo>(VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO);
        info.codeSize = bytes;
        info.pCode = code;
        VkShaderModule module = VK_NULL_HANDLE;
        vkCreateShaderModule(device_, &info, nullptr, &module);
        return module;
    }

    /* One pipeline for the 2D batch: no culling, no depth test, painter's
     * order with alpha blending.  The script decides what covers what by the
     * order it draws in, on the GPU exactly like in the software fallback. */
    bool create_pipeline() {
        if (descriptor_layout_ == VK_NULL_HANDLE) {
            /* One binding: the array of every image the game loaded. */
            VkDescriptorSetLayoutBinding binding{};
            binding.binding = 0;
            binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            binding.descriptorCount = 1;
            binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            VkDescriptorSetLayoutCreateInfo descriptor = vk_struct<VkDescriptorSetLayoutCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO);
            descriptor.bindingCount = 1;
            descriptor.pBindings = &binding;
            if (vkCreateDescriptorSetLayout(device_, &descriptor, nullptr, &descriptor_layout_) != VK_SUCCESS)
                return false;
        }
        if (pipeline_layout_ == VK_NULL_HANDLE) {
            VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
            VkPipelineLayoutCreateInfo layout = vk_struct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
            layout.setLayoutCount = 1;
            layout.pSetLayouts = &descriptor_layout_;
            layout.pushConstantRangeCount = 1;
            layout.pPushConstantRanges = &push;
            if (vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_) != VK_SUCCESS)
                return false;
        }

        VkShaderModule vert = make_shader(draw_vert_spv, sizeof(draw_vert_spv));
        VkShaderModule frag = make_shader(draw_frag_spv, sizeof(draw_frag_spv));
        if (!vert || !frag) {
            if (vert) vkDestroyShaderModule(device_, vert, nullptr);
            if (frag) vkDestroyShaderModule(device_, frag, nullptr);
            return false;
        }
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";

        VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
        VkVertexInputAttributeDescription attributes[4]{};
        attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 0}; /* position */
        attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3}; /* colour   */
        attributes[2] = {2, 0, VK_FORMAT_R32G32_SFLOAT, sizeof(float) * 6};    /* uv       */
        attributes[3] = {3, 0, VK_FORMAT_R32_SFLOAT, sizeof(float) * 8};       /* layer    */
        VkPipelineVertexInputStateCreateInfo vertex_input = vk_struct<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount = 4;
        vertex_input.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo assembly = vk_struct<VkPipelineInputAssemblyStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO);
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport = vk_struct<VkPipelineViewportStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO);
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;
        const VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic = vk_struct<VkPipelineDynamicStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO);
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        VkPipelineRasterizationStateCreateInfo raster = vk_struct<VkPipelineRasterizationStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO);
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        /* Y is flipped in the projection, so counter-clockwise data becomes
         * clockwise on screen. */
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample = vk_struct<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth = vk_struct<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        depth.depthTestEnable = VK_FALSE;
        depth.depthWriteEnable = VK_FALSE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        /* Painter's order: sprites and shapes are drawn in script order and
         * alpha blends over what is already there. */
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        VkPipelineColorBlendStateCreateInfo blend = vk_struct<VkPipelineColorBlendStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO);
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;

        VkGraphicsPipelineCreateInfo info = vk_struct<VkGraphicsPipelineCreateInfo>(VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO);
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vertex_input;
        info.pInputAssemblyState = &assembly;
        info.pViewportState = &viewport;
        info.pRasterizationState = &raster;
        info.pMultisampleState = &multisample;
        info.pDepthStencilState = &depth;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = pipeline_layout_;
        info.renderPass = render_pass_;
        info.subpass = 0;
        const VkResult result = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info,
                                                          nullptr, &pipeline_);
        vkDestroyShaderModule(device_, vert, nullptr);
        vkDestroyShaderModule(device_, frag, nullptr);
        return result == VK_SUCCESS;
    }

    /* One persistent host-visible buffer is enough for a 2D game: the fence we
     * wait on before recording guarantees the previous frame is finished. */
    bool create_batch_buffer() {
        VkBufferCreateInfo info = vk_struct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = sizeof(Vertex) * ENJOER_DRAW_MAX_VERTICES;
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &info, nullptr, &batch_buffer_) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, batch_buffer_, &requirements);
        VkMemoryAllocateInfo alloc = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = find_memory(requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(device_, &alloc, nullptr, &batch_memory_) != VK_SUCCESS) return false;
        vkBindBufferMemory(device_, batch_buffer_, batch_memory_, 0);
        return vkMapMemory(device_, batch_memory_, 0, info.size, 0, &batch_mapped_) == VK_SUCCESS;
    }

    void upload_batch() {
        batch_vertices_ = 0;
        if (!frame_ || !batch_mapped_ || frame_->vertex_count < 3) return;
        const int count = frame_->vertex_count < ENJOER_DRAW_MAX_VERTICES
                              ? frame_->vertex_count
                              : ENJOER_DRAW_MAX_VERTICES;
        std::memcpy(batch_mapped_, frame_->vertices, static_cast<size_t>(count) * sizeof(Vertex));
        batch_vertices_ = count;
    }

    /* Maps the pixel coordinates a script draws in onto Vulkan clip space: the
     * plain 2D ortho, with the surface rotation baked in so the panel gets the
     * picture already turned the way its compositor would have.  Both halves are
     * in src/surface_transform.h, where a host test can reach them. */
    Mat4 compute_ortho() const {
        Mat4 matrix{};
        enjoer_surface_ortho(rotation_, static_cast<float>(extent_.width),
                             static_cast<float>(extent_.height), matrix.value);
        return matrix;
    }

    /* The framebuffer this window needs, as the surface reports it: the
     * rotation to honour and the size to create, clamped into what the driver
     * accepts.  The window size (display orientation) is what the game and its
     * touches are measured in, so it is what decides — currentExtent is
     * deliberately not trusted for this, because drivers disagree about which
     * of the two spaces it is stated in. */
    void swapchain_size(const VkSurfaceCapabilitiesKHR &caps, VkExtent2D *out_extent,
                        int *out_rotation) const {
        const int rotation = enjoer_surface_rotation_for(static_cast<int>(caps.currentTransform),
                                                          static_cast<int>(caps.supportedTransforms));
        int width = 1, height = 1;
        enjoer_surface_framebuffer(rotation, width_, height_,
                                   static_cast<int>(caps.minImageExtent.width),
                                   static_cast<int>(caps.minImageExtent.height),
                                   static_cast<int>(caps.maxImageExtent.width),
                                   static_cast<int>(caps.maxImageExtent.height), &width, &height);
        out_extent->width = static_cast<uint32_t>(width);
        out_extent->height = static_cast<uint32_t>(height);
        *out_rotation = rotation;
    }

    /* True when the surface now wants a framebuffer other than the live one. */
    bool surface_changed() const {
        if (device_ == VK_NULL_HANDLE || physical_ == VK_NULL_HANDLE || surface_ == VK_NULL_HANDLE)
            return false;
        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS)
            return false;
        VkExtent2D want{};
        int rotation = ENJOER_SURFACE_ROTATE_0;
        swapchain_size(caps, &want, &rotation);
        return want.width != extent_.width || want.height != extent_.height ||
               rotation != rotation_;
    }

    /* --- images (sprite sheets) ------------------------------------------ */

    bool create_sampler() {
        VkSamplerCreateInfo info = vk_struct<VkSamplerCreateInfo>(VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO);
        /* Linear + clamp-to-edge matches the CPU sampler of the host build, so a
         * sprite looks the same in a test dump and on a phone. */
        info.magFilter = VK_FILTER_LINEAR;
        info.minFilter = VK_FILTER_LINEAR;
        info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        info.maxLod = 0.0f;
        return vkCreateSampler(device_, &info, nullptr, &sampler_) == VK_SUCCESS;
    }

    /* Two very different jobs hide behind "the textures changed": a new image
     * means the texture array itself has to be rebuilt, while a font that just
     * baked one glyph means one layer of the array that already exists has to
     * have its pixels copied in.  Treating the second like the first is what
     * made a score that gains a digit tear down and re-create every texture and
     * wait for the queue behind it — a frame of stutter on exactly the tap the
     * player was aiming at. */
    void refresh_textures() {
        const uint64_t generation = ds_image_generation();
        if (texture_view_ == VK_NULL_HANDLE || generation != texture_generation_) {
            create_textures();
            return;
        }
        const uint32_t dirty = ds_image_dirty_mask();
        if (!dirty) return;
        if (upload_layers(layer_width_, layer_height_, dirty)) ds_image_clear_dirty();
    }

    void create_textures() {
        destroy_textures();
        const int32_t count = ds_image_count();
        texture_layers_ = count > 0 ? count : 1;
        int32_t layer_width = 1;
        int32_t layer_height = 1;
        for (int32_t index = 0; index < count; ++index) {
            const DsImage *image = ds_image_at(index);
            if (!image) continue;
            if (image->width > layer_width) layer_width = image->width;
            if (image->height > layer_height) layer_height = image->height;
        }

        VkImageCreateInfo image_info = vk_struct<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {static_cast<uint32_t>(layer_width), static_cast<uint32_t>(layer_height), 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = static_cast<uint32_t>(texture_layers_);
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &image_info, nullptr, &texture_image_) != VK_SUCCESS) return;

        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, texture_image_, &requirements);
        VkMemoryAllocateInfo allocation = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = find_memory(requirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (allocation.memoryTypeIndex == UINT32_MAX)
            allocation.memoryTypeIndex = find_memory(requirements.memoryTypeBits, 0);
        if (allocation.memoryTypeIndex == UINT32_MAX) return;
        if (vkAllocateMemory(device_, &allocation, nullptr, &texture_memory_) != VK_SUCCESS) return;
        if (vkBindImageMemory(device_, texture_image_, texture_memory_, 0) != VK_SUCCESS) return;

        VkImageViewCreateInfo view = vk_struct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        view.image = texture_image_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        view.format = VK_FORMAT_R8G8B8A8_UNORM;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                 static_cast<uint32_t>(texture_layers_)};
        if (vkCreateImageView(device_, &view, nullptr, &texture_view_) != VK_SUCCESS) return;

        VkDescriptorPoolSize pool_size{};
        pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        pool_size.descriptorCount = 1;
        VkDescriptorPoolCreateInfo pool = vk_struct<VkDescriptorPoolCreateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO);
        pool.maxSets = 1;
        pool.poolSizeCount = 1;
        pool.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(device_, &pool, nullptr, &descriptor_pool_) != VK_SUCCESS) return;
        VkDescriptorSetAllocateInfo allocate = vk_struct<VkDescriptorSetAllocateInfo>(VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO);
        allocate.descriptorPool = descriptor_pool_;
        allocate.descriptorSetCount = 1;
        allocate.pSetLayouts = &descriptor_layout_;
        if (vkAllocateDescriptorSets(device_, &allocate, &descriptor_set_) != VK_SUCCESS) return;
        VkDescriptorImageInfo descriptor_image{sampler_, texture_view_,
                                               VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write = vk_struct<VkWriteDescriptorSet>(VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET);
        write.dstSet = descriptor_set_;
        write.dstBinding = 0;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.descriptorCount = 1;
        write.pImageInfo = &descriptor_image;
        vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

        layer_width_ = layer_width;
        layer_height_ = layer_height;
        upload_layers(layer_width, layer_height,
                      count >= 32 ? 0xFFFFFFFFu : ((1u << count) - 1u));
        ds_image_clear_dirty();
        texture_generation_ = ds_image_generation();
        app_log("Enjoer: %d image(s) uploaded as a %dx%d texture array", count, layer_width,
                layer_height);
    }

    /* Copies every layer named in `mask` into the array: a glyph added to one
     * atlas is one small upload, not a rebuild of the texture set. */
    bool upload_layers(int32_t layer_width, int32_t layer_height, uint32_t mask) {
        if (!command_pool_ || texture_image_ == VK_NULL_HANDLE) return false;
        const VkDeviceSize layer_bytes =
            static_cast<VkDeviceSize>(layer_width) * static_cast<VkDeviceSize>(layer_height) * 4u;
        VkBufferCreateInfo buffer_info = vk_struct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        buffer_info.size = layer_bytes;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        void *mapped = nullptr;
        if (vkCreateBuffer(device_, &buffer_info, nullptr, &staging) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, staging, &requirements);
        VkMemoryAllocateInfo allocation = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = find_memory(requirements.memoryTypeBits,
                                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (allocation.memoryTypeIndex == UINT32_MAX ||
            vkAllocateMemory(device_, &allocation, nullptr, &staging_memory) != VK_SUCCESS ||
            vkBindBufferMemory(device_, staging, staging_memory, 0) != VK_SUCCESS ||
            vkMapMemory(device_, staging_memory, 0, layer_bytes, 0, &mapped) != VK_SUCCESS) {
            if (staging) vkDestroyBuffer(device_, staging, nullptr);
            if (staging_memory) vkFreeMemory(device_, staging_memory, nullptr);
            return false;
        }

        VkCommandBufferAllocateInfo command_allocation = vk_struct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        command_allocation.commandPool = command_pool_;
        command_allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocation.commandBufferCount = 1;
        VkCommandBuffer command = VK_NULL_HANDLE;
        vkAllocateCommandBuffers(device_, &command_allocation, &command);
        VkCommandBufferBeginInfo begin = vk_struct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(command, &begin);

        VkImageMemoryBarrier to_transfer = vk_struct<VkImageMemoryBarrier>(VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER);
        to_transfer.srcAccessMask = 0;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = texture_image_;
        to_transfer.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                        static_cast<uint32_t>(texture_layers_)};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &to_transfer);

        for (int32_t index = 0; index < ds_image_count(); ++index) {
            const DsImage *image = ds_image_at(index);
            if (!image || !image->rgba) continue;
            if (!(mask & (1u << static_cast<uint32_t>(index)))) continue;
            /* Zero the padding, then copy row by row: a smaller image keeps its
             * transparent border instead of repeating the last row. */
            std::memset(mapped, 0, static_cast<size_t>(layer_bytes));
            for (int32_t row = 0; row < image->height; ++row)
                std::memcpy(static_cast<uint8_t *>(mapped) + static_cast<size_t>(row) * layer_width * 4,
                            image->rgba + static_cast<size_t>(row) * image->width * 4,
                            static_cast<size_t>(image->width) * 4);
            /* Vulkan gained an sType member on VkBufferImageCopy over time, so
             * the struct is zeroed and the member is set only when the headers
             * of the build know it. */
            VkBufferImageCopy copy{};
#ifdef VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY
            copy.sType = VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY;
#endif
            copy.bufferOffset = 0;
            copy.bufferRowLength = static_cast<uint32_t>(layer_width);
            copy.bufferImageHeight = static_cast<uint32_t>(layer_height);
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, static_cast<uint32_t>(index), 1};
            copy.imageOffset = {0, 0, 0};
            copy.imageExtent = {static_cast<uint32_t>(image->width),
                                static_cast<uint32_t>(image->height), 1};
            vkCmdCopyBufferToImage(command, staging, texture_image_,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        }

        VkImageMemoryBarrier to_shader = to_transfer;
        to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &to_shader);
        vkEndCommandBuffer(command);
        VkSubmitInfo submit = vk_struct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        if (vkQueueSubmit(queue_, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS)
            vkQueueWaitIdle(queue_);
        vkFreeCommandBuffers(device_, command_pool_, 1, &command);
        vkUnmapMemory(device_, staging_memory);
        vkDestroyBuffer(device_, staging, nullptr);
        vkFreeMemory(device_, staging_memory, nullptr);
        return true;
    }

    void destroy_textures() {
        if (device_ == VK_NULL_HANDLE) return;
        if (descriptor_pool_) vkDestroyDescriptorPool(device_, descriptor_pool_, nullptr);
        descriptor_pool_ = VK_NULL_HANDLE;
        descriptor_set_ = VK_NULL_HANDLE;
        if (texture_view_) vkDestroyImageView(device_, texture_view_, nullptr);
        if (texture_image_) vkDestroyImage(device_, texture_image_, nullptr);
        if (texture_memory_) vkFreeMemory(device_, texture_memory_, nullptr);
        texture_view_ = VK_NULL_HANDLE;
        texture_image_ = VK_NULL_HANDLE;
        texture_memory_ = VK_NULL_HANDLE;
        texture_generation_ = 0;
        texture_layers_ = 0;
    }

    bool create_commands() {
        VkCommandPoolCreateInfo pool = vk_struct<VkCommandPoolCreateInfo>(VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO);
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = queue_family_;
        if (vkCreateCommandPool(device_, &pool, nullptr, &command_pool_) != VK_SUCCESS)
            return false;
        return allocate_command_buffers();
    }

    bool allocate_command_buffers() {
        command_buffers_.assign(images_.size(), VK_NULL_HANDLE);
        VkCommandBufferAllocateInfo alloc = vk_struct<VkCommandBufferAllocateInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO);
        alloc.commandPool = command_pool_;
        alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        alloc.commandBufferCount = static_cast<uint32_t>(command_buffers_.size());
        return vkAllocateCommandBuffers(device_, &alloc, command_buffers_.data()) == VK_SUCCESS;
    }

    bool create_sync() {
        VkSemaphoreCreateInfo semaphore = vk_struct<VkSemaphoreCreateInfo>(VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO);
        VkFenceCreateInfo fence = vk_struct<VkFenceCreateInfo>(VK_STRUCTURE_TYPE_FENCE_CREATE_INFO);
        fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        return vkCreateSemaphore(device_, &semaphore, nullptr, &image_available_) == VK_SUCCESS &&
               vkCreateSemaphore(device_, &semaphore, nullptr, &render_finished_) == VK_SUCCESS &&
               vkCreateFence(device_, &fence, nullptr, &in_flight_) == VK_SUCCESS;
    }

    void record(uint32_t image) {
        VkCommandBuffer cmd = command_buffers_[image];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin = vk_struct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        VkClearValue clear{};
        const float *tint = frame_ ? frame_->clear_color : nullptr;
        clear.color = {{tint ? tint[0] : 0.05f, tint ? tint[1] : 0.08f,
                        tint ? tint[2] : 0.15f, 1.0f}};
        VkRenderPassBeginInfo pass = vk_struct<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        pass.renderPass = render_pass_;
        pass.framebuffer = framebuffers_[image];
        pass.renderArea = {{0, 0}, extent_};
        pass.clearValueCount = 1;
        pass.pClearValues = &clear;
        vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent_.width),
                            static_cast<float>(extent_.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent_};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        if (batch_vertices_ >= 3) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1,
                                    &descriptor_set_, 0, nullptr);
            const VkDeviceSize offset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &batch_buffer_, &offset);
            const Mat4 ortho = compute_ortho();
            vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                               sizeof(Mat4), ortho.value);
            vkCmdDraw(cmd, static_cast<uint32_t>(batch_vertices_), 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
    }

    void destroy_swapchain_objects() {
        if (device_ == VK_NULL_HANDLE) return;
        for (VkFramebuffer fb : framebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        framebuffers_.clear();
        for (VkImageView view : image_views_) if (view) vkDestroyImageView(device_, view, nullptr);
        image_views_.clear();
        images_.clear();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }

    void recreate_swapchain() {
        vkDeviceWaitIdle(device_);
        const VkFormat previous_format = surface_format_;
        destroy_swapchain_objects();
        if (!create_swapchain()) return;
        if (surface_format_ != previous_format) {
            /* Rare, but the render pass and pipeline bake the colour format. */
            vkDestroyPipeline(device_, pipeline_, nullptr);
            vkDestroyRenderPass(device_, render_pass_, nullptr);
            pipeline_ = VK_NULL_HANDLE;
            render_pass_ = VK_NULL_HANDLE;
            if (!create_render_pass()) return;
            vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
            pipeline_layout_ = VK_NULL_HANDLE;
            if (!create_pipeline()) return;
        }
        if (!create_framebuffers()) return;
        if (command_buffers_.size() != images_.size()) {
            vkFreeCommandBuffers(device_, command_pool_,
                                 static_cast<uint32_t>(command_buffers_.size()),
                                 command_buffers_.data());
            allocate_command_buffers();
        }
    }

    bool initialized_ = false;
    int width_ = 1, height_ = 1;
    /* Rotation of the panel relative to the window, baked into the projection
     * (EnjoerSurfaceRotation). */
    int rotation_ = ENJOER_SURFACE_ROTATE_0;
    const EnjoerFrame *frame_ = nullptr;
    int batch_vertices_ = 0;
    VkDescriptorSetLayout descriptor_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet descriptor_set_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkImage texture_image_ = VK_NULL_HANDLE;
    VkDeviceMemory texture_memory_ = VK_NULL_HANDLE;
    VkImageView texture_view_ = VK_NULL_HANDLE;
    /* The image set the array was built for; a layer whose pixels moved is not a
     * new set, so the two must not be conflated (see refresh_textures). */
    uint64_t texture_generation_ = 0;
    int32_t texture_layers_ = 0;
    /* Layer size the array was created with: a partial re-upload needs it. */
    int32_t layer_width_ = 1, layer_height_ = 1;
    VkBuffer batch_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory batch_memory_ = VK_NULL_HANDLE;
    void *batch_mapped_ = nullptr;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat surface_format_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkExtent2D extent_{1, 1};
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;
    VkSemaphore image_available_ = VK_NULL_HANDLE;
    VkSemaphore render_finished_ = VK_NULL_HANDLE;
    VkFence in_flight_ = VK_NULL_HANDLE;
};

static VulkanRenderer vulkan_renderer;
static bool vulkan_active;
#endif

static void *native_window;
static int width = 1;
static int height = 1;

} // namespace

extern "C" int renderer_init(void *window, int w, int h) {
    native_window = window;
    width = std::max(1, w);
    height = std::max(1, h);
#if ENJOER_USE_VULKAN
    if (native_window && vulkan_renderer.init(native_window, width, height)) {
        vulkan_active = true;
        return 1;
    }
    /* A preview has no native window. A Vulkan Android build should fail
     * loudly instead of silently switching away from GPU presentation. */
    if (native_window) return 0;
#endif
    return 1;
}

extern "C" void renderer_resize(int w, int h) {
    width = std::max(1, w);
    height = std::max(1, h);
#if ENJOER_USE_VULKAN
    if (vulkan_active) vulkan_renderer.resize(width, height);
#endif
}

extern "C" void renderer_render(Buffer *preview_target, const EnjoerFrame *frame) {
    /* The text pass runs before either branch: recorded texts with loaded
     * fonts become glyph quads inside the same batch, so the GPU upload and
     * the software walk below see identical triangles. */
    ds_ttf_resolve_frame();
#if ENJOER_USE_VULKAN
    if (vulkan_active) {
        vulkan_renderer.render(frame);
        return;
    }
#endif
    if (preview_target) {
        render_fallback(preview_target, frame);
        return;
    }
#ifdef __ANDROID__
    if (native_window) {
        ANativeWindow *window = static_cast<ANativeWindow *>(native_window);
        ANativeWindow_Buffer locked{};
        if (ANativeWindow_lock(window, &locked, nullptr) == 0) {
            Buffer target{};
            target.pixels = static_cast<uint32_t *>(locked.bits);
            target.width = locked.width;
            target.height = locked.height;
            target.stride = locked.stride;
            render_fallback(&target, frame);
            ANativeWindow_unlockAndPost(window);
        }
    }
#endif
}

extern "C" void renderer_shutdown(void) {
#if ENJOER_USE_VULKAN
    if (vulkan_active) vulkan_renderer.shutdown();
    vulkan_active = false;
#endif
    native_window = nullptr;
}

extern "C" const char *renderer_backend(void) {
#if ENJOER_USE_VULKAN
    return vulkan_active ? "Vulkan" : "software 2D fallback";
#else
    return "software 2D fallback (enable Vulkan for GPU presentation)";
#endif
}
