/*
 * The C++ half of Enjoer: a deliberately tiny Vulkan renderer.
 *
 * The Vulkan path is compiled when ENJOER_USE_VULKAN is enabled.  Keeping
 * the preview fallback in this translation unit is useful on a fresh
 * checkout: the C game and its controls can be tested on a host without a
 * GPU, while Android/device builds use exactly the same cube data and the
 * SPIR-V shaders from src/shaders/.
 *
 * The same CPU rasterizer is the runtime safety net: if a device has no
 * usable Vulkan driver, or loses it mid-session, the cube keeps spinning on
 * the CPU instead of showing a black screen.
 */
#include "vulkan_cube.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#include <android/native_window.h>
#endif
#include <time.h>

#ifndef ENJOER_USE_VULKAN
#define ENJOER_USE_VULKAN 0
#endif

#if ENJOER_USE_VULKAN
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR 1
#endif
#include <vulkan/vulkan.h>
#include "shaders/cube_spv.h"
#endif

namespace {

struct Vertex {
    float x, y, z;
    float r, g, b;
};

struct Projected {
    float x, y, z;
};

static uint32_t rgba(int r, int g, int b) {
    auto byte = [](int value) -> uint32_t {
        return static_cast<uint32_t>(std::max(0, std::min(255, value)));
    };
    return byte(r) | (byte(g) << 8) | (byte(b) << 16) | 0xff000000u;
}

static void put_pixel(Buffer *buffer, int x, int y, uint32_t color) {
    if (!buffer || !buffer->pixels || x < 0 || y < 0 ||
        x >= buffer->width || y >= buffer->height) return;
    buffer->pixels[y * buffer->stride + x] = color;
}

static Projected transform(float x, float y, float z, float rotation, float pitch,
                           float aspect) {
    const float sy = std::sin(rotation), cy = std::cos(rotation);
    const float sx = std::sin(pitch), cx = std::cos(pitch);

    /* Model rotation: pitch around X, then the continuous turn around Y. */
    float px = cy * x + sy * z;
    float pz = -sy * x + cy * z;
    float py = cx * y - sx * pz;
    pz = sx * y + cx * pz;
    pz += 5.0f;

    const float focal = 1.0f / std::tan(62.0f * 3.1415926535f / 360.0f);
    return {focal * px / (pz * aspect), focal * py / pz, pz};
}

static void fill_triangle(Buffer *buffer, std::vector<float> &depth,
                          Projected a, Projected b, Projected c,
                          uint32_t color) {
    if (!buffer || !buffer->pixels) return;
    const float area = (b.x - a.x) * (c.y - a.y) -
                       (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 0.00001f) return;

    int left = static_cast<int>(std::floor(std::min({a.x, b.x, c.x}) *
                                           buffer->height / 2.0f + buffer->width / 2.0f));
    int right = static_cast<int>(std::ceil(std::max({a.x, b.x, c.x}) *
                                            buffer->height / 2.0f + buffer->width / 2.0f));
    int top = static_cast<int>(std::floor(buffer->height / 2.0f -
                                          std::max({a.y, b.y, c.y}) * buffer->height / 2.0f));
    int bottom = static_cast<int>(std::ceil(buffer->height / 2.0f -
                                             std::min({a.y, b.y, c.y}) * buffer->height / 2.0f));
    left = std::max(0, left); right = std::min(buffer->width, right);
    top = std::max(0, top); bottom = std::min(buffer->height, bottom);

    for (int py = top; py < bottom; ++py) {
        for (int px = left; px < right; ++px) {
            const float x = (static_cast<float>(px) + .5f - buffer->width / 2.0f) *
                            2.0f / buffer->height;
            const float y = (buffer->height / 2.0f - static_cast<float>(py) - .5f) *
                            2.0f / buffer->height;
            const float wa = ((b.x - x) * (c.y - y) - (b.y - y) * (c.x - x)) / area;
            const float wb = ((c.x - x) * (a.y - y) - (c.y - y) * (a.x - x)) / area;
            const float wc = 1.0f - wa - wb;
            if (wa < 0 || wb < 0 || wc < 0) continue;
            const float z = wa * a.z + wb * b.z + wc * c.z;
            const size_t index = static_cast<size_t>(py) * buffer->width + px;
            if (z < depth[index]) {
                depth[index] = z;
                put_pixel(buffer, px, py, color);
            }
        }
    }
}

static const std::array<Vertex, 36> &cube_vertices() {
    /* Two triangles per face. Each face has its own cheerful material color. */
    static const std::array<Vertex, 36> vertices = {{
        {-1,-1, 1, .95f,.28f,.28f}, { 1,-1, 1, .95f,.28f,.28f}, { 1, 1, 1, .95f,.28f,.28f},
        {-1,-1, 1, .95f,.28f,.28f}, { 1, 1, 1, .95f,.28f,.28f}, {-1, 1, 1, .95f,.28f,.28f},
        { 1,-1,-1, .25f,.72f,1.0f}, {-1,-1,-1, .25f,.72f,1.0f}, {-1, 1,-1, .25f,.72f,1.0f},
        { 1,-1,-1, .25f,.72f,1.0f}, {-1, 1,-1, .25f,.72f,1.0f}, { 1, 1,-1, .25f,.72f,1.0f},
        {-1, 1, 1, .35f,1.0f,.55f}, { 1, 1, 1, .35f,1.0f,.55f}, { 1, 1,-1, .35f,1.0f,.55f},
        {-1, 1, 1, .35f,1.0f,.55f}, { 1, 1,-1, .35f,1.0f,.55f}, {-1, 1,-1, .35f,1.0f,.55f},
        {-1,-1,-1, 1.0f,.78f,.22f}, { 1,-1,-1, 1.0f,.78f,.22f}, { 1,-1, 1, 1.0f,.78f,.22f},
        {-1,-1,-1, 1.0f,.78f,.22f}, { 1,-1, 1, 1.0f,.78f,.22f}, {-1,-1, 1, 1.0f,.78f,.22f},
        { 1,-1, 1, 1.0f,.42f,.92f}, { 1,-1,-1, 1.0f,.42f,.92f}, { 1, 1,-1, 1.0f,.42f,.92f},
        { 1,-1, 1, 1.0f,.42f,.92f}, { 1, 1,-1, 1.0f,.42f,.92f}, { 1, 1, 1, 1.0f,.42f,.92f},
        {-1,-1,-1, .38f,.48f,1.0f}, {-1,-1, 1, .38f,.48f,1.0f}, {-1, 1, 1, .38f,.48f,1.0f},
        {-1,-1,-1, .38f,.48f,1.0f}, {-1, 1, 1, .38f,.48f,1.0f}, {-1, 1,-1, .38f,.48f,1.0f},
    }};
    return vertices;
}

/* ------------------------------------------------------------------ *
 * CPU rasterizer.                                                     *
 *                                                                     *
 * Safety net for a device without a working Vulkan driver: the same    *
 * cube data is rasterized by the CPU into a small offscreen buffer     *
 * which is then stretched over the native window.                      *
 * ------------------------------------------------------------------- */

/* Longest edge and pixel budget of the software framebuffer. Rasterizing a
 * phone screen 1:1 would cost tens of milliseconds per frame, so the CPU
 * always draws a downscaled image and the blit scales it back up. */
constexpr int kSoftwareMaxEdge = 400;
constexpr int kSoftwareMaxPixels = 160000;

/* Frames Vulkan may drop in a row before the renderer switches to the CPU. */
constexpr int kMaxVulkanFailures = 10;

struct SoftwareSurface {
    std::vector<uint32_t> pixels;
    std::vector<float> depth;
    int width = 0;
    int height = 0;

    /* Pick a resolution that keeps the CPU frame time sane. */
    void resize(int target_width, int target_height) {
        const int w = std::max(1, target_width);
        const int h = std::max(1, target_height);
        double scale = 1.0;
        const int longest = std::max(w, h);
        if (longest > kSoftwareMaxEdge)
            scale = std::min(scale, static_cast<double>(kSoftwareMaxEdge) / longest);
        const double total = static_cast<double>(w) * static_cast<double>(h);
        if (total * scale * scale > static_cast<double>(kSoftwareMaxPixels))
            scale = std::min(scale, std::sqrt(static_cast<double>(kSoftwareMaxPixels) / total));
        const int sw = std::max(1, static_cast<int>(std::lround(w * scale)));
        const int sh = std::max(1, static_cast<int>(std::lround(h * scale)));
        if (sw == width && sh == height) return;
        width = sw;
        height = sh;
        pixels.assign(static_cast<size_t>(sw) * sh, 0u);
        depth.assign(static_cast<size_t>(sw) * sh, std::numeric_limits<float>::infinity());
    }

    void release() {
        pixels.clear();
        pixels.shrink_to_fit();
        depth.clear();
        depth.shrink_to_fit();
        width = height = 0;
    }
};

/* Nearest-neighbour mapping from destination pixels to source pixels. The
 * tables are rebuilt only when either side changes size. */
struct BlitMap {
    std::vector<int> columns;
    std::vector<int> rows;
    int src_w = -1, src_h = -1, dst_w = -1, dst_h = -1;

    void build(int sw, int sh, int dw, int dh) {
        if (sw == src_w && sh == src_h && dw == dst_w && dh == dst_h) return;
        src_w = sw; src_h = sh; dst_w = dw; dst_h = dh;
        columns.resize(static_cast<size_t>(std::max(0, dw)));
        rows.resize(static_cast<size_t>(std::max(0, dh)));
        for (int x = 0; x < dw; ++x)
            columns[static_cast<size_t>(x)] =
                std::min(sw - 1, static_cast<int>((static_cast<int64_t>(x) * sw) / std::max(1, dw)));
        for (int y = 0; y < dh; ++y)
            rows[static_cast<size_t>(y)] =
                std::min(sh - 1, static_cast<int>((static_cast<int64_t>(y) * sh) / std::max(1, dh)));
    }
};

/* Our 32-bit colors are 0xAABBGGRR, i.e. little-endian R,G,B,A in memory. */
static uint16_t pack_rgb565(uint32_t color) {
    const uint32_t r = (color >> 0) & 0xffu;
    const uint32_t g = (color >> 8) & 0xffu;
    const uint32_t b = (color >> 16) & 0xffu;
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void blit_scaled(const SoftwareSurface &src, BlitMap &map, Buffer *dst) {
    if (!dst || !dst->pixels || src.pixels.empty()) return;
    const int dw = dst->width;
    const int dh = dst->height;
    if (dw < 1 || dh < 1) return;
    map.build(src.width, src.height, dw, dh);

    const uint16_t *rgb565_target =
        dst->format == ENJOER_BUFFER_FORMAT_RGB565
            ? reinterpret_cast<const uint16_t *>(dst->pixels)
            : nullptr;

    for (int y = 0; y < dh; ++y) {
        const uint32_t *source_row =
            src.pixels.data() + static_cast<size_t>(map.rows[static_cast<size_t>(y)]) * src.width;
        const int row_pixels = std::min(dw, std::max(0, dst->stride));
        if (rgb565_target) {
            uint16_t *target_row = reinterpret_cast<uint16_t *>(dst->pixels) +
                                   static_cast<size_t>(y) * dst->stride;
            if (src.width == dw) {
                for (int x = 0; x < row_pixels; ++x) target_row[x] = pack_rgb565(source_row[x]);
            } else {
                for (int x = 0; x < row_pixels; ++x)
                    target_row[x] = pack_rgb565(source_row[static_cast<size_t>(map.columns[static_cast<size_t>(x)])]);
            }
            continue;
        }
        uint32_t *target_row = dst->pixels + static_cast<size_t>(y) * dst->stride;
        if (src.width == dw) {
            std::memcpy(target_row, source_row, static_cast<size_t>(row_pixels) * sizeof(uint32_t));
        } else {
            for (int x = 0; x < row_pixels; ++x)
                target_row[x] = source_row[static_cast<size_t>(map.columns[static_cast<size_t>(x)])];
        }
    }
}

static void rasterize_cube(Buffer *buffer, std::vector<float> &depth, float rotation,
                           float pitch) {
    if (!buffer || !buffer->pixels || buffer->width < 1 || buffer->height < 1) return;
    const float aspect = static_cast<float>(buffer->width) /
                         static_cast<float>(std::max(1, buffer->height));
    for (int y = 0; y < buffer->height; ++y) {
        const float t = static_cast<float>(y) / std::max(1, buffer->height - 1);
        const uint32_t sky = rgba(static_cast<int>(25 + 35 * t),
                                  static_cast<int>(45 + 70 * t),
                                  static_cast<int>(90 + 95 * t));
        for (int x = 0; x < buffer->width; ++x)
            buffer->pixels[y * buffer->stride + x] = sky;
    }

    depth.assign(static_cast<size_t>(buffer->width) * buffer->height,
                 std::numeric_limits<float>::infinity());
    const auto &vertices = cube_vertices();
    for (size_t i = 0; i < vertices.size(); i += 3) {
        const Vertex &va = vertices[i];
        const Vertex &vb = vertices[i + 1];
        const Vertex &vc = vertices[i + 2];
        const Projected a = transform(va.x, va.y, va.z, rotation, pitch, aspect);
        const Projected b = transform(vb.x, vb.y, vb.z, rotation, pitch, aspect);
        const Projected c = transform(vc.x, vc.y, vc.z, rotation, pitch, aspect);
        const int shade = static_cast<int>(92 + 70 * std::max(0.0f, 1.0f - a.z / 8.0f));
        const int red = static_cast<int>(255 * va.r) * shade / 160;
        const int green = static_cast<int>(255 * va.g) * shade / 160;
        const int blue = static_cast<int>(255 * va.b) * shade / 160;
        fill_triangle(buffer, depth, a, b, c, rgba(red, green, blue));
    }
}

static void *native_window;
static int width = 1;
static int height = 1;
static bool software_active;
static SoftwareSurface software_surface;
static BlitMap software_map;
static std::vector<float> preview_depth;

/* Draw the cube on the CPU at a reduced resolution and stretch it over the
 * whole target. This is what a device without a Vulkan driver gets. */
static void software_render_into(Buffer *target, float rotation, float pitch) {
    if (!target || !target->pixels || target->width < 1 || target->height < 1) return;
    software_surface.resize(target->width, target->height);
    Buffer low{};
    low.pixels = software_surface.pixels.data();
    low.width = software_surface.width;
    low.height = software_surface.height;
    low.stride = software_surface.width;
    low.format = ENJOER_BUFFER_FORMAT_RGBA8888;
    rasterize_cube(&low, software_surface.depth, rotation, pitch);
    blit_scaled(software_surface, software_map, target);
}

#ifdef __ANDROID__
/* The CPU path redraws the whole screen, so pace it at roughly 30 fps instead
 * of burning a core at vsync rate. */
static bool software_frame_due(void) {
    static uint64_t last_ns;
    struct timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return true;
    const uint64_t now_ns = static_cast<uint64_t>(now.tv_sec) * 1000000000ull +
                            static_cast<uint64_t>(now.tv_nsec);
    if (last_ns && now_ns - last_ns < 33000000ull) return false;
    last_ns = now_ns;
    return true;
}

/* Ask for a 32-bit window surface. Devices that answer with RGB565 are
 * handled by the blit, which converts on the fly. */
static void request_window_format(void *window) {
    ANativeWindow *native = static_cast<ANativeWindow *>(window);
    if (!native) return;
    if (ANativeWindow_setBuffersGeometry(native, width, height, WINDOW_FORMAT_RGBX_8888) != 0)
        ANativeWindow_setBuffersGeometry(native, width, height, WINDOW_FORMAT_RGBA_8888);
}

static int buffer_format_from_window(int format) {
    return format == WINDOW_FORMAT_RGB_565 ? ENJOER_BUFFER_FORMAT_RGB565
                                          : ENJOER_BUFFER_FORMAT_RGBA8888;
}

static void software_present_to_window(float rotation, float pitch) {
    ANativeWindow *window = static_cast<ANativeWindow *>(native_window);
    if (!window || !software_frame_due()) return;
    ANativeWindow_Buffer locked{};
    if (ANativeWindow_lock(window, &locked, nullptr) != 0) return;
    Buffer target{};
    target.pixels = static_cast<uint32_t *>(locked.bits);
    target.width = locked.width;
    target.height = locked.height;
    target.stride = locked.stride;
    target.format = buffer_format_from_window(locked.format);
    software_render_into(&target, rotation, pitch);
    ANativeWindow_unlockAndPost(window);
}
#endif

/* Which renderer the user asked for. ENJOER_RENDERER works everywhere; on a
 * device `setprop debug.enjoer.renderer software` does the same thing without
 * rebuilding or rooting. */
enum class Backend { Automatic, Software, Vulkan };

static Backend requested_backend(void) {
    const char *value = std::getenv("ENJOER_RENDERER");
#ifdef __ANDROID__
    char property[PROP_VALUE_MAX] = {0};
    if ((!value || !*value) && __system_property_get("debug.enjoer.renderer", property) > 0)
        value = property;
#endif
    if (!value || !*value) return Backend::Automatic;
    if (!std::strcmp(value, "software") || !std::strcmp(value, "cpu") ||
        !std::strcmp(value, "fallback"))
        return Backend::Software;
    if (!std::strcmp(value, "vulkan") || !std::strcmp(value, "gpu")) return Backend::Vulkan;
    return Backend::Automatic;
}

#if ENJOER_USE_VULKAN

/* A compact column-major 4 by 4 matrix, matching GLSL's mat4 layout. */
struct Mat4 { float value[16]{}; };
static Mat4 multiply(const Mat4 &a, const Mat4 &b) {
    Mat4 result{};
    for (int col = 0; col < 4; ++col)
        for (int row = 0; row < 4; ++row)
            for (int k = 0; k < 4; ++k)
                result.value[col * 4 + row] +=
                    a.value[k * 4 + row] * b.value[col * 4 + k];
    return result;
}

/* Zero-initialise a Vulkan struct and set its sType. */
template <class T> static T vk_struct(VkStructureType type) {
    T value{};
    value.sType = type;
    return value;
}

/* How a render() call went. See VulkanCube::render. */
enum class RenderStatus { Ok, Transient, Fatal };

static bool is_fatal(VkResult result) {
    return result == VK_ERROR_DEVICE_LOST || result == VK_ERROR_SURFACE_LOST_KHR;
}

class VulkanCube {
public:
    bool init(void *native_window, int width, int height) {
        width_ = std::max(1, width);
        height_ = std::max(1, height);
        if (!create_instance()) return fail("vkCreateInstance");
        if (!create_surface(native_window)) return fail("vkCreateAndroidSurfaceKHR");
        if (!pick_device()) return fail("no graphics queue with present support");
        if (!create_device()) return fail("vkCreateDevice");
        if (!create_swapchain()) return fail("vkCreateSwapchainKHR");
        if (!create_render_pass()) return fail("vkCreateRenderPass");
        if (!create_depth()) return fail("depth buffer creation");
        if (!create_framebuffers()) return fail("vkCreateFramebuffer");
        if (!create_pipeline()) return fail("graphics pipeline creation");
        if (!create_vertex_buffer()) return fail("vertex buffer allocation");
        if (!create_commands()) return fail("command buffer allocation");
        if (!create_sync()) return fail("semaphore/fence creation");
        initialized_ = true;
        return true;
    }

    /* Name of the step that failed, for logcat. Valid until shutdown(). */
    const char *stage(void) const { return stage_ ? stage_ : "Vulkan initialisation"; }

    void resize(int width, int height) {
        width_ = std::max(1, width);
        height_ = std::max(1, height);
        if (initialized_) recreate_swapchain();
    }

    /* Ok: a frame was presented or the swapchain was rebuilt.
     * Transient: this frame failed but Vulkan may recover (rotation, resize).
     * Fatal: the device or surface is gone; the caller should fall back. */
    RenderStatus render(float rotation, float pitch) {
        if (!initialized_ || swapchain_ == VK_NULL_HANDLE) return RenderStatus::Transient;

        vkWaitForFences(device_, 1, &in_flight_, VK_TRUE, UINT64_MAX);
        uint32_t image = 0;
        VkResult acquired = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                                  image_available_, VK_NULL_HANDLE, &image);
        if (acquired == VK_ERROR_OUT_OF_DATE_KHR) { recreate_swapchain(); return RenderStatus::Ok; }
        if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR)
            return is_fatal(acquired) ? RenderStatus::Fatal : RenderStatus::Transient;
        vkResetFences(device_, 1, &in_flight_);

        const Mat4 mvp = compute_mvp(rotation, pitch);
        record(image, mvp);

        const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo submit = vk_struct<VkSubmitInfo>(VK_STRUCTURE_TYPE_SUBMIT_INFO);
        submit.waitSemaphoreCount = 1;
        submit.pWaitSemaphores = &image_available_;
        submit.pWaitDstStageMask = &wait_stage;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffers_[image];
        submit.signalSemaphoreCount = 1;
        submit.pSignalSemaphores = &render_finished_;
        if (vkQueueSubmit(queue_, 1, &submit, in_flight_) != VK_SUCCESS) return RenderStatus::Fatal;

        VkPresentInfoKHR present = vk_struct<VkPresentInfoKHR>(VK_STRUCTURE_TYPE_PRESENT_INFO_KHR);
        present.waitSemaphoreCount = 1;
        present.pWaitSemaphores = &render_finished_;
        present.swapchainCount = 1;
        present.pSwapchains = &swapchain_;
        present.pImageIndices = &image;
        const VkResult presented = vkQueuePresentKHR(queue_, &present);
        if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR)
            recreate_swapchain();
        if (presented == VK_SUCCESS || presented == VK_SUBOPTIMAL_KHR) return RenderStatus::Ok;
        return is_fatal(presented) ? RenderStatus::Fatal : RenderStatus::Transient;
    }

    void shutdown() {
        if (device_ != VK_NULL_HANDLE) vkDeviceWaitIdle(device_);
        destroy_swapchain_objects();
        if (device_ != VK_NULL_HANDLE) {
            if (in_flight_) vkDestroyFence(device_, in_flight_, nullptr);
            if (render_finished_) vkDestroySemaphore(device_, render_finished_, nullptr);
            if (image_available_) vkDestroySemaphore(device_, image_available_, nullptr);
            if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
            if (vertex_buffer_) vkDestroyBuffer(device_, vertex_buffer_, nullptr);
            if (vertex_memory_) vkFreeMemory(device_, vertex_memory_, nullptr);
            if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
            if (pipeline_layout_) vkDestroyPipelineLayout(device_, pipeline_layout_, nullptr);
            if (render_pass_) vkDestroyRenderPass(device_, render_pass_, nullptr);
            vkDestroyDevice(device_, nullptr);
        }
        if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
        if (instance_) vkDestroyInstance(instance_, nullptr);
        *this = VulkanCube{};
    }

private:
    bool fail(const char *stage) { stage_ = stage; return false; }

    Mat4 compute_mvp(float rotation, float pitch) const {
        const float aspect = static_cast<float>(width_) / height_;
        const float f = 1.0f / std::tan(62.0f * 3.1415926535f / 360.0f);
        /* Vulkan clip space: depth 0..1, Y points down. */
        Mat4 projection{};
        projection.value[0] = f / aspect;
        projection.value[5] = -f;
        projection.value[10] = 100.0f / 99.9f;
        projection.value[11] = 1.0f;
        projection.value[14] = -10.0f / 99.9f;

        const float sy = std::sin(rotation), cy = std::cos(rotation);
        const float sx = std::sin(pitch), cx = std::cos(pitch);
        Mat4 yaw{};
        yaw.value[0] = cy;  yaw.value[2] = -sy;
        yaw.value[5] = 1.0f;
        yaw.value[8] = sy;  yaw.value[10] = cy;
        yaw.value[15] = 1.0f;
        Mat4 tilt{};
        tilt.value[0] = 1.0f;
        tilt.value[5] = cx;  tilt.value[6] = sx;
        tilt.value[9] = -sx; tilt.value[10] = cx;
        tilt.value[15] = 1.0f;
        Mat4 translate{};
        translate.value[0] = translate.value[5] = translate.value[10] = translate.value[15] = 1.0f;
        translate.value[14] = 5.0f;
        return multiply(projection, multiply(translate, multiply(tilt, yaw)));
    }

    bool create_instance() {
        VkApplicationInfo app = vk_struct<VkApplicationInfo>(VK_STRUCTURE_TYPE_APPLICATION_INFO);
        app.pApplicationName = "Enjoer";
        app.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        app.pEngineName = "Enjoer cube";
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

        extent_ = caps.currentExtent;
        if (extent_.width == 0xFFFFFFFFu) {
            extent_.width = std::min(std::max(static_cast<uint32_t>(width_),
                                              caps.minImageExtent.width),
                                     caps.maxImageExtent.width);
            extent_.height = std::min(std::max(static_cast<uint32_t>(height_),
                                               caps.minImageExtent.height),
                                      caps.maxImageExtent.height);
        }
        if (extent_.width == 0 || extent_.height == 0) return false;

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
        info.preTransform = caps.currentTransform;
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
        VkAttachmentDescription attachments[2]{};
        attachments[0].format = surface_format_;
        attachments[0].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachments[1].format = depth_format_;
        attachments[1].samples = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        attachments[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        attachments[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkAttachmentReference depth{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color;
        subpass.pDepthStencilAttachment = &depth;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                  VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstStageMask = dependency.srcStageMask;
        dependency.srcAccessMask = 0;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo info = vk_struct<VkRenderPassCreateInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO);
        info.attachmentCount = 2;
        info.pAttachments = attachments;
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

    bool create_depth() {
        const VkFormat candidates[] = {VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D32_SFLOAT,
                                       VK_FORMAT_D16_UNORM};
        depth_format_ = VK_FORMAT_UNDEFINED;
        for (VkFormat candidate : candidates) {
            VkFormatProperties props{};
            vkGetPhysicalDeviceFormatProperties(physical_, candidate, &props);
            if (props.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
                depth_format_ = candidate;
                break;
            }
        }
        if (depth_format_ == VK_FORMAT_UNDEFINED) return false;
        if (render_pass_ == VK_NULL_HANDLE && !create_render_pass()) return false;

        VkImageCreateInfo image = vk_struct<VkImageCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO);
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = depth_format_;
        image.extent = {extent_.width, extent_.height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (vkCreateImage(device_, &image, nullptr, &depth_image_) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetImageMemoryRequirements(device_, depth_image_, &requirements);
        VkMemoryAllocateInfo alloc = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = find_memory(requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX)
            alloc.memoryTypeIndex = find_memory(requirements.memoryTypeBits, 0);
        if (alloc.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(device_, &alloc, nullptr, &depth_memory_) != VK_SUCCESS) return false;
        vkBindImageMemory(device_, depth_image_, depth_memory_, 0);
        VkImageViewCreateInfo view = vk_struct<VkImageViewCreateInfo>(VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO);
        view.image = depth_image_;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = depth_format_;
        view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        return vkCreateImageView(device_, &view, nullptr, &depth_view_) == VK_SUCCESS;
    }

    bool create_framebuffers() {
        framebuffers_.assign(image_views_.size(), VK_NULL_HANDLE);
        for (size_t i = 0; i < image_views_.size(); ++i) {
            const VkImageView attachments[] = {image_views_[i], depth_view_};
            VkFramebufferCreateInfo info = vk_struct<VkFramebufferCreateInfo>(VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO);
            info.renderPass = render_pass_;
            info.attachmentCount = 2;
            info.pAttachments = attachments;
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

    bool create_pipeline() {
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(Mat4)};
        VkPipelineLayoutCreateInfo layout = vk_struct<VkPipelineLayoutCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO);
        layout.pushConstantRangeCount = 1;
        layout.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device_, &layout, nullptr, &pipeline_layout_) != VK_SUCCESS)
            return false;

        VkShaderModule vert = make_shader(cube_vert_spv, sizeof(cube_vert_spv));
        VkShaderModule frag = make_shader(cube_frag_spv, sizeof(cube_frag_spv));
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
        VkVertexInputAttributeDescription attributes[2]{};
        attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0};
        attributes[1] = {1, 0, VK_FORMAT_R32G32B32_SFLOAT, sizeof(float) * 3};
        VkPipelineVertexInputStateCreateInfo vertex_input = vk_struct<VkPipelineVertexInputStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO);
        vertex_input.vertexBindingDescriptionCount = 1;
        vertex_input.pVertexBindingDescriptions = &binding;
        vertex_input.vertexAttributeDescriptionCount = 2;
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
        raster.cullMode = VK_CULL_MODE_BACK_BIT;
        /* Y is flipped in the projection, so counter-clockwise data becomes
         * clockwise on screen. */
        raster.frontFace = VK_FRONT_FACE_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample = vk_struct<VkPipelineMultisampleStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO);
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depth = vk_struct<VkPipelineDepthStencilStateCreateInfo>(VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO);
        depth.depthTestEnable = VK_TRUE;
        depth.depthWriteEnable = VK_TRUE;
        depth.depthCompareOp = VK_COMPARE_OP_LESS;

        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
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

    bool create_vertex_buffer() {
        const auto &vertices = cube_vertices();
        VkBufferCreateInfo info = vk_struct<VkBufferCreateInfo>(VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO);
        info.size = sizeof(Vertex) * vertices.size();
        info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device_, &info, nullptr, &vertex_buffer_) != VK_SUCCESS) return false;
        VkMemoryRequirements requirements{};
        vkGetBufferMemoryRequirements(device_, vertex_buffer_, &requirements);
        VkMemoryAllocateInfo alloc = vk_struct<VkMemoryAllocateInfo>(VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO);
        alloc.allocationSize = requirements.size;
        alloc.memoryTypeIndex = find_memory(requirements.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (alloc.memoryTypeIndex == UINT32_MAX) return false;
        if (vkAllocateMemory(device_, &alloc, nullptr, &vertex_memory_) != VK_SUCCESS) return false;
        vkBindBufferMemory(device_, vertex_buffer_, vertex_memory_, 0);
        void *mapped = nullptr;
        if (vkMapMemory(device_, vertex_memory_, 0, info.size, 0, &mapped) != VK_SUCCESS)
            return false;
        std::memcpy(mapped, vertices.data(), static_cast<size_t>(info.size));
        vkUnmapMemory(device_, vertex_memory_);
        return true;
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

    void record(uint32_t image, const Mat4 &mvp) {
        VkCommandBuffer cmd = command_buffers_[image];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo begin = vk_struct<VkCommandBufferBeginInfo>(VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO);
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &begin);

        VkClearValue clears[2]{};
        clears[0].color = {{0.11f, 0.20f, 0.42f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo pass = vk_struct<VkRenderPassBeginInfo>(VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO);
        pass.renderPass = render_pass_;
        pass.framebuffer = framebuffers_[image];
        pass.renderArea = {{0, 0}, extent_};
        pass.clearValueCount = 2;
        pass.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &pass, VK_SUBPASS_CONTENTS_INLINE);

        VkViewport viewport{0.0f, 0.0f, static_cast<float>(extent_.width),
                            static_cast<float>(extent_.height), 0.0f, 1.0f};
        VkRect2D scissor{{0, 0}, extent_};
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer_, &offset);
        vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                           sizeof(Mat4), mvp.value);
        vkCmdDraw(cmd, static_cast<uint32_t>(cube_vertices().size()), 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
    }

    void destroy_swapchain_objects() {
        if (device_ == VK_NULL_HANDLE) return;
        for (VkFramebuffer fb : framebuffers_) if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        framebuffers_.clear();
        if (depth_view_) vkDestroyImageView(device_, depth_view_, nullptr);
        if (depth_image_) vkDestroyImage(device_, depth_image_, nullptr);
        if (depth_memory_) vkFreeMemory(device_, depth_memory_, nullptr);
        depth_view_ = VK_NULL_HANDLE; depth_image_ = VK_NULL_HANDLE; depth_memory_ = VK_NULL_HANDLE;
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
        if (!create_depth()) return;
        if (!create_framebuffers()) return;
        if (command_buffers_.size() != images_.size()) {
            vkFreeCommandBuffers(device_, command_pool_,
                                 static_cast<uint32_t>(command_buffers_.size()),
                                 command_buffers_.data());
            allocate_command_buffers();
        }
    }

    bool initialized_ = false;
    const char *stage_ = nullptr;
    int width_ = 1, height_ = 1;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    uint32_t queue_family_ = 0;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkFormat surface_format_ = VK_FORMAT_R8G8B8A8_UNORM;
    VkFormat depth_format_ = VK_FORMAT_UNDEFINED;
    VkExtent2D extent_{1, 1};
    std::vector<VkImage> images_;
    std::vector<VkImageView> image_views_;
    std::vector<VkFramebuffer> framebuffers_;
    VkImage depth_image_ = VK_NULL_HANDLE;
    VkDeviceMemory depth_memory_ = VK_NULL_HANDLE;
    VkImageView depth_view_ = VK_NULL_HANDLE;
    VkRenderPass render_pass_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;
    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> command_buffers_;
    VkSemaphore image_available_ = VK_NULL_HANDLE;
    VkSemaphore render_finished_ = VK_NULL_HANDLE;
    VkFence in_flight_ = VK_NULL_HANDLE;
};

static VulkanCube vulkan_cube;
static bool vulkan_active;
static int vulkan_failures;
#endif

#if ENJOER_USE_VULKAN
/* Give up on Vulkan and keep the cube alive on the CPU instead of freezing on
 * a black screen. A later APP_CMD_INIT_WINDOW gives Vulkan another chance. */
static void switch_to_software(const char *reason) {
    app_log_error("Enjoer: %s - falling back to the CPU rasterizer", reason);
    vulkan_cube.shutdown();
    vulkan_active = false;
    vulkan_failures = 0;
    software_active = true;
#ifdef __ANDROID__
    request_window_format(native_window);
#endif
}
#endif

} // namespace

extern "C" int cube_renderer_init(void *window, int w, int h) {
    native_window = window;
    width = std::max(1, w);
    height = std::max(1, h);
    software_active = false;
    const Backend requested = requested_backend();

#if ENJOER_USE_VULKAN
    if (requested != Backend::Software && native_window &&
        vulkan_cube.init(native_window, width, height)) {
        vulkan_active = true;
        return 1;
    }
    if (native_window) {
        if (requested == Backend::Vulkan) {
            app_log_error("Enjoer: ENJOER_RENDERER=vulkan was requested, but Vulkan failed (%s)",
                          vulkan_cube.stage());
            vulkan_cube.shutdown();
            return 0;
        }
        app_log_error("Enjoer: no usable Vulkan device (%s) - drawing the cube on the CPU",
                      vulkan_cube.stage());
        vulkan_cube.shutdown();
    }
#endif

    software_active = true;
    if (requested == Backend::Software)
        app_log("Enjoer: CPU rasterizer requested through ENJOER_RENDERER");
#ifdef __ANDROID__
    if (native_window) request_window_format(native_window);
#endif
    return 1;
}

extern "C" void cube_renderer_resize(int w, int h) {
    width = std::max(1, w);
    height = std::max(1, h);
#if ENJOER_USE_VULKAN
    if (vulkan_active) {
        vulkan_cube.resize(width, height);
        return;
    }
#endif
#ifdef __ANDROID__
    if (software_active && native_window) request_window_format(native_window);
#endif
}

extern "C" void cube_renderer_render(Buffer *preview_target, float rotation, float pitch) {
#if ENJOER_USE_VULKAN
    if (vulkan_active) {
        const RenderStatus status = vulkan_cube.render(rotation, pitch);
        if (status == RenderStatus::Ok) {
            vulkan_failures = 0;
            return;
        }
        if (status == RenderStatus::Fatal) switch_to_software("the Vulkan device was lost");
        else if (++vulkan_failures < kMaxVulkanFailures) return;
        else switch_to_software("Vulkan stopped presenting frames");
    }
#endif
    if (preview_target) {
        /* The HTTP preview has no window: draw at the requested resolution. */
        rasterize_cube(preview_target, preview_depth, rotation, pitch);
        return;
    }
#ifdef __ANDROID__
    software_present_to_window(rotation, pitch);
#endif
}

extern "C" void cube_renderer_shutdown(void) {
#if ENJOER_USE_VULKAN
    if (vulkan_active) vulkan_cube.shutdown();
    vulkan_active = false;
    vulkan_failures = 0;
#endif
    software_active = false;
    software_surface.release();
    native_window = nullptr;
}

extern "C" const char *cube_renderer_backend(void) {
#if ENJOER_USE_VULKAN
    if (vulkan_active) return "Vulkan";
    return "CPU rasterizer (Vulkan unavailable)";
#else
    return "CPU rasterizer (built without Vulkan)";
#endif
}

extern "C" int cube_renderer_software_active(void) {
#if ENJOER_USE_VULKAN
    return vulkan_active ? 0 : 1;
#else
    return 1;
#endif
}

extern "C" int cube_software_render(Buffer *target, float rotation, float pitch) {
    if (!target || !target->pixels) return 0;
    software_render_into(target, rotation, pitch);
    return 1;
}
