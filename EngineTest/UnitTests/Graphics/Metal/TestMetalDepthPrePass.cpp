/**
 * @file TestMetalDepthPrePass.cpp
 * @brief Phase 4b Tier 3.1 — Metal reference generator for DepthPrePass parity.
 * @details Mirrors TestVulkanDepthPrePass.cpp on the Metal backend. Renders a
 *          sphere to a D32_FLOAT depth-only render target via embedded MSL
 *          (camera-depth vertex shader + noop fragment). Reads back the depth
 *          buffer, converts to RGBA8 grayscale, and saves the PNG as
 *          Assets/ReferenceImages/P4b-T3/depth_sphere_metal.png.
 *
 *          This PNG is the parity reference for TestVulkanDepthPrePass's SSIM
 *          check. Same MVP as Vulkan (eye=(0,0,3), 45° FOV, near=0.1/far=100),
 *          same sphere (radius=1, 24 segments × 12 rings, 32B/vertex stride).
 *
 *          Metal framebuffer Y is top-to-bottom by default (matches OpenGL
 *          convention); the Vulkan test calls FlipYInPlace after readback to
 *          match this. So Metal readback needs no Y flip.
 *
 * Metal binding convention:
 *   - Vertex array:        [[buffer(20)]]  (avoids collision with UBO at slot 0)
 *   - CameraDepthPerObject: [[buffer(0)]]
 */

#ifdef __APPLE__

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"

#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"

#include <iostream>
#include <vector>
#include <cstring>
#include <cmath>
#include <filesystem>

using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;
namespace et = EngineTest;

namespace {

// ============================= Embedded MSL shader source =============================
static const char* kVertexShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct CameraDepthPerObject {
    float4x4 worldViewProjection;
};

struct VertexIn {
    packed_float3 position;   // 12B
    packed_float3 normalPad;  // 12B (matches Vulkan float-normal slot)
    float2         uvPad;     //  8B
};

vertex float4 vertexMain(
    uint vid [[vertex_id]],
    constant CameraDepthPerObject& obj [[buffer(0)]],
    device const VertexIn* vertices [[buffer(20)]]
) {
    float3 p = float3(vertices[vid].position);
    return obj.worldViewProjection * float4(p, 1.0);
}
)MSL";

static const char* kFragmentShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

fragment float4 fragmentMain() {
    return float4(0.0);
}
)MSL";

// ============================= Math helpers (column-major, matches math::m4x4) =============================
m4x4 make_identity_m4x4() {
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = 1.0f;
    r.columns[1][1] = 1.0f;
    r.columns[2][2] = 1.0f;
    r.columns[3][3] = 1.0f;
    return r;
}

m4x4 make_view_m4x4(v3 eye, v3 target, v3 up) {
    v3 diff = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
    float zl = std::sqrt(diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2]);
    if (zl < 1e-8f) zl = 1.0f;
    v3 zaxis = {diff[0]/zl, diff[1]/zl, diff[2]/zl};
    v3 x = {
        up[1]*zaxis[2] - up[2]*zaxis[1],
        up[2]*zaxis[0] - up[0]*zaxis[2],
        up[0]*zaxis[1] - up[1]*zaxis[0],
    };
    float xl = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
    if (xl < 1e-8f) xl = 1.0f;
    v3 xaxis = {x[0]/xl, x[1]/xl, x[2]/xl};
    v3 yaxis = {
        zaxis[1]*xaxis[2] - zaxis[2]*xaxis[1],
        zaxis[2]*xaxis[0] - zaxis[0]*xaxis[2],
        zaxis[0]*xaxis[1] - zaxis[1]*xaxis[0],
    };
    m4x4 m = make_identity_m4x4();
    m.columns[0][0] = xaxis[0]; m.columns[0][1] = yaxis[0]; m.columns[0][2] = zaxis[0];
    m.columns[1][0] = xaxis[1]; m.columns[1][1] = yaxis[1]; m.columns[1][2] = zaxis[1];
    m.columns[2][0] = xaxis[2]; m.columns[2][1] = yaxis[2]; m.columns[2][2] = zaxis[2];
    m.columns[3][0] = -(xaxis[0]*eye[0] + xaxis[1]*eye[1] + xaxis[2]*eye[2]);
    m.columns[3][1] = -(yaxis[0]*eye[0] + yaxis[1]*eye[1] + yaxis[2]*eye[2]);
    m.columns[3][2] = -(zaxis[0]*eye[0] + zaxis[1]*eye[1] + zaxis[2]*eye[2]);
    m.columns[3][3] = 1.0f;
    return m;
}

m4x4 make_perspective_m4x4(float fov_y, float aspect, float zNear, float zFar) {
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

m4x4 multiply_m4x4(const m4x4& a, const m4x4& b) { return a * b; }

// ============================= Sphere geometry (32B/vertex, byte-identical to Vulkan) =============================
constexpr u32 kVertexStride = 32;

struct SphereMesh {
    std::vector<u8> vertices;
    std::vector<u32> indices;
    u32 vertexCount{0};
    u32 indexCount{0};
};

SphereMesh make_sphere(float radius, u32 segments, u32 rings) {
    SphereMesh s;
    s.vertexCount = (rings + 1) * (segments + 1);
    s.indexCount = rings * segments * 6;
    s.vertices.resize(size_t(s.vertexCount) * kVertexStride);
    s.indices.resize(s.indexCount);

    auto write_v = [&](u32 i, float px, float py, float pz,
                       float nx, float ny, float nz, float u, float v) {
        u8* p = s.vertices.data() + size_t(i) * kVertexStride;
        std::memcpy(p + 0,  &px, 4);
        std::memcpy(p + 4,  &py, 4);
        std::memcpy(p + 8,  &pz, 4);
        std::memcpy(p + 12, &nx, 4);
        std::memcpy(p + 16, &ny, 4);
        std::memcpy(p + 20, &nz, 4);
        std::memcpy(p + 24, &u,  4);
        std::memcpy(p + 28, &v,  4);
    };

    u32 vi = 0;
    for (u32 r = 0; r <= rings; ++r) {
        float phi = 3.14159265358979f * float(r) / float(rings);
        float sinP = std::sin(phi), cosP = std::cos(phi);
        for (u32 seg = 0; seg <= segments; ++seg) {
            float theta = 2.0f * 3.14159265358979f * float(seg) / float(segments);
            float sinT = std::sin(theta), cosT = std::cos(theta);
            float nx = sinP * cosT;
            float ny = cosP;
            float nz = sinP * sinT;
            write_v(vi,
                    nx * radius, ny * radius, nz * radius,
                    nx, ny, nz,
                    float(seg) / float(segments), float(r) / float(rings));
            ++vi;
        }
    }

    u32 ii = 0;
    for (u32 r = 0; r < rings; ++r) {
        for (u32 seg = 0; seg < segments; ++seg) {
            u32 a = r * (segments + 1) + seg;
            u32 b = a + segments + 1;
            s.indices[ii++] = a;
            s.indices[ii++] = b;
            s.indices[ii++] = a + 1;
            s.indices[ii++] = a + 1;
            s.indices[ii++] = b;
            s.indices[ii++] = b + 1;
        }
    }
    return s;
}

constexpr u32 kW = 64;
constexpr u32 kH = 64;
constexpr float kSphereRadius = 1.0f;
constexpr u32 kSegments = 24;
constexpr u32 kRings = 12;
constexpr u32 kVertexBufferSlot = 20;

} // anonymous namespace

// ============================= Main test =============================
TestResult TestDepthPrePass_RenderSphere_Metal() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        std::cerr << "[TestMetalDepthPrePass] MetalDevice::Initialize failed" << std::endl;
        return TestResult::Failed;
    }

    // ----- 1. Shaders (embedded MSL) -----
    ShaderHandle vs = device.CreateShader(
        kVertexShaderSource, std::strlen(kVertexShaderSource),
        ShaderStage::Vertex, "vertexMain");
    TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader vertex");
    ShaderHandle fs = device.CreateShader(
        kFragmentShaderSource, std::strlen(kFragmentShaderSource),
        ShaderStage::Pixel, "fragmentMain");
    TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader fragment");

    // ----- 2. Sphere geometry (byte-identical to Vulkan test) -----
    SphereMesh sphere = make_sphere(kSphereRadius, kSegments, kRings);

    BufferDesc vbufDesc{};
    vbufDesc.size = sphere.vertices.size();
    vbufDesc.type = BufferType::Vertex;
    vbufDesc.vertex.vertexCount = sphere.vertexCount;
    vbufDesc.vertex.vertexStride = kVertexStride;
    vbufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    vbufDesc.name = "SphereVB";
    ResourceHandle vb = device.CreateBuffer(vbufDesc);
    TEST_ASSERT(vb != handles::INVALID_RESOURCE, "CreateBuffer vertex");
    TEST_ASSERT(device.UpdateBufferData(vb, sphere.vertices.data(), sphere.vertices.size(), 0),
                "UpdateBufferData vertex");

    BufferDesc ibufDesc{};
    ibufDesc.size = sphere.indices.size() * sizeof(u32);
    ibufDesc.type = BufferType::Index;
    ibufDesc.index.indexCount = sphere.indexCount;
    ibufDesc.index.format = DataFormat::R32_UInt;
    ibufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ibufDesc.name = "SphereIB";
    ResourceHandle ib = device.CreateBuffer(ibufDesc);
    TEST_ASSERT(ib != handles::INVALID_RESOURCE, "CreateBuffer index");
    TEST_ASSERT(device.UpdateBufferData(ib, sphere.indices.data(), ibufDesc.size, 0),
                "UpdateBufferData index");

    // ----- 3. UBO (CameraDepthPerObject: m4x4 worldViewProjection @ binding 0) -----
    // Same MVP as Vulkan test: eye (0,0,3), look at origin, 45° FOV.
    m4x4 view = make_view_m4x4(v3{0.0f, 0.0f, 3.0f},
                               v3{0.0f, 0.0f, 0.0f},
                               v3{0.0f, 1.0f, 0.0f});
    m4x4 proj = make_perspective_m4x4(0.785398f, 1.0f, 0.1f, 100.0f);
    m4x4 wvp = multiply_m4x4(proj, view);  // world = identity

    struct CameraDepthPerObject { m4x4 worldViewProjection; } uboBytes;
    uboBytes.worldViewProjection = wvp;
    constexpr u32 kUBOSize = sizeof(CameraDepthPerObject);  // 64

    BufferDesc ubufDesc{};
    ubufDesc.size = kUBOSize;
    ubufDesc.type = BufferType::Constant;
    ubufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ubufDesc.name = "CameraDepthUBO";
    ResourceHandle ubo = device.CreateBuffer(ubufDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer CameraDepthUBO");
    TEST_ASSERT(device.UpdateBufferData(ubo, &uboBytes, kUBOSize, 0),
                "UpdateBufferData CameraDepthUBO");

    // ----- 4. Descriptor set (binding 0) -----
    DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = DescriptorType::UniformBuffer;
    binding.descriptorCount = 1;
    binding.stageFlags = ShaderStage::Vertex;
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 1;
    layoutDesc.bindings = &binding;
    DescriptorSetLayoutHandle layout = device.CreateDescriptorSetLayout(layoutDesc);
    TEST_ASSERT(layout != handles::INVALID_RESOURCE, "CreateDescriptorSetLayout");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = device.CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = device.CreateDescriptorSet(dsDesc);
    TEST_ASSERT(ds != handles::INVALID_RESOURCE, "CreateDescriptorSet");

    DescriptorBufferInfo uboInfo{};
    uboInfo.buffer = ubo;
    uboInfo.offset = 0;
    uboInfo.range = kUBOSize;
    WriteDescriptorSet write{};
    write.dstSet = ds;
    write.dstBinding = 0;
    write.dstArrayElement = 0;
    write.descriptorCount = 1;
    write.descriptorType = DescriptorType::UniformBuffer;
    write.bufferInfo = &uboInfo;
    device.UpdateDescriptorSets(1, &write);

    // ----- 5. Pipeline (depth-only: renderTargetCount=0, D32 depth) -----
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    // Metal vertex fetch is manual via [[buffer(20)]]; no vertex descriptor needed.
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::Back;
    gpd.renderTargetCount = 0;  // depth-only
    gpd.depthStencilFormat = DataFormat::D32_Float;
    gpd.enableDepthTest = true;
    gpd.enableDepthWrite = true;
    gpd.depthFunc = ComparisonFunc::Less;
    PipelineHandle pipe = device.CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline (depth-only)");

    // ----- 6. Depth RT + readback buffer -----
    TextureDesc rtDesc{};
    rtDesc.size = { kW, kH, 1 };
    rtDesc.mipLevels = 1;
    rtDesc.arraySize = 1;
    rtDesc.format = DataFormat::D32_Float;
    rtDesc.type = TextureType::Texture2D;
    rtDesc.usage = TextureUsage::DepthStencil | TextureUsage::CopySource;
    rtDesc.memoryUsage = GPUMemoryUsage::Static;
    rtDesc.name = "DepthPrePass_RT";
    ResourceHandle rt = device.CreateTexture(rtDesc);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture D32 RT");

    BufferDesc readbackDesc{};
    readbackDesc.size = u64(kW) * kH * 4;  // D32 = 4 bytes/pixel
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "DepthPrePass_Readback";
    ResourceHandle readback = device.CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    // ----- 7. Render -----
    RenderPassDesc rpd{};
    rpd.colorAttachments.clear();  // depth-only
    rpd.depthAttachment.texture = rt;
    rpd.depthAttachment.format = DataFormat::D32_Float;
    rpd.depthAttachment.loadOp = LoadAction::Clear;
    rpd.depthAttachment.storeOp = StoreAction::Store;
    rpd.depthAttachment.clearValue.depth = 1.0f;
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.viewport.minDepth = 0.0f;
    rpd.viewport.maxDepth = 1.0f;
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};

    CommandBufferHandle cmd = device.CreateCommandBuffer(CommandQueueType::Graphics);
    TEST_ASSERT(cmd != handles::INVALID_COMMAND_BUFFER, "CreateCommandBuffer");
    MetalCommandBuffer* mcmd = device.GetCommandBuffer(cmd);
    TEST_ASSERT(mcmd != nullptr, "GetCommandBuffer");
    TEST_ASSERT(mcmd->Reset(), "Reset");
    TEST_ASSERT(mcmd->Begin(), "Begin");
    mcmd->BeginRenderPass(rpd);
    mcmd->BindGraphicsPipeline(pipe);
    mcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
    ResourceHandle vbArr[1] = { vb };
    u64 vbOffsets[1] = { 0 };
    mcmd->BindVertexBuffers(kVertexBufferSlot, 1, vbArr, vbOffsets);
    mcmd->BindIndexBuffer(ib, DataFormat::R32_UInt, 0);
    mcmd->DrawIndexed(sphere.indexCount, 0, 0, 1, 0);
    mcmd->EndRenderPass();

    // Copy depth RT → readback
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kW, kH, 1};
    mcmd->CopyTextureToBuffer(rt, readback, &region, 1);

    TEST_ASSERT(mcmd->End(), "End");
    TEST_ASSERT(mcmd->Submit(0), "Submit");
    TEST_ASSERT(mcmd->WaitForCompletion(), "WaitForCompletion");

    // ----- 8. Readback + convert to RGBA8 grayscale -----
    // Metal framebuffer Y is already top-to-bottom (OpenGL convention). NO Y flip.
    // The Vulkan test does FlipYInPlace after readback to match this orientation.
    void* mapped = device.MapBuffer(readback, 0, readbackDesc.size);
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");

    std::vector<u8> rgba8(size_t(kW) * kH * 4);
    et::Depth32ToRGBA8(static_cast<const u8*>(mapped), rgba8.data(), kW, kH);
    device.UnmapBuffer(readback);

    // Stats: count foreground pixels (depth < 1.0), center depth.
    u32 fg = 0;
    double depthSum = 0.0;
    u32 cx = kW / 2, cy = kH / 2;
    u32 centerIdx = (cy * kW + cx) * 4;
    u8 centerByte = rgba8[centerIdx];
    float centerDepth = centerByte / 255.0f;
    for (u32 i = 0; i < kW * kH; ++i) {
        u8 d = rgba8[i * 4];
        if (d < 255) {
            ++fg;
            depthSum += d / 255.0;
        }
    }
    double meanDepth = fg ? depthSum / fg : 0.0;
    std::cout << "[TestMetalDepthPrePass] foreground pixels (depth<1): "
              << fg << " / " << (kW * kH) << std::endl;
    std::cout << "[TestMetalDepthPrePass] mean depth (fg): " << meanDepth << std::endl;
    std::cout << "[TestMetalDepthPrePass] center pixel depth byte = "
              << int(centerByte) << " (" << centerDepth << ")" << std::endl;

    // Sanity: sphere should be visible (>25% foreground, <90% foreground).
    TEST_ASSERT(fg > (kW * kH) / 4, "Sphere should cover > 1/4 of depth frame");
    TEST_ASSERT(fg < (kW * kH * 9) / 10, "Sphere should not fill depth frame");
    // Center pixel is front of sphere (closest to camera). With eye=(0,0,3),
    // sphere radius 1 at origin → front at view-z=-2; near=0.1/far=100
    // OpenGL projection → depth ≈ 0.95.
    TEST_ASSERT(centerByte < 250, "Center pixel depth < 250 (not background)");
    TEST_ASSERT(centerByte > 128, "Center pixel depth > 128 (in front of camera)");

    // ----- 9. Save reference PNG -----
    const char* refPath = "Assets/ReferenceImages/P4b-T3/depth_sphere_metal.png";
    std::filesystem::create_directories("Assets/ReferenceImages/P4b-T3");
    if (et::SavePNG(refPath, rgba8.data(), kW, kH)) {
        std::cout << "[TestMetalDepthPrePass] saved " << refPath << std::endl;
    } else {
        std::cerr << "[TestMetalDepthPrePass] SavePNG failed for " << refPath << std::endl;
        return TestResult::Failed;
    }

    // Cleanup
    device.DestroyCommandBuffer(cmd);
    device.DestroyBuffer(readback);
    device.DestroyTexture(rt);
    device.DestroyPipeline(pipe);
    device.DestroyPipelineLayout(pl);
    device.DestroyDescriptorSet(ds);
    device.DestroyDescriptorSetLayout(layout);
    device.DestroyBuffer(ubo);
    device.DestroyBuffer(ib);
    device.DestroyBuffer(vb);
    device.DestroyShader(fs);
    device.DestroyShader(vs);
    device.Shutdown();
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("MetalDepthPrePassTests");
    suite->AddTestCase(TestCase("RenderSphere", TestDepthPrePass_RenderSphere_Metal));
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    return 0;
}

#else // !__APPLE__

#include <iostream>

int main() {
    std::cout << "[TestMetalDepthPrePass] Not Apple platform — no-op." << std::endl;
    return 0;
}

#endif
