/*
 * The C++ half of Enjoer: a deliberately tiny WebGPU/Dawn renderer.
 *
 * The Dawn path is compiled when ENJOER_USE_DAWN is enabled.  Keeping the
 * preview fallback in this translation unit is useful on a fresh checkout:
 * the C game and its controls can be tested without downloading Dawn, while
 * Android/device builds use exactly the same cube data and WGSL shader.
 */
#include "dawn_cube.h"

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

#ifndef ENJOER_USE_DAWN
#define ENJOER_USE_DAWN 0
#endif

#if ENJOER_USE_DAWN
#if __has_include(<dawn/native/DawnNative.h>)
#include <dawn/native/DawnNative.h>
#else
#include <dawn_native/DawnNative.h>
namespace dawn { namespace native = ::dawn_native; }
#endif
#if __has_include(<dawn/webgpu_cpp.h>)
#include <dawn/webgpu_cpp.h>
#else
#include <webgpu/webgpu_cpp.h>
#endif
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

static void render_fallback(Buffer *buffer, float rotation, float pitch) {
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

    std::vector<float> depth(static_cast<size_t>(buffer->width) * buffer->height,
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

#if ENJOER_USE_DAWN

/* A compact column-major 4 by 4 matrix, matching WGSL's mat4x4 layout. */
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

class DawnCube {
public:
    bool init(void *native_window, int width, int height) {
        width_ = std::max(1, width);
        height_ = std::max(1, height);
        native_instance_.DiscoverDefaultAdapters();
        const auto adapters = native_instance_.GetAdapters();
        if (adapters.empty()) return false;

        adapter_ = wgpu::Adapter::Acquire(adapters.front().Get());
        device_ = wgpu::Device::Acquire(adapters.front().CreateDevice());
        if (!device_) return false;
        queue_ = device_.GetQueue();
        instance_ = wgpu::Instance::Acquire(native_instance_.Get());

#ifdef __ANDROID__
        wgpu::SurfaceDescriptorFromAndroidNativeWindow source{};
        source.window = static_cast<ANativeWindow *>(native_window);
        wgpu::SurfaceDescriptor surface_desc{};
        surface_desc.nextInChain = &source;
        surface_ = instance_.CreateSurface(&surface_desc);
#else
        (void)native_window;
#endif
        if (!surface_) return false;
        configure_surface();
        make_pipeline();
        initialized_ = pipeline_ && surface_;
        return initialized_;
    }

    void resize(int width, int height) {
        width_ = std::max(1, width);
        height_ = std::max(1, height);
        if (initialized_) configure_surface();
    }

    void render(float rotation, float pitch) {
        if (!initialized_) return;
        wgpu::SurfaceTexture acquired{};
        surface_.GetCurrentTexture(&acquired);
        if (acquired.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal &&
            acquired.status != wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal) return;
        wgpu::TextureView view = acquired.texture.CreateView();
        if (!view) return;

        const float aspect = static_cast<float>(width_) / height_;
        const float f = 1.0f / std::tan(62.0f * 3.1415926535f / 360.0f);
        Mat4 projection{};
        projection.value[0] = f / aspect;
        projection.value[5] = f;
        projection.value[10] = 100.0f / 99.9f;
        projection.value[11] = 1.0f;
        projection.value[14] = -10.0f / 99.9f;

        const float sy = std::sin(rotation), cy = std::cos(rotation);
        const float sx = std::sin(pitch), cx = std::cos(pitch);
        Mat4 model{};
        model.value[0] = cy; model.value[2] = -sy;
        model.value[5] = cx; model.value[6] = -sx;
        model.value[8] = sy; model.value[9] = sx; model.value[10] = cx;
        model.value[15] = 1.0f;
        model.value[12] = 0.0f; model.value[13] = 0.0f; model.value[14] = 5.0f;
        const Mat4 mvp = multiply(projection, model);
        queue_.WriteBuffer(uniform_buffer_, 0, mvp.value, sizeof(mvp.value));

        wgpu::CommandEncoder encoder = device_.CreateCommandEncoder();
        wgpu::RenderPassColorAttachment color{};
        color.view = view;
        color.loadOp = wgpu::LoadOp::Clear;
        color.storeOp = wgpu::StoreOp::Store;
        color.clearValue = {0.10, 0.18, 0.35, 1.0};
        wgpu::RenderPassDepthStencilAttachment depth{};
        depth.view = depth_view_;
        depth.depthLoadOp = wgpu::LoadOp::Clear;
        depth.depthStoreOp = wgpu::StoreOp::Store;
        depth.depthClearValue = 1.0f;
        wgpu::RenderPassDescriptor pass{};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments = &color;
        pass.depthStencilAttachment = &depth;
        wgpu::RenderPassEncoder render_pass = encoder.BeginRenderPass(&pass);
        render_pass.SetPipeline(pipeline_);
        render_pass.SetBindGroup(0, bind_group_);
        render_pass.SetVertexBuffer(0, vertex_buffer_);
        render_pass.Draw(static_cast<uint32_t>(cube_vertices().size()), 1, 0, 0);
        render_pass.End();
        wgpu::CommandBuffer commands = encoder.Finish();
        queue_.Submit(1, &commands);
        surface_.Present();
    }

private:
    void configure_surface() {
        wgpu::SurfaceCapabilities capabilities{};
        surface_.GetCapabilities(adapter_, &capabilities);
        if (capabilities.formatCount > 0) surface_format_ = capabilities.formats[0];
        wgpu::SurfaceConfiguration config{};
        config.device = device_;
        config.format = surface_format_;
        config.usage = wgpu::TextureUsage::RenderAttachment;
        config.width = static_cast<uint32_t>(width_);
        config.height = static_cast<uint32_t>(height_);
        config.presentMode = wgpu::PresentMode::Fifo;
        config.alphaMode = wgpu::CompositeAlphaMode::Opaque;
        surface_.Configure(&config);
        wgpu::TextureDescriptor depth{};
        depth.size = {config.width, config.height, 1};
        depth.format = wgpu::TextureFormat::Depth24Plus;
        depth.usage = wgpu::TextureUsage::RenderAttachment;
        depth_texture_ = device_.CreateTexture(&depth);
        depth_view_ = depth_texture_.CreateView();
    }

    void make_pipeline() {
        static const char *shader_text = R"WGSL(
struct Uniforms { mvp: mat4x4<f32> };
@group(0) @binding(0) var<uniform> uniforms: Uniforms;
struct VertexIn { @location(0) position: vec3<f32>, @location(1) color: vec3<f32> };
struct VertexOut { @builtin(position) position: vec4<f32>, @location(0) color: vec3<f32> };
@vertex fn vs_main(input: VertexIn) -> VertexOut {
  var out: VertexOut;
  out.position = uniforms.mvp * vec4<f32>(input.position, 1.0);
  out.color = input.color;
  return out;
}
@fragment fn fs_main(input: VertexOut) -> @location(0) vec4<f32> {
  return vec4<f32>(input.color, 1.0);
}
)WGSL";
        wgpu::ShaderModuleWGSLDescriptor wgsl{};
        wgsl.code = shader_text;
        wgpu::ShaderModuleDescriptor shader_desc{};
        shader_desc.nextInChain = &wgsl;
        wgpu::ShaderModule shader = device_.CreateShaderModule(&shader_desc);

        wgpu::BindGroupLayoutEntry uniform_entry{};
        uniform_entry.binding = 0;
        uniform_entry.visibility = wgpu::ShaderStage::Vertex;
        uniform_entry.buffer.type = wgpu::BufferBindingType::Uniform;
        uniform_entry.buffer.minBindingSize = sizeof(float) * 16;
        wgpu::BindGroupLayoutDescriptor group_desc{};
        group_desc.entryCount = 1;
        group_desc.entries = &uniform_entry;
        wgpu::BindGroupLayout group = device_.CreateBindGroupLayout(&group_desc);
        wgpu::PipelineLayoutDescriptor layout_desc{};
        layout_desc.bindGroupLayoutCount = 1;
        layout_desc.bindGroupLayouts = &group;
        wgpu::PipelineLayout layout = device_.CreatePipelineLayout(&layout_desc);

        wgpu::VertexAttribute attributes[2]{};
        attributes[0].format = wgpu::VertexFormat::Float32x3;
        attributes[0].offset = 0;
        attributes[0].shaderLocation = 0;
        attributes[1].format = wgpu::VertexFormat::Float32x3;
        attributes[1].offset = sizeof(float) * 3;
        attributes[1].shaderLocation = 1;
        wgpu::VertexBufferLayout vertex_layout{};
        vertex_layout.arrayStride = sizeof(float) * 6;
        vertex_layout.attributeCount = 2;
        vertex_layout.attributes = attributes;

        wgpu::ColorTargetState target{};
        target.format = surface_format_;
        wgpu::FragmentState fragment{};
        fragment.module = shader;
        fragment.entryPoint = "fs_main";
        fragment.targetCount = 1;
        fragment.targets = &target;
        wgpu::DepthStencilState depth{};
        depth.format = wgpu::TextureFormat::Depth24Plus;
        depth.depthWriteEnabled = true;
        depth.depthCompare = wgpu::CompareFunction::Less;
        wgpu::RenderPipelineDescriptor pipeline_desc{};
        pipeline_desc.layout = layout;
        pipeline_desc.vertex.module = shader;
        pipeline_desc.vertex.entryPoint = "vs_main";
        pipeline_desc.vertex.bufferCount = 1;
        pipeline_desc.vertex.buffers = &vertex_layout;
        pipeline_desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
        pipeline_desc.primitive.cullMode = wgpu::CullMode::Back;
        pipeline_desc.fragment = &fragment;
        pipeline_desc.depthStencil = &depth;
        pipeline_ = device_.CreateRenderPipeline(&pipeline_desc);

        wgpu::BufferDescriptor vertex_desc{};
        vertex_desc.size = sizeof(Vertex) * cube_vertices().size();
        vertex_desc.usage = wgpu::BufferUsage::Vertex | wgpu::BufferUsage::CopyDst;
        vertex_buffer_ = device_.CreateBuffer(&vertex_desc);
        queue_.WriteBuffer(vertex_buffer_, 0, cube_vertices().data(), vertex_desc.size);
        wgpu::BufferDescriptor uniform_desc{};
        uniform_desc.size = sizeof(float) * 16;
        uniform_desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
        uniform_buffer_ = device_.CreateBuffer(&uniform_desc);
        wgpu::BindGroupEntry entry{};
        entry.binding = 0;
        entry.buffer = uniform_buffer_;
        entry.size = uniform_desc.size;
        wgpu::BindGroupDescriptor bind_desc{};
        bind_desc.layout = group;
        bind_desc.entryCount = 1;
        bind_desc.entries = &entry;
        bind_group_ = device_.CreateBindGroup(&bind_desc);
    }

    bool initialized_ = false;
    int width_ = 1, height_ = 1;
    dawn::native::Instance native_instance_;
    wgpu::Instance instance_;
    wgpu::Adapter adapter_;
    wgpu::Device device_;
    wgpu::Queue queue_;
    wgpu::Surface surface_;
    wgpu::RenderPipeline pipeline_;
    wgpu::Buffer vertex_buffer_, uniform_buffer_;
    wgpu::BindGroup bind_group_;
    wgpu::Texture depth_texture_;
    wgpu::TextureView depth_view_;
    wgpu::TextureFormat surface_format_ = wgpu::TextureFormat::BGRA8Unorm;
};

static DawnCube dawn_cube;
static bool dawn_active;
#endif

static void *native_window;
static int width = 1;
static int height = 1;

} // namespace

extern "C" int cube_renderer_init(void *window, int w, int h) {
    native_window = window;
    width = std::max(1, w);
    height = std::max(1, h);
#if ENJOER_USE_DAWN
    if (native_window && dawn_cube.init(native_window, width, height)) {
        dawn_active = true;
        return 1;
    }
    /* A preview has no native window. A Dawn Android build should fail loudly
     * instead of silently switching away from GPU presentation. */
    if (native_window) return 0;
#endif
    return 1;
}

extern "C" void cube_renderer_resize(int w, int h) {
    width = std::max(1, w);
    height = std::max(1, h);
#if ENJOER_USE_DAWN
    if (dawn_active) dawn_cube.resize(width, height);
#endif
}

extern "C" void cube_renderer_render(Buffer *preview_target, float rotation, float pitch) {
#if ENJOER_USE_DAWN
    if (dawn_active) {
        dawn_cube.render(rotation, pitch);
        return;
    }
#endif
    if (preview_target) {
        render_fallback(preview_target, rotation, pitch);
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
            render_fallback(&target, rotation, pitch);
            ANativeWindow_unlockAndPost(window);
        }
    }
#endif
}

extern "C" void cube_renderer_shutdown(void) {
#if ENJOER_USE_DAWN
    dawn_active = false;
#endif
    native_window = nullptr;
}

extern "C" const char *cube_renderer_backend(void) {
#if ENJOER_USE_DAWN
    return dawn_active ? "Dawn / WebGPU" : "Dawn-compatible preview fallback";
#else
    return "C++ preview fallback (enable Dawn for GPU presentation)";
#endif
}
