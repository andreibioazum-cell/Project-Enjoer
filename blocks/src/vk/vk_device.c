/* src/vk/vk_device.c — Vulkan instance, device, memory, uploads and targets.
 *
 * No allocator library: every buffer/image owns one vkAllocateMemory block.
 * That keeps the backend readable and stays well inside the guaranteed
 * maxMemoryAllocationCount (the world keeps at most one mesh per chunk slot).
 *
 * Two presentation paths share the same command stream:
 *   windowed (Android): VK_KHR_swapchain, images presented to the surface;
 *   offscreen (PC preview): a colour image copied into the host frame buffer,
 *   which is what the HTTP preview sends to the browser. */
#include "vk_internal.h"

#include <stdarg.h>
#include <stdio.h>

#ifndef ENJOER_VK_ANDROID
#include <dlfcn.h>          /* the loader is opened by name on every platform */
#endif

VkContext vk;

void vk_set_error(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    vsnprintf(vk.error, sizeof(vk.error), format, arguments);
    va_end(arguments);
    app_log_error("vulkan: %s", vk.error);
}

/* ── memory helpers ──────────────────────────────────────────────────── */

static uint32_t find_memory_type(uint32_t bits, VkMemoryPropertyFlags properties) {
    for (uint32_t i = 0; i < vk.memory.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (vk.memory.memoryTypes[i].propertyFlags & properties) == properties) return i;
    }
    return UINT32_MAX;
}

int vk_create_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkGpuBuffer *out) {
    if (size <= 0) size = 4;
    memset(out, 0, sizeof(*out));
    VkBufferCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(vk.device, &info, NULL, &out->buffer) != VK_SUCCESS) return vk_set_error("buffer creation failed"), 0;
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(vk.device, out->buffer, &requirements);
    uint32_t type = find_memory_type(requirements.memoryTypeBits, properties);
    if (type == UINT32_MAX) { vkDestroyBuffer(vk.device, out->buffer, NULL); out->buffer = VK_NULL_HANDLE; return vk_set_error("no memory type for a buffer"), 0; }
    VkMemoryAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(vk.device, &allocate, NULL, &out->memory) != VK_SUCCESS) {
        vkDestroyBuffer(vk.device, out->buffer, NULL); out->buffer = VK_NULL_HANDLE;
        return vk_set_error("buffer memory allocation failed"), 0;
    }
    if (vkBindBufferMemory(vk.device, out->buffer, out->memory, 0) != VK_SUCCESS) {
        vkFreeMemory(vk.device, out->memory, NULL); vkDestroyBuffer(vk.device, out->buffer, NULL);
        out->memory = VK_NULL_HANDLE; out->buffer = VK_NULL_HANDLE;
        return vk_set_error("buffer binding failed"), 0;
    }
    out->size = size;
    if (properties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) {
        if (vkMapMemory(vk.device, out->memory, 0, size, 0, &out->mapped) != VK_SUCCESS) out->mapped = NULL;
    }
    return 1;
}

void vk_free_buffer(VkGpuBuffer *buffer) {
    if (!buffer) return;
    if (buffer->mapped) { vkUnmapMemory(vk.device, buffer->memory); buffer->mapped = NULL; }
    if (buffer->buffer) vkDestroyBuffer(vk.device, buffer->buffer, NULL);
    if (buffer->memory) vkFreeMemory(vk.device, buffer->memory, NULL);
    memset(buffer, 0, sizeof(*buffer));
}

int vk_create_color_image(int width, int height, VkFormat format, VkImageUsageFlags usage,
                          VkImage *image, VkImageView *view, VkDeviceMemory *memory) {
    *image = VK_NULL_HANDLE; *view = VK_NULL_HANDLE; *memory = VK_NULL_HANDLE;
    if (width <= 0 || height <= 0) return 0;
    VkImageCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent.width = (uint32_t)width;
    info.extent.height = (uint32_t)height;
    info.extent.depth = 1;
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(vk.device, &info, NULL, image) != VK_SUCCESS) return vk_set_error("image creation failed"), 0;
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(vk.device, *image, &requirements);
    uint32_t type = find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) { vkDestroyImage(vk.device, *image, NULL); *image = VK_NULL_HANDLE; return vk_set_error("no device-local memory"), 0; }
    VkMemoryAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vkAllocateMemory(vk.device, &allocate, NULL, memory) != VK_SUCCESS) {
        vkDestroyImage(vk.device, *image, NULL); *image = VK_NULL_HANDLE;
        return vk_set_error("image memory allocation failed"), 0;
    }
    if (vkBindImageMemory(vk.device, *image, *memory, 0) != VK_SUCCESS) {
        vkFreeMemory(vk.device, *memory, NULL); vkDestroyImage(vk.device, *image, NULL);
        *memory = VK_NULL_HANDLE; *image = VK_NULL_HANDLE;
        return vk_set_error("image binding failed"), 0;
    }
    VkImageViewCreateInfo view_info = {0};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = *image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vkCreateImageView(vk.device, &view_info, NULL, view) != VK_SUCCESS) {
        vkFreeMemory(vk.device, *memory, NULL); vkDestroyImage(vk.device, *image, NULL);
        *memory = VK_NULL_HANDLE; *image = VK_NULL_HANDLE;
        return vk_set_error("image view creation failed"), 0;
    }
    return 1;
}

void vk_destroy_image(VkImage *image, VkImageView *view, VkDeviceMemory *memory) {
    if (view && *view) { vkDestroyImageView(vk.device, *view, NULL); *view = VK_NULL_HANDLE; }
    if (image && *image) { vkDestroyImage(vk.device, *image, NULL); *image = VK_NULL_HANDLE; }
    if (memory && *memory) { vkFreeMemory(vk.device, *memory, NULL); *memory = VK_NULL_HANDLE; }
}

/* ── one-shot command buffers for uploads ────────────────────────────── */

VkCommandBuffer vk_begin_once(void) {
    VkCommandBufferAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = vk.command_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(vk.device, &allocate, &cmd) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);
    return cmd;
}

void vk_end_once(VkCommandBuffer cmd) {
    if (cmd == VK_NULL_HANDLE) return;
    vkEndCommandBuffer(cmd);
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info = {0};
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    vkCreateFence(vk.device, &fence_info, NULL, &fence);
    if (vkQueueSubmit(vk.queue, 1, &submit, fence) == VK_SUCCESS) {
        vkWaitForFences(vk.device, 1, &fence, VK_TRUE, 5000000000ull);
    }
    if (fence) vkDestroyFence(vk.device, fence, NULL);
    vkFreeCommandBuffers(vk.device, vk.command_pool, 1, &cmd);
}

int vk_upload(VkBuffer destination, VkDeviceSize offset, const void *data, VkDeviceSize size) {
    if (!data || size == 0) return 1;
    VkGpuBuffer staging;
    if (!vk_create_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &staging)) return 0;
    if (!staging.mapped) { vk_free_buffer(&staging); return vk_set_error("staging buffer is not mappable"), 0; }
    memcpy(staging.mapped, data, (size_t)size);
    VkCommandBuffer cmd = vk_begin_once();
    if (cmd == VK_NULL_HANDLE) { vk_free_buffer(&staging); return vk_set_error("no upload command buffer"), 0; }
    VkBufferCopy region = {0};
    region.srcOffset = 0;
    region.dstOffset = offset;
    region.size = size;
    vkCmdCopyBuffer(cmd, staging.buffer, destination, 1, &region);
    vk_end_once(cmd);
    vk_free_buffer(&staging);
    return 1;
}

/* ── instance and device ─────────────────────────────────────────────── */

/* The loader is optional: vk_entry.c dlopens it and wires the entry points. */
static int load_instance_proc(void) { return vk_entry_load(); }

static int queue_family_ok(VkPhysicalDevice device, uint32_t family) {
    if (vk.native_window == NULL) return 1;
#if defined(ENJOER_VK_ANDROID)
    VkBool32 present = VK_FALSE;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR support =
        (PFN_vkGetPhysicalDeviceSurfaceSupportKHR)vkGetInstanceProcAddr(vk.instance, "vkGetPhysicalDeviceSurfaceSupportKHR");
    if (!support || family >= 64) return 0;
    return support(device, family, (VkSurfaceKHR)vk.native_window, &present) == VK_SUCCESS && present;
#else
    (void)family;
    return 0;
#endif
}

static int pick_physical_device(void) {
    uint32_t count = 0;
    if (vkEnumeratePhysicalDevices(vk.instance, &count, NULL) != VK_SUCCESS || count == 0)
        return vk_set_error("no Vulkan device found"), 0;
    VkPhysicalDevice devices[16];
    if (count > 16) count = 16;
    if (vkEnumeratePhysicalDevices(vk.instance, &count, devices) != VK_SUCCESS)
        return vk_set_error("device enumeration failed"), 0;
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[i], &properties);
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, NULL);
        if (families == 0) continue;
        VkQueueFamilyProperties *list = malloc((size_t)families * sizeof(*list));
        if (!list) continue;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[i], &families, list);
        int chosen = -1;
        for (uint32_t family = 0; family < families; family++) {
            if (!(list[family].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            if (!queue_family_ok(devices[i], family)) continue;
            chosen = (int)family;
            break;
        }
        free(list);
        if (chosen < 0) continue;
        int score = properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 3 :
                    properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 2 : 1;
        if (score > best_score) { best_score = score; best = (int)i; vk.queue_family = (uint32_t)chosen; }
    }
    if (best < 0) {
        if (vk.native_window) return 0;         /* no device can present: fall back */
        return vk_set_error("no usable graphics queue"), 0;
    }
    vk.physical = devices[best];
    vkGetPhysicalDeviceProperties(vk.physical, &vk.properties);
    vkGetPhysicalDeviceMemoryProperties(vk.physical, &vk.memory);
    return 1;
}

static int create_instance(void) {
    VkApplicationInfo application = {0};
    application.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    application.pApplicationName = "Enjoer Blocks";
    application.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    application.pEngineName = "Enjoer";
    application.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    application.apiVersion = VK_API_VERSION_1_0;

    const char *extensions[4];
    uint32_t extension_count = 0;
#if defined(ENJOER_VK_ANDROID)
    if (vk.native_window) {
        extensions[extension_count++] = VK_KHR_SURFACE_EXTENSION_NAME;
        extensions[extension_count++] = VK_KHR_ANDROID_SURFACE_EXTENSION_NAME;
    }
#endif

    const char *layers[2];
    uint32_t layer_count = 0;
    const char *wanted_layers[1] = { "VK_LAYER_KHRONOS_validation" };
    if (getenv("ENJOER_VK_VALIDATION")) {
        uint32_t available = 0;
        vkEnumerateInstanceLayerProperties(&available, NULL);
        VkLayerProperties *list = available ? malloc((size_t)available * sizeof(*list)) : NULL;
        if (list) {
            vkEnumerateInstanceLayerProperties(&available, list);
            for (uint32_t i = 0; i < available; i++) {
                if (strcmp(list[i].layerName, wanted_layers[0]) == 0) { layers[layer_count++] = wanted_layers[0]; break; }
            }
            free(list);
        }
        if (!layer_count) app_log("vulkan: validation layer not available");
    }

    VkInstanceCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &application;
    info.enabledExtensionCount = extension_count;
    info.ppEnabledExtensionNames = extensions;
    info.enabledLayerCount = layer_count;
    info.ppEnabledLayerNames = layers;
    if (vkCreateInstance(&info, NULL, &vk.instance) != VK_SUCCESS)
        return vk_set_error("vkCreateInstance failed (no driver or loader)"), 0;
    vk_entry_load_instance(vk.instance);
    return 1;
}

static int create_device(void) {
    const char *extensions[1];
    uint32_t extension_count = 0;
#if defined(ENJOER_VK_ANDROID)
    if (vk.native_window) extensions[extension_count++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
#endif
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {0};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = vk.queue_family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info = {0};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = extension_count;
    info.ppEnabledExtensionNames = extensions;
    if (vkCreateDevice(vk.physical, &info, NULL, &vk.device) != VK_SUCCESS)
        return vk_set_error("vkCreateDevice failed"), 0;
    vk_entry_load_device(vk.device);
    vkGetDeviceQueue(vk.device, vk.queue_family, 0, &vk.queue);

    VkCommandPoolCreateInfo pool = {0};
    pool.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool.queueFamilyIndex = vk.queue_family;
    if (vkCreateCommandPool(vk.device, &pool, NULL, &vk.command_pool) != VK_SUCCESS)
        return vk_set_error("command pool creation failed"), 0;

    VkCommandBufferAllocateInfo allocate = {0};
    allocate.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocate.commandPool = vk.command_pool;
    allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate.commandBufferCount = VK_FRAMES_IN_FLIGHT;
    if (vkAllocateCommandBuffers(vk.device, &allocate, vk.cmd) != VK_SUCCESS)
        return vk_set_error("command buffer allocation failed"), 0;
    VkFenceCreateInfo fence = {0};
    fence.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int i = 0; i < VK_FRAMES_IN_FLIGHT; i++)
        if (vkCreateFence(vk.device, &fence, NULL, &vk.fence[i]) != VK_SUCCESS)
            return vk_set_error("fence creation failed"), 0;
    VkSemaphoreCreateInfo semaphore = {0};
    semaphore.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    if (vkCreateSemaphore(vk.device, &semaphore, NULL, &vk.image_ready) != VK_SUCCESS ||
        vkCreateSemaphore(vk.device, &semaphore, NULL, &vk.render_done) != VK_SUCCESS)
        return vk_set_error("semaphore creation failed"), 0;

    app_log("vulkan: %s (api %u.%u.%u, queue family %u)", vk.properties.deviceName,
            VK_VERSION_MAJOR(vk.properties.apiVersion), VK_VERSION_MINOR(vk.properties.apiVersion),
            VK_VERSION_PATCH(vk.properties.apiVersion), vk.queue_family);
    return 1;
}

/* ── targets ─────────────────────────────────────────────────────────── */

static void destroy_swapchain(void) {
    for (uint32_t i = 0; i < vk.swapchain_count; i++) {
        if (vk.swapchain_views[i]) vkDestroyImageView(vk.device, vk.swapchain_views[i], NULL);
        vk.swapchain_views[i] = VK_NULL_HANDLE;
        vk.swapchain_images[i] = VK_NULL_HANDLE;
    }
    vk.swapchain_count = 0;
    if (vk.swapchain && vkDestroySwapchainKHR) {
        vkDestroySwapchainKHR(vk.device, vk.swapchain, NULL);
        vk.swapchain = VK_NULL_HANDLE;
    }
}

#if defined(ENJOER_VK_ANDROID)
static int create_swapchain(void) {
    destroy_swapchain();
    if (!vk.native_window) return 0;
    PFN_vkCreateAndroidSurfaceKHR create_surface =
        (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(vk.instance, "vkCreateAndroidSurfaceKHR");
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities =
        (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)vkGetInstanceProcAddr(vk.instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats =
        (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)vkGetInstanceProcAddr(vk.instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
    PFN_vkGetPhysicalDeviceSurfacePresentModesKHR modes =
        (PFN_vkGetPhysicalDeviceSurfacePresentModesKHR)vkGetInstanceProcAddr(vk.instance, "vkGetPhysicalDeviceSurfacePresentModesKHR");
    if (!create_surface || !capabilities || !formats || !modes) return vk_set_error("surface entry points missing"), 0;

    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vk.swapchain_dirty = 0;
    VkAndroidSurfaceCreateInfoKHR surface_info = {0};
    surface_info.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surface_info.window = (ANativeWindow *)vk.native_window;
    if (create_surface(vk.instance, &surface_info, NULL, &surface) != VK_SUCCESS)
        return vk_set_error("android surface creation failed"), 0;

    VkSurfaceCapabilitiesKHR caps;
    if (capabilities(vk.physical, surface, &caps) != VK_SUCCESS) { if (vkDestroySurfaceKHR) vkDestroySurfaceKHR(vk.instance, surface, NULL); return 0; }
    uint32_t format_count = 0;
    formats(vk.physical, surface, &format_count, NULL);
    VkSurfaceFormatKHR *surface_formats = format_count ? malloc((size_t)format_count * sizeof(*surface_formats)) : NULL;
    if (!surface_formats) { if (vkDestroySurfaceKHR) vkDestroySurfaceKHR(vk.instance, surface, NULL); return 0; }
    formats(vk.physical, surface, &format_count, surface_formats);
    VkSurfaceFormatKHR chosen = surface_formats[0];
    for (uint32_t i = 0; i < format_count; i++) {
        VkSurfaceFormatKHR candidate = surface_formats[i];
        if (candidate.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) continue;
        if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM || candidate.format == VK_FORMAT_B8G8R8A8_UNORM ||
            candidate.format == VK_FORMAT_R8G8B8A8_SRGB || candidate.format == VK_FORMAT_B8G8R8A8_SRGB) {
            chosen = candidate;
            if (candidate.format == VK_FORMAT_R8G8B8A8_UNORM || candidate.format == VK_FORMAT_B8G8R8A8_UNORM) break;
        }
    }
    free(surface_formats);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xffffffffu || extent.height == 0xffffffffu) {
        extent.width = (uint32_t)vk.width;
        extent.height = (uint32_t)vk.height;
        if (extent.width < caps.minImageExtent.width) extent.width = caps.minImageExtent.width;
        if (extent.height < caps.minImageExtent.height) extent.height = caps.minImageExtent.height;
        if (extent.width > caps.maxImageExtent.width) extent.width = caps.maxImageExtent.width;
        if (extent.height > caps.maxImageExtent.height) extent.height = caps.maxImageExtent.height;
    }
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount && image_count > caps.maxImageCount) image_count = caps.maxImageCount;
    VkSwapchainCreateInfoKHR chain = {0};
    chain.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    chain.surface = surface;
    chain.minImageCount = image_count;
    chain.imageFormat = chosen.format;
    chain.imageColorSpace = chosen.colorSpace;
    chain.imageExtent = extent;
    chain.imageArrayLayers = 1;
    chain.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    chain.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    chain.preTransform = caps.currentTransform;
    chain.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    chain.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    chain.clipped = VK_TRUE;
    PFN_vkCreateSwapchainKHR create_chain =
        (PFN_vkCreateSwapchainKHR)vkGetInstanceProcAddr(vk.instance, "vkCreateSwapchainKHR");
    if (!create_chain || create_chain(vk.device, &chain, NULL, &vk.swapchain) != VK_SUCCESS) {
        if (vkDestroySurfaceKHR) vkDestroySurfaceKHR(vk.instance, surface, NULL);
        return vk_set_error("swapchain creation failed"), 0;
    }
    vk.swapchain_format = chosen.format;
    vk.swapchain_extent = extent;
    PFN_vkGetSwapchainImagesKHR get_images =
        (PFN_vkGetSwapchainImagesKHR)vkGetInstanceProcAddr(vk.instance, "vkGetSwapchainImagesKHR");
    uint32_t count = 0;
    get_images(vk.device, vk.swapchain, &count, NULL);
    if (count > VK_SWAPCHAIN_IMAGES) count = VK_SWAPCHAIN_IMAGES;
    get_images(vk.device, vk.swapchain, &count, vk.swapchain_images);
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo view = {0};
        view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view.image = vk.swapchain_images[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = vk.swapchain_format;
        view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view.subresourceRange.levelCount = 1;
        view.subresourceRange.layerCount = 1;
        if (vkCreateImageView(vk.device, &view, NULL, &vk.swapchain_views[i]) != VK_SUCCESS) break;
        vk.swapchain_count = i + 1;
    }
    if (vkDestroySurfaceKHR) vkDestroySurfaceKHR(vk.instance, surface, NULL);
    vk.width = (int)extent.width;
    vk.height = (int)extent.height;
    return vk.swapchain_count > 0;
}
#else
static int create_swapchain(void) { return 0; }
#endif

static void destroy_target(void) {
    if (vk.target_view) { vkDestroyImageView(vk.device, vk.target_view, NULL); vk.target_view = VK_NULL_HANDLE; }
    if (vk.target_image) { vkDestroyImage(vk.device, vk.target_image, NULL); vk.target_image = VK_NULL_HANDLE; }
    if (vk.target_memory) { vkFreeMemory(vk.device, vk.target_memory, NULL); vk.target_memory = VK_NULL_HANDLE; }
    if (vk.readback) {
        VkGpuBuffer readback = { vk.readback, vk.readback_memory, vk.readback_size, vk.readback_mapped };
        vk_free_buffer(&readback);
        vk.readback = VK_NULL_HANDLE;
        vk.readback_memory = VK_NULL_HANDLE;
        vk.readback_mapped = NULL;
        vk.readback_size = 0;
    }
    vk.target_ready = 0;
}

static int ensure_offscreen_target(int width, int height) {
    if (vk.target_ready && vk.target_format == VK_FORMAT_R8G8B8A8_UNORM) return 1;
    destroy_target();
    vk.target_format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!vk_create_color_image(width, height, vk.target_format, usage, &vk.target_image, &vk.target_view, &vk.target_memory)) return 0;
    vk.target_ready = 1;
    return 1;
}

void vk_require_targets(int width, int height, int scale) {
    if (width <= 0 || height <= 0) return;
    vk.width = width;
    vk.height = height;
    vk.scale = scale < 1 ? 1 : scale;
    if (vk.windowed) return;
    ensure_offscreen_target(vk.width, vk.height);
    VkDeviceSize needed = (VkDeviceSize)vk.width * (VkDeviceSize)vk.height * 4;
    if (vk.readback_size < needed) {
        if (vk.readback) {
            VkGpuBuffer old = { vk.readback, vk.readback_memory, vk.readback_size, vk.readback_mapped };
            vk_free_buffer(&old);
            vk.readback = VK_NULL_HANDLE; vk.readback_memory = VK_NULL_HANDLE; vk.readback_mapped = NULL;
        }
        VkGpuBuffer buffer;
        if (vk_create_buffer(needed, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buffer)) {
            vk.readback = buffer.buffer; vk.readback_memory = buffer.memory;
            vk.readback_mapped = buffer.mapped; vk.readback_size = buffer.size;
        } else {
            vk.readback_size = 0;
        }
    }
}

int vk_attach_window(void *native_window) {
    vk.native_window = native_window;
    vk.windowed = native_window != NULL;
    vk.swapchain_dirty = 1;
    destroy_swapchain();
    return 1;
}

/* ── frame lifecycle ─────────────────────────────────────────────────── */

static void sync_immediate(VkImmediate *batch, size_t stride, size_t capacity, VkBufferUsageFlags usage) {
    batch->count = 0;
    batch->recorded = 0;
    if (!batch->gpu.buffer) {
        VkGpuBuffer buffer;
        if (!vk_create_buffer((VkDeviceSize)capacity * stride, usage,
                              VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, &buffer)) {
            batch->capacity = 0;
            return;
        }
        batch->gpu = buffer;
        batch->capacity = buffer.size / stride;
    }
}

int vk_frame_begin(Buffer *buffer) {
    if (!vk.ready) return 0;
    if (buffer) {
        if (!buffer->pixels || buffer->width <= 0 || buffer->height <= 0 || buffer->stride < buffer->width) return 0;
        vk.frame = buffer;
        vk.frame_has_pixels = 1;
        vk_require_targets(buffer->width, buffer->height, vk.scale < 1 ? 1 : vk.scale);
    } else {
        vk.frame = NULL;
        vk.frame_has_pixels = 0;
    }
    if (vk.windowed && (vk.swapchain == VK_NULL_HANDLE || vk.swapchain_dirty)) {
        if (!create_swapchain()) return 0;
    }
    if (vk.windowed) {
        PFN_vkAcquireNextImageKHR acquire =
            (PFN_vkAcquireNextImageKHR)vkGetInstanceProcAddr(vk.instance, "vkAcquireNextImageKHR");
        if (!acquire) return vk_set_error("vkAcquireNextImageKHR missing"), 0;
        VkResult result = acquire(vk.device, vk.swapchain, 5000000000ull, vk.image_ready, VK_NULL_HANDLE, &vk.swapchain_index);
        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            if (!create_swapchain()) return 0;
            result = acquire(vk.device, vk.swapchain, 5000000000ull, vk.image_ready, VK_NULL_HANDLE, &vk.swapchain_index);
        }
        if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) return vk_set_error("swapchain acquire failed"), 0;
    } else if (!vk.target_ready) {
        return 0;
    }

    vkWaitForFences(vk.device, 1, &vk.fence[vk.frame_index], VK_TRUE, 1000000000ull);
    vkResetFences(vk.device, 1, &vk.fence[vk.frame_index]);
    vkResetCommandBuffer(vk.cmd[vk.frame_index], 0);
    VkCommandBufferBeginInfo begin = {0};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(vk.cmd[vk.frame_index], &begin) != VK_SUCCESS)
        return vk_set_error("command buffer begin failed"), 0;

    size_t index = (size_t)vk.frame_index;
    sync_immediate(&vk.lines[index], sizeof(VkLineVertex), VK_LINE_CAPACITY, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    sync_immediate(&vk.triangles[index], sizeof(VkWorldVertex), VK_TRIANGLE_CAPACITY, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    sync_immediate(&vk.ui[index], sizeof(VkUiVertex), VK_UI_CAPACITY, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    vk.frame_active = 1;
    vk.pass3d_active = 0;
    vk.scene_started = 0;
    vk.viewmodel = 0;
    vk.ui_dirty = 0;
    vk_ui_reset();
    return 1;
}

static void record_readback(void) {
    VkCommandBuffer cmd = vk.cmd[vk.frame_index];
    if (!vk.readback || !vk.readback_mapped || !vk.frame_has_pixels) return;
    VkBufferImageCopy region = {0};
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.layerCount = 1;
    region.imageExtent.width = (uint32_t)vk.width;
    region.imageExtent.height = (uint32_t)vk.height;
    region.imageExtent.depth = 1;
    region.bufferRowLength = (uint32_t)vk.width;
    vkCmdCopyImageToBuffer(cmd, vk.target_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vk.readback, 1, &region);
    VkBufferMemoryBarrier barrier = {0};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = vk.readback;
    barrier.size = vk.readback_size;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &barrier, 0, NULL);
}

void vk_frame_end(void) {
    if (!vk.ready || !vk.frame_active) return;
    vk_flush_lines();
    vk_flush_triangles();
    if (vk.pass3d_active) vk_end_scene_pass();

    VkImage target = vk.windowed ? vk.swapchain_images[vk.swapchain_index] : vk.target_image;
    VkImageView target_view = vk.windowed ? vk.swapchain_views[vk.swapchain_index] : vk.target_view;
    if (target == VK_NULL_HANDLE) { vk_frame_cancel(); return; }

    VkRenderPassBeginInfo pass = {0};
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = vk.pass2d;
    pass.framebuffer = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkImageView attachment = target_view;
    VkFramebufferCreateInfo framebuffer_info = {0};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = vk.pass2d;
    framebuffer_info.attachmentCount = 1;
    framebuffer_info.pAttachments = &attachment;
    framebuffer_info.width = (uint32_t)vk.width;
    framebuffer_info.height = (uint32_t)vk.height;
    framebuffer_info.layers = 1;
    if (vkCreateFramebuffer(vk.device, &framebuffer_info, NULL, &framebuffer) != VK_SUCCESS) { vk_frame_cancel(); return; }

    if (vk.scene_started) {
        /* The 3D pass wrote the scene image; make the writes visible to the
         * upscale draw (the layout already is SHADER_READ_ONLY). */
        VkImageMemoryBarrier barrier = {0};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vk.scene.color;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(vk.cmd[vk.frame_index], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    }

    VkClearValue clear = {0};
    clear.color.float32[0] = 0.102f; clear.color.float32[1] = 0.102f; clear.color.float32[2] = 0.180f;
    clear.color.float32[3] = 1.0f;
    pass.framebuffer = framebuffer;
    pass.renderArea.extent.width = (uint32_t)vk.width;
    pass.renderArea.extent.height = (uint32_t)vk.height;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    vkCmdBeginRenderPass(vk.cmd[vk.frame_index], &pass, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport = {0, 0, (float)vk.width, (float)vk.height, 0.0f, 1.0f};
    vkCmdSetViewport(vk.cmd[vk.frame_index], 0, 1, &viewport);
    VkRect2D scissor = {{0, 0}, {(uint32_t)vk.width, (uint32_t)vk.height}};
    vkCmdSetScissor(vk.cmd[vk.frame_index], 0, 1, &scissor);
    if (vk.scene_started) vk_ui_draw_present(vk.scene.color_view, vk.scene_width, vk.scene_height);
    vk_ui_flush();
    vkCmdEndRenderPass(vk.cmd[vk.frame_index]);
    vkDestroyFramebuffer(vk.device, framebuffer, NULL);

    if (!vk.windowed) {
        VkImageMemoryBarrier barrier = {0};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = vk.target_image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.layerCount = 1;
        barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(vk.cmd[vk.frame_index], VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
        record_readback();
    }

    if (vkEndCommandBuffer(vk.cmd[vk.frame_index]) != VK_SUCCESS) { vk_frame_cancel(); return; }

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo submit = {0};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &vk.cmd[vk.frame_index];
    if (vk.windowed) {
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &vk.image_ready;
        submit.pWaitDstStageMask = &wait_stage;
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &vk.render_done;
    }
    if (vkQueueSubmit(vk.queue, 1, &submit, vk.fence[vk.frame_index]) != VK_SUCCESS) { vk_frame_cancel(); return; }

    if (vk.windowed) {
        PFN_vkQueuePresentKHR present = (PFN_vkQueuePresentKHR)vkGetInstanceProcAddr(vk.instance, "vkQueuePresentKHR");
        VkPresentInfoKHR info = {0};
        info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &vk.render_done;
        info.swapchainCount = 1;
        info.pSwapchains = &vk.swapchain;
        info.pImageIndices = &vk.swapchain_index;
        VkResult result = present ? present(vk.queue, &info) : VK_ERROR_OUT_OF_DATE_KHR;
        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) vk.swapchain_dirty = 1;
    } else {
        vkWaitForFences(vk.device, 1, &vk.fence[vk.frame_index], VK_TRUE, 1000000000ull);
        if (vk.frame_has_pixels && vk.readback_mapped && vk.frame) {
            const uint8_t *source = (const uint8_t *)vk.readback_mapped;
            size_t row = (size_t)vk.width * 4;
            for (int y = 0; y < vk.height; y++) {
                memcpy((uint8_t *)vk.frame->pixels + (size_t)y * vk.frame->stride * 4,
                       source + (size_t)y * row, row);
            }
        }
    }
    vk.frame_active = 0;
    vk.frame_index = (vk.frame_index + 1) % VK_FRAMES_IN_FLIGHT;
}

void vk_frame_cancel(void) {
    if (vk.frame_active) {
        vkEndCommandBuffer(vk.cmd[vk.frame_index]);
        if (vk.windowed) {
            vkQueueWaitIdle(vk.queue);
        } else {
            vkResetFences(vk.device, 1, &vk.fence[vk.frame_index]);
        }
    }
    vk.frame_active = 0;
    vk.pass3d_active = 0;
    vk.frame_index = (vk.frame_index + 1) % VK_FRAMES_IN_FLIGHT;
}

/* ── init / shutdown ─────────────────────────────────────────────────── */

int vk_device_init(AAssetManager *assets) {
    memset(&vk, 0, sizeof(vk));
    vk.error[0] = '\0';
    vk.target_format = VK_FORMAT_UNDEFINED;
    vk.scale = 1;
    vk.width = screen_w > 0 ? screen_w : 960;
    vk.height = screen_h > 0 ? screen_h : 540;

    if (!load_instance_proc()) return vk_set_error("the Vulkan loader (libvulkan) is not available"), 0;
    if (!create_instance()) return 0;
    if (!pick_physical_device()) return 0;
    if (!create_device()) return 0;
    if (!vk_textures_init(assets)) return 0;
    if (!vk_pipelines_init(assets)) return 0;
    vk.ready = 1;
    return 1;
}

void vk_device_shutdown(void) {
    if (vk.device == VK_NULL_HANDLE) { memset(&vk, 0, sizeof(vk)); return; }
    vkDeviceWaitIdle(vk.device);
    vk_world_shutdown();
    vk_textures_shutdown();
    vk_pipelines_shutdown();
    for (int i = 0; i < VK_FRAMES_IN_FLIGHT; i++) {
        vk_free_buffer(&vk.lines[i].gpu);
        vk_free_buffer(&vk.triangles[i].gpu);
        vk_free_buffer(&vk.ui[i].gpu);
    }
    destroy_target();   /* also releases the readback buffer */
    destroy_swapchain();   /* the scene target belongs to vk_pipelines_shutdown() */
    if (vk.image_ready) vkDestroySemaphore(vk.device, vk.image_ready, NULL);
    if (vk.render_done) vkDestroySemaphore(vk.device, vk.render_done, NULL);
    for (int i = 0; i < VK_FRAMES_IN_FLIGHT; i++) if (vk.fence[i]) vkDestroyFence(vk.device, vk.fence[i], NULL);
    if (vk.command_pool) vkDestroyCommandPool(vk.device, vk.command_pool, NULL);
    vkDestroyDevice(vk.device, NULL);
    vkDestroyInstance(vk.instance, NULL);
    memset(&vk, 0, sizeof(vk));
}
