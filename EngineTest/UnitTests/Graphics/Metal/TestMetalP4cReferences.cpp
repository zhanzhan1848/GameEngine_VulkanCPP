/**
 * @file TestMetalP4cReferences.cpp
 * @brief P4c 双后端对等 — Metal 侧参照帧生成器
 * @details 复用 TestMetalSimplePBR 的嵌入式 MSL 离屏渲染模式,为
 *          TestVulkanFormatParity / TextureUpdate / StagingUpload /
 *          LayeredRendering / ComputeBytesLarge / SecondaryCommandBuffer
 *          生成跨后端 SSIM 验收所需的参照 PNG:
 *
 *   F1  Assets/ReferenceImages/P4c-F1/format_<name>.png     (12 个浮点家族格式渐变)
 *       Assets/ReferenceImages/P4c-F1/depth16_sphere_metal.png (D16 DepthPrePass)
 *   F3  Assets/ReferenceImages/P4c-F3/checkerboard_metal.png   (8x8 棋盘格采样)
 *   F4  Assets/ReferenceImages/P4c-F4/rolling_final_metal.png  (300 帧滚动终帧,updateData 上传)
 *   F5  Assets/ReferenceImages/P4c-F5/layer<u>_metal.png       (4 层 layered 渐变)
 *   F6  Assets/ReferenceImages/P4c-F6/compute512_metal.png     (512B setBytes compute)
 *   F7  Assets/ReferenceImages/P4c-F7/tiles32_metal.png        (32 tile,parallel secondary 绘制)
 *
 * 朝向约定:Vulkan 测试在读回后做 FlipYInPlace 的用例(F1 渐变/深度、F5、
 * F7 之外的渲染类)对应 MSL 中镜像 v;F3/F4/F6/F7 为上传/compute 路径,
 * 无翻转。逐项注释标明。
 */

#ifdef __APPLE__

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"

#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalTexture.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <cmath>
#include <filesystem>
#include <memory>
#include <algorithm>

using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;
namespace et = EngineTest;

namespace {

constexpr u32 kW = 64, kH = 64;

// ============================================================================
// 嵌入式 MSL
// ============================================================================

// F1/F5 渐变(镜像 v:Vulkan 测试读回后 FlipY,Metal 侧用 64-y 对齐朝向)
static const char* kGradientVS = R"MSL(
#include <metal_stdlib>
using namespace metal;
vertex float4 gradientVS(uint vid [[vertex_id]]) {
    float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
    return float4(uv * 2.0 - 1.0, 0.0, 1.0);
}
)MSL";

static const char* kGradientFS = R"MSL(
#include <metal_stdlib>
using namespace metal;
fragment float4 gradientFS(float4 pos [[position]]) {
    // 镜像 v:对齐 Vulkan 测试 FlipYInPlace 后的朝向
    float2 uv = float2(pos.x, 64.0 - pos.y) / 64.0;
    float3 c = float3(uv.x, uv.y, 0.25 + 0.5 * uv.x * uv.y);
    float2 d = uv - float2(0.5, 0.5);
    if (dot(d, d) < 0.15 * 0.15) c = float3(0.9, 0.1, 0.3);
    return float4(clamp(c, float3(0.0), float3(1.0)), 1.0);
}
)MSL";

// F3 棋盘格采样(Metal 帧缓冲的插值朝向与 Vulkan 相反 → 镜像 v;
// Vulkan 测试读回不翻转,此处镜像后两端同向)
static const char* kSampleVS = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VSOut { float4 pos [[position]]; float2 uv; };
vertex VSOut sampleVS(uint vid [[vertex_id]]) {
    float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
    VSOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.uv = float2(uv.x, 1.0 - uv.y);
    return o;
}
)MSL";

static const char* kSampleFS = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VSOut { float4 pos [[position]]; float2 uv; };
fragment float4 sampleFS(VSOut in [[stage_in]],
                         texture2d<float> tex [[texture(0)]],
                         sampler smp [[sampler(0)]]) {
    return tex.sample(smp, in.uv);
}
)MSL";

// F5 layered:render_target_array_index 由 vertex 输出(镜像 v 同 F1)
static const char* kLayeredVS = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VSOut {
    float4 pos [[position]];
    uint  rtai [[render_target_array_index]];
};
vertex VSOut layeredVS(uint vid [[vertex_id]], uint iid [[instance_id]]) {
    float2 uv = float2(float((vid << 1) & 2), float(vid & 2));
    VSOut o;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.rtai = iid;
    return o;
}
)MSL";

static const char* kLayeredFS = R"MSL(
#include <metal_stdlib>
using namespace metal;
fragment float4 layeredFS(float4 pos [[position]], uint layer [[render_target_array_index]]) {
    float l = float(layer);
    float2 uv = float2(pos.x, 64.0 - pos.y) / 64.0;   // 镜像 v
    float4 c = float4(l / 3.0, 1.0 - l / 3.0, 0.25 + 0.1 * l, 1.0);
    float2 cc = float2(0.2 + 0.15 * l, 0.5);
    float2 d = uv - cc;
    if (dot(d, d) < 0.01) c = float4(1.0);
    return c;
}
)MSL";

// F6 compute(512B setBytes;内容 = Vulkan blob 的前 512B 探针)
static const char* kComputeMS = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct BigBlock { float4 v[32]; };   // 512B
kernel void computeMain(texture2d<float, access::write> out [[texture(0)]],
                        constant BigBlock& big [[buffer(0)]],
                        uint2 gid [[thread_position_in_grid]]) {
    out.write(float4(big.v[0].x, big.v[8].y, big.v[31].z, 1.0), gid);
}
)MSL";

// F7 32 tile:颜色烘焙进顶点数据(每个 tile 一个 24B 顶点缓冲)
static const char* kTileVS = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct TileIn { packed_float2 pos; packed_float4 color; };   // 24B(color@8)
struct VSOut { float4 pos [[position]]; float4 color; };
vertex VSOut tileVS(uint vid [[vertex_id]], constant TileIn* verts [[buffer(20)]]) {
    TileIn v = verts[vid];
    VSOut o;
    o.pos = float4(float2(v.pos), 0.0, 1.0);
    o.color = v.color;
    return o;
}
)MSL";

static const char* kTileFS = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct VSOut { float4 pos [[position]]; float4 color; };
fragment float4 tileFS(VSOut in [[stage_in]]) { return in.color; }
)MSL";

// F1 D16 DepthPrePass(克隆 TestMetalDepthPrePass 的 camera-depth 顶点)
static const char* kDepthVS = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct CameraDepthPerObject { float4x4 worldViewProjection; };
vertex float4 depthVS(uint vid [[vertex_id]],
                      constant CameraDepthPerObject& ubo [[buffer(0)]],
                      constant float* verts [[buffer(20)]]) {
    // 32B stride: pos(3f) + pad(3u32-as-float) + uv(2f) — 与 Vulkan 侧
    // TestVulkanFormatParity 的 sphere 布局一致,shader 只读 pos。
    float3 p = float3(verts[vid * 8], verts[vid * 8 + 1], verts[vid * 8 + 2]);
    return ubo.worldViewProjection * float4(p, 1.0);
}
)MSL";

static const char* kNoopFS = R"MSL(
#include <metal_stdlib>
using namespace metal;
fragment float4 noopFS() { return float4(0.0); }
)MSL";

// ============================================================================
// 通用 helper
// ============================================================================
struct MetalFixture {
    std::unique_ptr<MetalDevice> device;
    bool Init() {
        DeviceDesc d;
        d.platform = RHIPlatform::Metal;
        d.enableDebug = true;
        device = std::make_unique<MetalDevice>(d);
        return device->Initialize();
    }
};

ShaderHandle MakeShader(MetalDevice& device, const char* src, ShaderStage stage, const char* entry) {
    return device.CreateShader(src, std::strlen(src), stage, entry);
}

ResourceHandle MakeRT(MetalDevice& device, DataFormat format, TextureUsage usage, u32 w = kW, u32 h = kH,
                      u32 arraySize = 1, TextureType type = TextureType::Texture2D) {
    TextureDesc td{};
    td.size = {w, h, 1};
    td.mipLevels = 1;
    td.arraySize = arraySize;
    td.format = format;
    td.type = type;
    td.usage = usage | TextureUsage::CopySource;
    td.memoryUsage = GPUMemoryUsage::Static;
    td.name = "P4cRefRT";
    return device.CreateTexture(td);
}

/// 整张 readback(RGBA8 类);baseArrayLayer 可选(数组纹理逐层)
std::vector<u8> ReadbackLayer(MetalDevice& device, ResourceHandle tex, u32 layer, u32 w, u32 h,
                              u64 bytesPerPixel) {
    BufferDesc rbd{};
    rbd.size = u64(w) * h * bytesPerPixel;
    rbd.type = BufferType::Raw;
    rbd.memoryUsage = GPUMemoryUsage::Readback;
    rbd.name = "P4cRefReadback";
    ResourceHandle rb = device.CreateBuffer(rbd);
    if (rb == handles::INVALID_RESOURCE) return {};
    CommandBufferHandle cmd = device.CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* mcmd = device.GetCommandBuffer(cmd);
    std::vector<u8> out;
    if (mcmd && mcmd->Reset() && mcmd->Begin()) {
        BufferTextureCopyRegion region{};
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {w, h, 1};
        mcmd->CopyTextureToBuffer(tex, rb, &region, 1);
        if (mcmd->End() && mcmd->Submit(0) && mcmd->WaitForCompletion()) {
            void* mapped = device.MapBuffer(rb, 0, rbd.size);
            if (mapped) {
                out.resize(size_t(rbd.size));
                std::memcpy(out.data(), mapped, size_t(rbd.size));
                device.UnmapBuffer(rb);
            }
        }
    }
    device.DestroyCommandBuffer(cmd);
    device.DestroyBuffer(rb);
    return out;
}

bool SaveRef(const char* relPath, const std::vector<u8>& rgba, u32 w, u32 h) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(relPath).parent_path(), ec);
    if (et::SavePNG(relPath, rgba.data(), w, h)) {
        std::cout << "[TestMetalP4cRef] saved " << relPath << std::endl;
        return true;
    }
    std::cerr << "[TestMetalP4cRef] SavePNG failed: " << relPath << std::endl;
    return false;
}

/// 单通道原始值 → [0,255](对齐 Vulkan 测试的 ChannelToByte)
u8 ChannelToByte(bool unorm16, bool snorm16, bool isFloat32, const u8* raw) {
    auto u16v = [&]() -> u16 { u16 v; std::memcpy(&v, raw, 2); return v; };
    auto s16v = [&]() -> s16 { s16 v; std::memcpy(&v, raw, 2); return v; };
    auto f32v = [&]() -> float { float v; std::memcpy(&v, raw, 4); return v; };
    float c = 0.0f;
    if (unorm16) c = u16v() / 65535.0f;
    else if (snorm16) { float s = s16v() / 32767.0f; c = s < 0 ? 0 : s; }
    else if (isFloat32) c = f32v();
    if (c < 0) c = 0;
    if (c > 1) c = 1;
    return static_cast<u8>(c * 255.0f + 0.5f);
}

// ============================================================================
// F1 D16 DepthPrePass 参照(克隆 TestMetalDepthPrePass;Metal 帧缓冲 Y 顶起,
// Vulkan 测试读回后 FlipY 对齐 — 与 T3 的 D32 参照同款朝向约定)
// ============================================================================
m4x4 ref_identity_m4x4() {
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = 1.0f; r.columns[1][1] = 1.0f;
    r.columns[2][2] = 1.0f; r.columns[3][3] = 1.0f;
    return r;
}
m4x4 ref_view_m4x4(primal::math::v3 eye, primal::math::v3 target, primal::math::v3 up) {
    using primal::math::v3;
    v3 diff = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
    float zl = std::sqrt(diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2]);
    if (zl < 1e-8f) zl = 1.0f;
    v3 zaxis = {diff[0]/zl, diff[1]/zl, diff[2]/zl};
    v3 x = {up[1]*zaxis[2] - up[2]*zaxis[1],
            up[2]*zaxis[0] - up[0]*zaxis[2],
            up[0]*zaxis[1] - up[1]*zaxis[0]};
    float xl = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
    if (xl < 1e-8f) xl = 1.0f;
    v3 xaxis = {x[0]/xl, x[1]/xl, x[2]/xl};
    v3 yaxis = {zaxis[1]*xaxis[2] - zaxis[2]*xaxis[1],
                zaxis[2]*xaxis[0] - zaxis[0]*xaxis[2],
                zaxis[0]*xaxis[1] - zaxis[1]*xaxis[0]};
    m4x4 m = ref_identity_m4x4();
    m.columns[0][0] = xaxis[0]; m.columns[0][1] = yaxis[0]; m.columns[0][2] = zaxis[0];
    m.columns[1][0] = xaxis[1]; m.columns[1][1] = yaxis[1]; m.columns[1][2] = zaxis[1];
    m.columns[2][0] = xaxis[2]; m.columns[2][1] = yaxis[1]; m.columns[2][2] = zaxis[2];
    m.columns[3][0] = -(xaxis[0]*eye[0] + xaxis[1]*eye[1] + xaxis[2]*eye[2]);
    m.columns[3][1] = -(yaxis[0]*eye[0] + yaxis[1]*eye[1] + yaxis[2]*eye[2]);
    m.columns[3][2] = -(zaxis[0]*eye[0] + zaxis[1]*eye[1] + zaxis[2]*eye[2]);
    m.columns[3][3] = 1.0f;
    return m;
}
m4x4 ref_perspective_m4x4(float fov_y, float aspect, float zNear, float zFar) {
    float f = 1.0f / std::tan(fov_y * 0.5f);
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = f / aspect;
    r.columns[1][1] = f;
    r.columns[2][2] = (zFar + zNear) / (zNear - zFar);
    r.columns[2][3] = -1.0f;
    r.columns[3][2] = (2.0f * zFar * zNear) / (zNear - zFar);
    return r;
}

/// 32B/顶点球体(与 Vulkan TestVulkanFormatParity 的 D16 用例同布局)
struct RefSphereMesh {
    std::vector<u8> vertices;
    std::vector<u32> indices;
    u32 vertexCount{0}, indexCount{0};
};
RefSphereMesh ref_make_sphere(float radius, u32 segments, u32 rings) {
    constexpr u32 stride = 32;
    RefSphereMesh s;
    s.vertexCount = (rings + 1) * (segments + 1);
    s.indexCount = rings * segments * 6;
    s.vertices.resize(size_t(s.vertexCount) * stride);
    s.indices.resize(s.indexCount);
    auto write_v = [&](u32 i, float px, float py, float pz,
                       float nx, float ny, float nz, float u, float v) {
        u8* p = s.vertices.data() + size_t(i) * stride;
        std::memcpy(p + 0, &px, 4); std::memcpy(p + 4, &py, 4); std::memcpy(p + 8, &pz, 4);
        std::memcpy(p + 12, &nx, 4); std::memcpy(p + 16, &ny, 4); std::memcpy(p + 20, &nz, 4);
        std::memcpy(p + 24, &u, 4); std::memcpy(p + 28, &v, 4);
    };
    u32 vi = 0;
    for (u32 r = 0; r <= rings; ++r) {
        float phi = 3.14159265358979f * float(r) / float(rings);
        float sinP = std::sin(phi), cosP = std::cos(phi);
        for (u32 seg = 0; seg <= segments; ++seg) {
            float theta = 2.0f * 3.14159265358979f * float(seg) / float(segments);
            float nx = sinP * std::cos(theta), ny = cosP, nz = sinP * std::sin(theta);
            write_v(vi, nx * radius, ny * radius, nz * radius, nx, ny, nz,
                    float(seg) / float(segments), float(r) / float(rings));
            ++vi;
        }
    }
    u32 ii = 0;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 seg = 0; seg < segments; ++seg) {
            u32 a = r * (segments + 1) + seg;
            u32 b = a + segments + 1;
            s.indices[ii++] = a;     s.indices[ii++] = b; s.indices[ii++] = a + 1;
            s.indices[ii++] = a + 1; s.indices[ii++] = b; s.indices[ii++] = b + 1;
        }
    }
    return s;
}

TestResult Generate_F1_Depth16() {
    MetalFixture fx;
    TEST_ASSERT(fx.Init(), "MetalDevice init");

    ShaderHandle vs = MakeShader(*fx.device, kDepthVS, ShaderStage::Vertex, "depthVS");
    ShaderHandle fs = MakeShader(*fx.device, kNoopFS, ShaderStage::Pixel, "noopFS");
    TEST_ASSERT(vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER, "shaders");

    RefSphereMesh sphere = ref_make_sphere(1.0f, 24, 16);
    BufferDesc vbd{};
    vbd.size = sphere.vertices.size();
    vbd.type = BufferType::Vertex;
    vbd.vertex.vertexCount = sphere.vertexCount;
    vbd.vertex.vertexStride = 32;
    vbd.memoryUsage = GPUMemoryUsage::Dynamic;
    vbd.name = "SphereVB";
    ResourceHandle vb = fx.device->CreateBuffer(vbd);
    TEST_ASSERT(fx.device->UpdateBufferData(vb, sphere.vertices.data(), sphere.vertices.size(), 0), "VB");
    BufferDesc ibd{};
    ibd.size = sphere.indices.size() * sizeof(u32);
    ibd.type = BufferType::Index;
    ibd.index.indexCount = sphere.indexCount;
    ibd.index.format = DataFormat::R32_UInt;
    ibd.memoryUsage = GPUMemoryUsage::Dynamic;
    ibd.name = "SphereIB";
    ResourceHandle ib = fx.device->CreateBuffer(ibd);
    TEST_ASSERT(fx.device->UpdateBufferData(ib, sphere.indices.data(), ibd.size, 0), "IB");

    using primal::math::v3;
    m4x4 view = ref_view_m4x4(v3{0.0f, 0.0f, 3.0f}, v3{0.0f, 0.0f, 0.0f}, v3{0.0f, 1.0f, 0.0f});
    m4x4 proj = ref_perspective_m4x4(0.785398f, 1.0f, 0.1f, 100.0f);
    m4x4 wvp = proj * view;
    struct CameraDepthPerObject { m4x4 worldViewProjection; } uboBytes{};
    uboBytes.worldViewProjection = wvp;
    BufferDesc ubd{};
    ubd.size = sizeof(uboBytes);
    ubd.type = BufferType::Constant;
    ubd.memoryUsage = GPUMemoryUsage::Dynamic;
    ubd.name = "CameraDepthUBO";
    ResourceHandle ubo = fx.device->CreateBuffer(ubd);
    TEST_ASSERT(fx.device->UpdateBufferData(ubo, &uboBytes, sizeof(uboBytes), 0), "UBO");

    DescriptorSetLayoutBinding bind{};
    bind.binding = 0;
    bind.descriptorType = DescriptorType::UniformBuffer;
    bind.descriptorCount = 1;
    bind.stageFlags = ShaderStage::Vertex;
    DescriptorSetLayoutDesc dslDesc{};
    dslDesc.bindingCount = 1;
    dslDesc.bindings = &bind;
    DescriptorSetLayoutHandle dsl = fx.device->CreateDescriptorSetLayout(dslDesc);
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &dsl;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.device->CreatePipelineLayout(plDesc);
    DescriptorSetDesc dsDesc{}; dsDesc.layout = dsl;
    DescriptorSetHandle ds = fx.device->CreateDescriptorSet(dsDesc);
    DescriptorBufferInfo bi{};
    bi.buffer = ubo; bi.offset = 0; bi.range = sizeof(uboBytes);
    WriteDescriptorSet w{};
    w.dstSet = ds; w.dstBinding = 0; w.dstArrayElement = 0;
    w.descriptorCount = 1; w.descriptorType = DescriptorType::UniformBuffer;
    w.bufferInfo = &bi;
    fx.device->UpdateDescriptorSets(1, &w);

    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.cullMode = CullMode::Back;
    gpd.renderTargetCount = 0;   // depth-only
    gpd.depthStencilFormat = DataFormat::D16_UNorm;
    gpd.enableDepthTest = true;
    gpd.enableDepthWrite = true;
    gpd.depthFunc = ComparisonFunc::Less;
    PipelineHandle pipe = fx.device->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "depth-only pipeline (D16)");

    ResourceHandle rt = MakeRT(*fx.device, DataFormat::D16_UNorm, TextureUsage::DepthStencil);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture D16 RT");

    CommandBufferHandle cmd = fx.device->CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* mcmd = fx.device->GetCommandBuffer(cmd);
    RenderPassDesc rpd{};
    rpd.colorAttachments.clear();
    rpd.depthAttachment.texture = rt;
    rpd.depthAttachment.format = DataFormat::D16_UNorm;
    rpd.depthAttachment.loadOp = LoadAction::Clear;
    rpd.depthAttachment.storeOp = StoreAction::Store;
    rpd.depthAttachment.clearValue.depth = 1.0f;
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.viewport.minDepth = 0.0f;
    rpd.viewport.maxDepth = 1.0f;
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};
    TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "Reset/Begin");
    mcmd->BeginRenderPass(rpd);
    mcmd->BindGraphicsPipeline(pipe);
    mcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
    ResourceHandle vbArr[1] = {vb};
    u64 vbOffs[1] = {0};
    mcmd->BindVertexBuffers(20, 1, vbArr, vbOffs);
    mcmd->BindIndexBuffer(ib, DataFormat::R32_UInt, 0);
    mcmd->DrawIndexed(sphere.indexCount, 0, 0, 1, 0);
    mcmd->EndRenderPass();
    TEST_ASSERT(mcmd->End() && mcmd->Submit(0) && mcmd->WaitForCompletion(), "Submit");

    std::vector<u8> raw = ReadbackLayer(*fx.device, rt, 0, kW, kH, 2);   // D16 = 2B/px
    TEST_ASSERT(raw.size() == size_t(kW) * kH * 2, "D16 readback size");
    std::vector<u8> rgba(size_t(kW) * kH * 4);
    et::Depth16ToRGBA8(raw.data(), rgba.data(), kW, kH);
    u32 fg = 0;
    for (u32 i = 0; i < kW * kH; ++i) if (rgba[i * 4] < 255) ++fg;
    std::cout << "[TestMetalP4cRef] D16 depth fg pixels: " << fg << " / " << kW * kH << std::endl;
    TEST_ASSERT(fg > (kW * kH) / 4, "sphere covers > 1/4 of D16 depth frame");
    TEST_ASSERT(SaveRef("Assets/ReferenceImages/P4c-F1/depth16_sphere_metal.png", rgba, kW, kH),
                "SaveRef depth16");

    fx.device->DestroyCommandBuffer(cmd);
    fx.device->DestroyTexture(rt);
    fx.device->DestroyPipeline(pipe);
    fx.device->DestroyPipelineLayout(pl);
    fx.device->DestroyDescriptorSet(ds);
    fx.device->DestroyDescriptorSetLayout(dsl);
    fx.device->DestroyBuffer(ubo);
    fx.device->DestroyBuffer(ib);
    fx.device->DestroyBuffer(vb);
    fx.device->DestroyShader(fs);
    fx.device->DestroyShader(vs);
    return TestResult::Passed;
}

} // anonymous namespace

// ============================================================================
// F1: 12 个浮点家族格式的渐变参照(Vulkan 测试 FlipY 后比对 → MSL 镜像 v)
// ============================================================================
TestResult Generate_F1_FormatGradients() {
    struct FmtRef {
        const char* name;
        DataFormat fmt;      // RHI 格式(决定 MTL 像素格式)
        u32 channels;
        bool unorm16, snorm16, isFloat32;   // readback 转换类别
    };
    const FmtRef refs[] = {
        {"R16_UNorm",    DataFormat::R16_UNorm,   1, true,  false, false},
        {"R16_SNorm",    DataFormat::R16_SNorm,   1, false, true,  false},
        {"RG16_UNorm",   DataFormat::RG16_UNorm,  2, true,  false, false},
        {"RG16_SNorm",   DataFormat::RG16_SNorm,  2, false, true,  false},
        {"RGBA16_UNorm", DataFormat::RGBA16_UNorm,4, true,  false, false},
        {"RGBA16_SNorm", DataFormat::RGBA16_SNorm,4, false, true,  false},
        {"R32_UNorm",    DataFormat::R32_UNorm,   1, false, false, true},   // → R32Float
        {"R32_SNorm",    DataFormat::R32_SNorm,   1, false, false, true},   // → R32Float
        {"RG32_UNorm",   DataFormat::RG32_UNorm,  2, false, false, true},
        {"RG32_SNorm",   DataFormat::RG32_SNorm,  2, false, false, true},
        {"RGBA32_UNorm", DataFormat::RGBA32_UNorm,4, false, false, true},
        {"RGBA32_SNorm", DataFormat::RGBA32_SNorm,4, false, false, true},
    };

    MetalFixture fx;
    TEST_ASSERT(fx.Init(), "MetalDevice init");

    ShaderHandle vs = MakeShader(*fx.device, kGradientVS, ShaderStage::Vertex, "gradientVS");
    ShaderHandle fs = MakeShader(*fx.device, kGradientFS, ShaderStage::Pixel, "gradientFS");
    TEST_ASSERT(vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER, "shaders");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 0;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.device->CreatePipelineLayout(plDesc);

    u32 generated = 0;
    for (const FmtRef& r : refs) {
        GraphicsPipelineDesc gpd{};
        gpd.vertexShader = vs;
        gpd.pixelShader = fs;
        gpd.layout = pl;
        gpd.topology = PrimitiveTopology::TriangleList;
        gpd.cullMode = CullMode::None;
        gpd.renderTargetCount = 1;
        gpd.renderTargetFormats[0] = r.fmt;
        gpd.enableDepthTest = false;
        gpd.enableDepthWrite = false;
        PipelineHandle pipe = fx.device->CreateGraphicsPipeline(gpd);
        if (pipe == handles::INVALID_PIPELINE) {
            std::cerr << "[TestMetalP4cRef] pipeline failed for " << r.name << " — skip"
                      << std::endl;
            continue;
        }
        ResourceHandle rt = MakeRT(*fx.device, r.fmt, TextureUsage::RenderTarget);
        if (rt == handles::INVALID_RESOURCE) {
            fx.device->DestroyPipeline(pipe);
            continue;
        }

        CommandBufferHandle cmd = fx.device->CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* mcmd = fx.device->GetCommandBuffer(cmd);
        RenderPassDesc rpd{};
        rpd.colorAttachments.resize(1);
        rpd.colorAttachments[0].texture = rt;
        rpd.colorAttachments[0].format = r.fmt;
        rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
        rpd.colorAttachments[0].storeOp = StoreAction::Store;
        rpd.viewport.topLeft = {0.0f, 0.0f};
        rpd.viewport.size = {float(kW), float(kH)};
        rpd.scissor.offset = {0, 0};
        rpd.scissor.extent = {kW, kH};
        TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "Reset/Begin");
        mcmd->BeginRenderPass(rpd);
        mcmd->BindGraphicsPipeline(pipe);
        mcmd->Draw(3, 0, 1, 0);
        mcmd->EndRenderPass();
        TEST_ASSERT(mcmd->End() && mcmd->Submit(0) && mcmd->WaitForCompletion(), "Submit");

        const u64 bpt = (r.unorm16 || r.snorm16) ? 2 : 4;
        std::vector<u8> raw = ReadbackLayer(*fx.device, rt, 0, kW, kH, bpt * r.channels);
        fx.device->DestroyCommandBuffer(cmd);
        fx.device->DestroyTexture(rt);
        fx.device->DestroyPipeline(pipe);
        if (raw.empty()) continue;

        std::vector<u8> rgba(size_t(kW) * kH * 4, 255);
        for (u32 p = 0; p < kW * kH; ++p) {
            const u8* texel = raw.data() + size_t(p) * bpt * r.channels;
            for (u32 c = 0; c < r.channels; ++c) {
                rgba[p * 4 + c] = ChannelToByte(r.unorm16, r.snorm16, r.isFloat32,
                                                texel + c * bpt);
            }
        }
        char path[160];
        std::snprintf(path, sizeof(path), "Assets/ReferenceImages/P4c-F1/format_%s.png", r.name);
        TEST_ASSERT(SaveRef(path, rgba, kW, kH), "SaveRef format");
        ++generated;
    }

    fx.device->DestroyPipelineLayout(pl);
    fx.device->DestroyShader(fs);
    fx.device->DestroyShader(vs);
    std::cout << "[TestMetalP4cRef] F1 format references: " << generated << "/12" << std::endl;
    TEST_ASSERT(generated >= 10, "most format references must generate on this device");
    return TestResult::Passed;
}

// ============================================================================
// F3: 8x8 棋盘格 NEAREST 采样(Vulkan 测试不翻转 → 无镜像)
// ============================================================================
TestResult Generate_F3_Checkerboard() {
    MetalFixture fx;
    TEST_ASSERT(fx.Init(), "MetalDevice init");

    constexpr u32 kT = 8;
    std::vector<u8> checker(size_t(kT) * kT * 4);
    for (u32 y = 0; y < kT; ++y) {
        for (u32 x = 0; x < kT; ++x) {
            const bool white = ((x / 4) + (y / 4)) % 2 == 0;
            u8 v = white ? 255 : 0;
            u8* p = &checker[(y * kT + x) * 4];
            p[0] = v; p[1] = v; p[2] = v; p[3] = 255;
        }
    }

    // 纹理上传:走 Metal 的 ReplaceRegion/updateData 通道(对应 Vulkan F3 路径)
    TextureDesc td{};
    td.size = {kT, kT, 1};
    td.mipLevels = 1;
    td.arraySize = 1;
    td.format = DataFormat::RGBA8_UNorm;
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::ShaderResource | TextureUsage::CopySource;
    td.memoryUsage = GPUMemoryUsage::Static;
    td.name = "CheckerTex";
    ResourceHandle tex = fx.device->CreateTexture(td);
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture");
    MetalTexture* mtex = static_cast<MetalTexture*>(fx.device->GetTexture(tex));
    TEST_ASSERT(mtex->UpdateData(checker.data(), checker.size(), 0), "Metal UpdateData");
    // Metal 侧 updateData 走 staging 队列;无帧上下文时显式 flush 才落 GPU
    fx.device->GetStagingAllocator().FlushBlocking();

    ShaderHandle vs = MakeShader(*fx.device, kSampleVS, ShaderStage::Vertex, "sampleVS");
    ShaderHandle fs = MakeShader(*fx.device, kSampleFS, ShaderStage::Pixel, "sampleFS");
    TEST_ASSERT(vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER, "shaders");

    SamplerDesc sd{};
    sd.minFilter = FilterMode::Point;
    sd.magFilter = FilterMode::Point;
    sd.mipFilter = FilterMode::Point;
    sd.addressU = TextureAddressMode::Clamp;
    sd.addressV = TextureAddressMode::Clamp;
    sd.maxAnisotropy = 1;
    sd.comparisonFunc = ComparisonFunc::Never;
    SamplerHandle sampler = fx.device->CreateSampler(sd);

    DescriptorSetLayoutBinding bind{};
    bind.binding = 0;
    bind.descriptorType = DescriptorType::CombinedImageSampler;
    bind.descriptorCount = 1;
    bind.stageFlags = ShaderStage::Pixel;
    DescriptorSetLayoutDesc dslDesc{};
    dslDesc.bindingCount = 1;
    dslDesc.bindings = &bind;
    DescriptorSetLayoutHandle dsl = fx.device->CreateDescriptorSetLayout(dslDesc);
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &dsl;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.device->CreatePipelineLayout(plDesc);
    DescriptorSetDesc dsDesc{}; dsDesc.layout = dsl;
    DescriptorSetHandle ds = fx.device->CreateDescriptorSet(dsDesc);
    DescriptorImageInfo info{};
    info.sampler = sampler;
    info.imageView = tex;
    info.imageLayout = ResourceState::ShaderResource;
    WriteDescriptorSet w{};
    w.dstSet = ds; w.dstBinding = 0; w.dstArrayElement = 0;
    w.descriptorCount = 1; w.descriptorType = DescriptorType::CombinedImageSampler;
    w.imageInfo = &info;
    fx.device->UpdateDescriptorSets(1, &w);

    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = fx.device->CreateGraphicsPipeline(gpd);

    ResourceHandle rt = MakeRT(*fx.device, DataFormat::RGBA8_UNorm,
                               TextureUsage::RenderTarget);
    CommandBufferHandle cmd = fx.device->CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* mcmd = fx.device->GetCommandBuffer(cmd);
    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = rt;
    rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
    rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};
    TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "Reset/Begin");
    mcmd->BeginRenderPass(rpd);
    mcmd->BindGraphicsPipeline(pipe);
    mcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
    mcmd->Draw(3, 0, 1, 0);
    mcmd->EndRenderPass();
    TEST_ASSERT(mcmd->End() && mcmd->Submit(0) && mcmd->WaitForCompletion(), "Submit");

    std::vector<u8> raw = ReadbackLayer(*fx.device, rt, 0, kW, kH, 4);
    TEST_ASSERT(!raw.empty(), "readback");
    TEST_ASSERT(SaveRef("Assets/ReferenceImages/P4c-F3/checkerboard_metal.png", raw, kW, kH),
                "SaveRef checkerboard");

    fx.device->DestroyCommandBuffer(cmd);
    fx.device->DestroyTexture(rt);
    fx.device->DestroyPipeline(pipe);
    fx.device->DestroyPipelineLayout(pl);
    fx.device->DestroyDescriptorSet(ds);
    fx.device->DestroyDescriptorSetLayout(dsl);
    fx.device->DestroySampler(sampler);
    fx.device->DestroyShader(fs);
    fx.device->DestroyShader(vs);
    fx.device->DestroyTexture(tex);
    return TestResult::Passed;
}

// ============================================================================
// F4: 300 帧滚动终帧(与 TestVulkanStagingUpload 的 CPU 期望帧逐字节同源;
//     Metal 侧用 updateData 整图上传,验证 ReplaceRegion/staged 等价路径)
// ============================================================================
TestResult Generate_F4_RollingFinal() {
    MetalFixture fx;
    TEST_ASSERT(fx.Init(), "MetalDevice init");

    constexpr u32 kW4 = 128, kH4 = 128, kTile = 16;
    constexpr u32 kTilesX = kW4 / kTile, kTilesY = kH4 / kTile;
    constexpr u32 kFrames = 300;

    std::vector<u8> shadow(size_t(kW4) * kH4 * 4, 0);
    for (u32 i = 0; i < kW4 * kH4; ++i) shadow[i * 4 + 3] = 255;

    u32 seq = 0;
    for (u32 f = 0; f < kFrames; ++f) {
        const u32 updates = 1 + (f % 2);
        for (u32 u = 0; u < updates; ++u, ++seq) {
            const u32 tx = seq % kTilesX;
            const u32 ty = (seq / kTilesX) % (kTilesY - 1);
            const u8 r = static_cast<u8>((f * 7 + u * 31) & 0xFF);
            const u8 g = static_cast<u8>((f * 13 + u * 3) & 0xFF);
            const u8 b = static_cast<u8>((f * 29 + u * 11) & 0xFF);
            for (u32 y = 0; y < kTile; ++y) {
                for (u32 x = 0; x < kTile; ++x) {
                    u8* p = &shadow[((ty * kTile + y) * kW4 + (tx * kTile + x)) * 4];
                    p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
                }
            }
        }
    }

    TextureDesc td{};
    td.size = {kW4, kH4, 1};
    td.mipLevels = 1;
    td.arraySize = 1;
    td.format = DataFormat::RGBA8_UNorm;
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest | TextureUsage::CopySource;
    td.memoryUsage = GPUMemoryUsage::Static;
    td.name = "RollingTex";
    ResourceHandle tex = fx.device->CreateTexture(td);
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture");
    MetalTexture* mtex = static_cast<MetalTexture*>(fx.device->GetTexture(tex));
    TEST_ASSERT(mtex->UpdateData(shadow.data(), shadow.size(), 0), "Metal UpdateData full");
    fx.device->GetStagingAllocator().FlushBlocking();

    std::vector<u8> rb = ReadbackLayer(*fx.device, tex, 0, kW4, kH4, 4);
    TEST_ASSERT(!rb.empty(), "readback");
    const int maxDiff = et::MaxAbsDiff(rb.data(), shadow.data(), kW4, kH4);
    std::cout << "[TestMetalP4cRef] F4 roundtrip max-abs-diff = " << maxDiff << std::endl;
    TEST_ASSERT(maxDiff <= 2, "Metal updateData roundtrip within 2/255");

    TEST_ASSERT(SaveRef("Assets/ReferenceImages/P4c-F4/rolling_final_metal.png", rb, kW4, kH4),
                "SaveRef rolling final");
    fx.device->DestroyTexture(tex);
    return TestResult::Passed;
}

// ============================================================================
// F5: 4 层 layered 渐变(Vulkan 测试逐层 FlipY 后比对 → MSL 镜像 v)
// ============================================================================
TestResult Generate_F5_Layered() {
    MetalFixture fx;
    TEST_ASSERT(fx.Init(), "MetalDevice init");

    ShaderHandle vs = MakeShader(*fx.device, kLayeredVS, ShaderStage::Vertex, "layeredVS");
    ShaderHandle fs = MakeShader(*fx.device, kLayeredFS, ShaderStage::Pixel, "layeredFS");
    TEST_ASSERT(vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER, "shaders");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 0;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.device->CreatePipelineLayout(plDesc);

    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = fx.device->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "pipeline");

    ResourceHandle tex = MakeRT(*fx.device, DataFormat::RGBA8_UNorm, TextureUsage::RenderTarget,
                                kW, kH, 4, TextureType::Texture2DArray);
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture 4-layer array");

    CommandBufferHandle cmd = fx.device->CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* mcmd = fx.device->GetCommandBuffer(cmd);
    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = tex;
    rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
    rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.renderTargetArrayLength = 4;   // ← setRenderTargetArrayLength
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};
    TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "Reset/Begin");
    mcmd->BeginRenderPass(rpd);
    mcmd->BindGraphicsPipeline(pipe);
    mcmd->Draw(3, 0, 4, 0);   // 4 instances → 4 layers
    mcmd->EndRenderPass();
    TEST_ASSERT(mcmd->End() && mcmd->Submit(0) && mcmd->WaitForCompletion(), "Submit");

    for (u32 i = 0; i < 4; ++i) {
        std::vector<u8> raw = ReadbackLayer(*fx.device, tex, i, kW, kH, 4);
        TEST_ASSERT(!raw.empty(), "layer readback");
        char path[128];
        std::snprintf(path, sizeof(path), "Assets/ReferenceImages/P4c-F5/layer%u_metal.png", i);
        TEST_ASSERT(SaveRef(path, raw, kW, kH), "SaveRef layer");
    }

    fx.device->DestroyCommandBuffer(cmd);
    fx.device->DestroyTexture(tex);
    fx.device->DestroyPipeline(pipe);
    fx.device->DestroyPipelineLayout(pl);
    fx.device->DestroyShader(fs);
    fx.device->DestroyShader(vs);
    return TestResult::Passed;
}

// ============================================================================
// F6: 512B setBytes 驱动 compute 写 storage image(compute 无朝向问题)
// ============================================================================
TestResult Generate_F6_Compute512() {
    MetalFixture fx;
    TEST_ASSERT(fx.Init(), "MetalDevice init");

    ShaderHandle cs = MakeShader(*fx.device, kComputeMS, ShaderStage::Compute, "computeMain");
    TEST_ASSERT(cs != handles::INVALID_SHADER, "compute shader");

    // 与 Vulkan 测试 Case A 相同的探针值(v[0].x=0.25, v[8].y=0.50, v[31].z=0.75)
    struct BigBlock { float v[128]; } big{};
    big.v[0] = 0.25f;
    big.v[33] = 0.50f;
    big.v[126] = 0.75f;

    DescriptorSetLayoutBinding bind{};
    bind.binding = 0;
    bind.descriptorType = DescriptorType::StorageImage;
    bind.descriptorCount = 1;
    bind.stageFlags = ShaderStage::Compute;
    DescriptorSetLayoutDesc dslDesc{};
    dslDesc.bindingCount = 1;
    dslDesc.bindings = &bind;
    DescriptorSetLayoutHandle dsl = fx.device->CreateDescriptorSetLayout(dslDesc);
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &dsl;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.device->CreatePipelineLayout(plDesc);
    ComputePipelineDesc cpd{};
    cpd.computeShader = cs;
    cpd.layout = pl;
    cpd.threadGroupSize = {8, 8, 1};   // 与 MSL kernel 网格划分一致
    PipelineHandle pipe = fx.device->CreateComputePipeline(cpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "compute pipeline");

    ResourceHandle tex = MakeRT(*fx.device, DataFormat::RGBA8_UNorm, TextureUsage::UnorderedAccess);
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture storage");

    DescriptorSetDesc dsDesc{}; dsDesc.layout = dsl;
    DescriptorSetHandle ds = fx.device->CreateDescriptorSet(dsDesc);
    DescriptorImageInfo info{};
    info.imageView = tex;
    info.imageLayout = ResourceState::UnorderedAccess;
    WriteDescriptorSet w{};
    w.dstSet = ds; w.dstBinding = 0; w.dstArrayElement = 0;
    w.descriptorCount = 1; w.descriptorType = DescriptorType::StorageImage;
    w.imageInfo = &info;
    fx.device->UpdateDescriptorSets(1, &w);

    CommandBufferHandle cmd = fx.device->CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* mcmd = fx.device->GetCommandBuffer(cmd);
    TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "Reset/Begin");
    mcmd->BindComputePipeline(pipe);
    mcmd->BindDescriptorSets(PipelineBindPoint::Compute, pl, 0, 1, &ds, 0, nullptr);
    mcmd->SetComputeBytes(0, &big, sizeof(big));   // Metal setBytes 路径
    mcmd->Dispatch(kW / 8, kH / 8, 1);
    TEST_ASSERT(mcmd->End() && mcmd->Submit(0) && mcmd->WaitForCompletion(), "Submit");

    std::vector<u8> raw = ReadbackLayer(*fx.device, tex, 0, kW, kH, 4);
    TEST_ASSERT(!raw.empty(), "readback");
    TEST_ASSERT(SaveRef("Assets/ReferenceImages/P4c-F6/compute512_metal.png", raw, kW, kH),
                "SaveRef compute512");

    fx.device->DestroyCommandBuffer(cmd);
    fx.device->DestroyDescriptorSet(ds);
    fx.device->DestroyTexture(tex);
    fx.device->DestroyPipeline(pipe);
    fx.device->DestroyPipelineLayout(pl);
    fx.device->DestroyDescriptorSetLayout(dsl);
    fx.device->DestroyShader(cs);
    return TestResult::Passed;
}

// ============================================================================
// F7: 32 tile 经 parallel secondary 绘制(Vulkan 测试不翻转;tile 坐标同源)
// ============================================================================
TestResult Generate_F7_Tiles32() {
    MetalFixture fx;
    TEST_ASSERT(fx.Init(), "MetalDevice init");

    constexpr u32 kTilesX = 8, kTilesY = 4;
    const u32 tw = kW / kTilesX, th = kH / kTilesY;

    ShaderHandle vs = MakeShader(*fx.device, kTileVS, ShaderStage::Vertex, "tileVS");
    ShaderHandle fs = MakeShader(*fx.device, kTileFS, ShaderStage::Pixel, "tileFS");
    TEST_ASSERT(vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER, "shaders");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 0;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.device->CreatePipelineLayout(plDesc);

    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = fx.device->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "pipeline");

    // 32 个 24B 顶点缓冲(颜色烘焙):三角形覆盖 NDC 全屏,靠 scissor 裁到 tile
    std::vector<ResourceHandle> tileVBs(kTilesX * kTilesY);
    for (u32 t = 0; t < kTilesX * kTilesY; ++t) {
        const u8 r = static_cast<u8>((t * 37) & 0xFF);
        const u8 g = static_cast<u8>((t * 73) & 0xFF);
        const u8 b = static_cast<u8>((t * 151) & 0xFF);
        float verts[3][6] = {
            {-1.0f, -1.0f, r / 255.0f, g / 255.0f, b / 255.0f, 1.0f},
            { 3.0f, -1.0f, r / 255.0f, g / 255.0f, b / 255.0f, 1.0f},
            {-1.0f,  3.0f, r / 255.0f, g / 255.0f, b / 255.0f, 1.0f},
        };
        BufferDesc bd{};
        bd.size = sizeof(verts);
        bd.type = BufferType::Vertex;
        bd.vertex.vertexCount = 3;
        bd.vertex.vertexStride = 24;
        bd.memoryUsage = GPUMemoryUsage::Dynamic;
        bd.name = "TileVB";
        tileVBs[t] = fx.device->CreateBuffer(bd);
        TEST_ASSERT(fx.device->UpdateBufferData(tileVBs[t], verts, sizeof(verts), 0), "tile VB");
    }

    ResourceHandle rt = MakeRT(*fx.device, DataFormat::RGBA8_UNorm, TextureUsage::RenderTarget);

    CommandBufferHandle cmd = fx.device->CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* mcmd = fx.device->GetCommandBuffer(cmd);
    TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "primary Begin");

    // Metal 并行语义:先开 parallel pass,secondary 挂其上
    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = rt;
    rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
    rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};
    mcmd->BeginParallelRenderPass(rpd);

    // 4 个 secondary,各画 8 个 tile(镜像 Vulkan 测试的分组)
    constexpr u32 kSecondaries = 4;
    constexpr u32 kTilesPer = (kTilesX * kTilesY) / kSecondaries;
    SecondaryCommandBufferDesc sd{};   // Metal:parallel 子 encoder 自动继承
    CommandBufferHandle secs[kSecondaries];
    for (u32 s = 0; s < kSecondaries; ++s) {
        secs[s] = mcmd->BeginSecondaryCommandBuffer(sd);
        TEST_ASSERT(secs[s] != handles::INVALID_COMMAND_BUFFER, "BeginSecondary (Metal parallel)");
        MetalCommandBuffer* sec = fx.device->GetCommandBuffer(secs[s]);
        sec->BindGraphicsPipeline(pipe);
        ViewportDesc vp{};
        vp.topLeft = {0.0f, 0.0f};
        vp.size = {float(kW), float(kH)};
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        sec->SetViewport(vp);
        for (u32 t = s * kTilesPer; t < (s + 1) * kTilesPer; ++t) {
            const u32 tx = t % kTilesX, ty = t / kTilesX;
            primal::graphics::rhi::Rect sc;
            sc.offset = {int32_t(tx * tw), int32_t(ty * th)};
            sc.extent = {tw, th};
            sec->SetScissor(sc);
            ResourceHandle vbArr[1] = {tileVBs[t]};
            u64 offs[1] = {0};
            sec->BindVertexBuffers(20, 1, vbArr, offs);
            sec->Draw(3, 0, 1, 0);
        }
        TEST_ASSERT(sec->End(), "secondary End");
    }

    mcmd->ExecuteSecondaryCommandBuffers(kSecondaries, secs);
    mcmd->EndRenderPass();
    TEST_ASSERT(mcmd->End() && mcmd->Submit(0) && mcmd->WaitForCompletion(), "primary Submit");

    std::vector<u8> raw = ReadbackLayer(*fx.device, rt, 0, kW, kH, 4);
    TEST_ASSERT(!raw.empty(), "readback");
    // 抽样验证 tile 颜色(Metal 自检)
    u32 bad = 0;
    for (u32 t = 0; t < kTilesX * kTilesY; ++t) {
        const u32 tx = t % kTilesX, ty = t / kTilesX;
        const u8* p = &raw[((ty * th + th / 2) * kW + tx * tw + tw / 2) * 4];
        if (std::abs(int(p[0]) - int((t * 37) & 0xFF)) > 2) ++bad;
    }
    std::cout << "[TestMetalP4cRef] F7 tile self-check mismatches: " << bad << std::endl;
    TEST_ASSERT(SaveRef("Assets/ReferenceImages/P4c-F7/tiles32_metal.png", raw, kW, kH),
                "SaveRef tiles32");
    TEST_ASSERT(bad == 0, "Metal parallel secondary tiles correct");

    for (u32 s = 0; s < kSecondaries; ++s) fx.device->DestroyCommandBuffer(secs[s]);
    fx.device->DestroyCommandBuffer(cmd);
    fx.device->DestroyTexture(rt);
    for (auto h : tileVBs) fx.device->DestroyBuffer(h);
    fx.device->DestroyPipeline(pipe);
    fx.device->DestroyPipelineLayout(pl);
    fx.device->DestroyShader(fs);
    fx.device->DestroyShader(vs);
    return TestResult::Passed;
}

void RegisterMetalP4cReferenceTests() {
    auto suite = std::make_shared<TestSuite>("MetalP4cReferenceTests");
    suite->AddTestCase(TestCase("F3_Checkerboard",     Generate_F3_Checkerboard));
    suite->AddTestCase(TestCase("F1_FormatGradients",  Generate_F1_FormatGradients));
    suite->AddTestCase(TestCase("F1_Depth16",          Generate_F1_Depth16));
    suite->AddTestCase(TestCase("F4_RollingFinal",     Generate_F4_RollingFinal));
    suite->AddTestCase(TestCase("F5_Layered",          Generate_F5_Layered));
    suite->AddTestCase(TestCase("F6_Compute512",       Generate_F6_Compute512));
    suite->AddTestCase(TestCase("F7_Tiles32",          Generate_F7_Tiles32));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterMetalP4cReferenceTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // !__APPLE__

#include <iostream>

int main() {
    std::cout << "[TestMetalP4cReferences] Not Apple platform — no-op." << std::endl;
    return 0;
}

#endif
