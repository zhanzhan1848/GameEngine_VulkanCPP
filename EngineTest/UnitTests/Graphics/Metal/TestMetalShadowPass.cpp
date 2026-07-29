/**
 * @file TestMetalShadowPass.cpp
 * @brief Phase 4b Tier 3.2 — Metal reference generator for ShadowPass parity.
 * @details Mirrors TestVulkanShadowPass.cpp on the Metal backend. Renders a
 *          sphere into a 4-layer D32_FLOAT 2D-array depth texture from a
 *          directional light at (5,5,5), using orthographic projection
 *          (-2,2,-2,2,0.1,20). All 4 cascade layers use the same light VP
 *          (test simplification — engine truth uses 4 different splits).
 *
 *          Saves 4 PNGs (one per cascade layer) to:
 *            Assets/ReferenceImages/P4b-T3/shadow_sphere_metal_layer{0..3}.png
 *
 *          These are the parity references for TestVulkanShadowPass's per-layer
 *          SSIM check.
 *
 * Projection matrix convention:
 *   - Metal uses OpenGL NDC.z convention [-1,1] mapped to depth [0,1].
 *     So Metal-style (OpenGL) ortho produces clip.z in [-1,1] for visible
 *     vertices and Metal's viewport maps them to depth [0,1].
 *   - Vulkan uses NDC.z convention [0,1] directly. Vulkan test uses
 *     Vulkan-style ortho to produce clip.z in [0,1] directly.
 *   - Both conventions produce the SAME depth buffer value for the same
 *     geometry: depth = (d-zNear)/(zFar-zNear) where d is vertex-eye distance.
 *     So SSIM >= 0.95 holds despite different matrix coefficients.
 *
 * Metal binding convention:
 *   - Vertex array:        [[buffer(20)]]  (avoids collision with UBO at slot 0)
 *   - ShadowPerObject UBO: [[buffer(0)]]
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

struct ShadowPerObject {
    float4x4 world;
    float4x4 worldLightVP;
};

struct VertexIn {
    packed_float3 position;   // 12B
    packed_float3 normalPad;  // 12B
    float2         uvPad;     //  8B
};

vertex float4 vertexMain(
    uint vid [[vertex_id]],
    constant ShadowPerObject& obj [[buffer(0)]],
    device const VertexIn* vertices [[buffer(20)]]
) {
    float3 p = float3(vertices[vid].position);
    return obj.worldLightVP * float4(p, 1.0);
}
)MSL";

static const char* kFragmentShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

fragment float4 fragmentMain() {
    return float4(0.0);
}
)MSL";

// ============================= Math helpers =============================
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
    float ex = eye[0],    ey = eye[1],    ez = eye[2];
    float tx = target[0], ty = target[1], tz = target[2];
    float ux = up[0],     uy = up[1],     uz = up[2];

    float dx = ex - tx, dy = ey - ty, dz = ez - tz;
    float zl = std::sqrt(dx*dx + dy*dy + dz*dz);
    if (zl < 1e-8f) zl = 1.0f;
    float zx = dx/zl, zy = dy/zl, zz = dz/zl;

    float xx0 = uy*zz - uz*zy;
    float xy0 = uz*zx - ux*zz;
    float xz0 = ux*zy - uy*zx;
    float xl = std::sqrt(xx0*xx0 + xy0*xy0 + xz0*xz0);
    if (xl < 1e-8f) xl = 1.0f;
    float axx = xx0/xl, axy = xy0/xl, axz = xz0/xl;

    float ayx = zy*axz - zz*axy;
    float ayy = zz*axx - zx*axz;
    float ayz = zx*axy - zy*axx;

    m4x4 m = make_identity_m4x4();
    m.columns[0][0] = axx; m.columns[0][1] = ayx; m.columns[0][2] = zx;
    m.columns[1][0] = axy; m.columns[1][1] = ayy; m.columns[1][2] = zy;
    m.columns[2][0] = axz; m.columns[2][1] = ayz; m.columns[2][2] = zz;
    m.columns[3][0] = -(axx*ex + axy*ey + axz*ez);
    m.columns[3][1] = -(ayx*ex + ayy*ey + ayz*ez);
    m.columns[3][2] = -(zx*ex + zy*ey + zz*ez);
    m.columns[3][3] = 1.0f;
    return m;
}

// Metal clip space uses NDC.z in [0,1] (like Vulkan, NOT [-1,1] like OpenGL).
// So we use the same Vulkan-style ortho coefficients as the Vulkan test:
//   P[2][2] = -1/(zFar-zNear), P[3][2] = -zNear/(zFar-zNear).
// Both backends then produce identical depth values for the same geometry:
//   depth = (d-zNear)/(zFar-zNear) where d is vertex-eye distance.
m4x4 make_ortho_m4x4(float left, float right, float bottom, float top,
                     float zNear, float zFar) {
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = 2.0f / (right - left);
    r.columns[1][1] = 2.0f / (top - bottom);
    r.columns[2][2] = -1.0f / (zFar - zNear);
    r.columns[3][0] = -(right + left) / (right - left);
    r.columns[3][1] = -(top + bottom) / (top - bottom);
    r.columns[3][2] = -zNear / (zFar - zNear);
    r.columns[3][3] = 1.0f;
    return r;
}

m4x4 multiply_m4x4(const m4x4& a, const m4x4& b) { return a * b; }

// ============================= Sphere geometry (byte-identical to Vulkan) =============================
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
constexpr u32 kCascadeCount = 4;
constexpr float kSphereRadius = 1.0f;
constexpr u32 kSegments = 24;
constexpr u32 kRings = 12;
constexpr u32 kVertexBufferSlot = 20;

} // anonymous namespace

// ============================= Main test =============================
TestResult TestShadowPass_RenderSphere_Metal() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        std::cerr << "[TestMetalShadowPass] MetalDevice::Initialize failed" << std::endl;
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

    // ----- 2. Sphere geometry -----
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

    // ----- 3. UBO (ShadowPerObject: m4x4 world @ 0, m4x4 worldLightVP @ 64) -----
    m4x4 world = make_identity_m4x4();
    m4x4 lightView = make_view_m4x4(v3{5.0f, 5.0f, 5.0f},
                                    v3{0.0f, 0.0f, 0.0f},
                                    v3{0.0f, 1.0f, 0.0f});
    m4x4 lightProj = make_ortho_m4x4(-2.0f, 2.0f, -2.0f, 2.0f, 0.1f, 20.0f);
    m4x4 lightVP = multiply_m4x4(lightProj, lightView);

    struct ShadowPerObject {
        m4x4 world;
        m4x4 worldLightVP;
    } uboBytes;
    uboBytes.world = world;
    uboBytes.worldLightVP = lightVP;
    constexpr u32 kUBOSize = sizeof(ShadowPerObject);  // 128

    BufferDesc ubufDesc{};
    ubufDesc.size = kUBOSize;
    ubufDesc.type = BufferType::Constant;
    ubufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ubufDesc.name = "ShadowUBO";
    ResourceHandle ubo = device.CreateBuffer(ubufDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer ShadowUBO");
    TEST_ASSERT(device.UpdateBufferData(ubo, &uboBytes, kUBOSize, 0),
                "UpdateBufferData ShadowUBO");

    // ----- 4. Descriptor set -----
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

    // ----- 5. Pipeline (depth-only) -----
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::None;  // match Vulkan test (debug: rule out winding)
    gpd.renderTargetCount = 0;
    gpd.depthStencilFormat = DataFormat::D32_Float;
    gpd.enableDepthTest = true;
    gpd.enableDepthWrite = true;
    gpd.depthFunc = ComparisonFunc::Less;
    PipelineHandle pipe = device.CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline (shadow)");

    // ----- 6. D32 2D-array RT (4 cascades) + readback buffer -----
    TextureDesc rtDesc{};
    rtDesc.size = { kW, kH, 1 };
    rtDesc.mipLevels = 1;
    rtDesc.arraySize = kCascadeCount;
    rtDesc.format = DataFormat::D32_Float;
    rtDesc.type = TextureType::Texture2DArray;
    rtDesc.usage = TextureUsage::DepthStencil | TextureUsage::CopySource;
    rtDesc.memoryUsage = GPUMemoryUsage::Static;
    rtDesc.name = "ShadowPass_RT_Array";
    ResourceHandle rt = device.CreateTexture(rtDesc);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture D32 array RT");

    BufferDesc readbackDesc{};
    readbackDesc.size = u64(kW) * kH * 4;  // D32 = 4 bytes/pixel
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "ShadowPass_Readback";
    ResourceHandle readback = device.CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    // ----- 7. Render 4 cascades (same VP) -----
    CommandBufferHandle cmd = device.CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* mcmd = device.GetCommandBuffer(cmd);
    TEST_ASSERT(mcmd->Reset(), "Reset");
    TEST_ASSERT(mcmd->Begin(), "Begin");

    for (u32 c = 0; c < kCascadeCount; ++c) {
        RenderPassDesc rpd{};
        rpd.colorAttachments.clear();
        rpd.depthAttachment.texture = rt;
        rpd.depthAttachment.format = DataFormat::D32_Float;
        rpd.depthAttachment.arrayLayer = static_cast<u16>(c);
        rpd.depthAttachment.loadOp = LoadAction::Clear;
        rpd.depthAttachment.storeOp = StoreAction::Store;
        rpd.depthAttachment.clearValue.depth = 1.0f;
        rpd.viewport.topLeft = {0.0f, 0.0f};
        rpd.viewport.size = {float(kW), float(kH)};
        rpd.viewport.minDepth = 0.0f;
        rpd.viewport.maxDepth = 1.0f;
        rpd.scissor.offset = {0, 0};
        rpd.scissor.extent = {kW, kH};

        mcmd->BeginRenderPass(rpd);
        mcmd->BindGraphicsPipeline(pipe);
        mcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
        ResourceHandle vbArr[1] = { vb };
        u64 vbOffsets[1] = { 0 };
        mcmd->BindVertexBuffers(kVertexBufferSlot, 1, vbArr, vbOffsets);
        mcmd->BindIndexBuffer(ib, DataFormat::R32_UInt, 0);
        mcmd->DrawIndexed(sphere.indexCount, 0, 0, 1, 0);
        mcmd->EndRenderPass();
    }

    TEST_ASSERT(mcmd->End(), "End");
    TEST_ASSERT(mcmd->Submit(0), "Submit");
    TEST_ASSERT(mcmd->WaitForCompletion(), "WaitForCompletion");

    // ----- 8. Per-layer readback + save PNG -----
    std::filesystem::create_directories("Assets/ReferenceImages/P4b-T3");

    for (u32 c = 0; c < kCascadeCount; ++c) {
        CommandBufferHandle rcmd = device.CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* mrcmd = device.GetCommandBuffer(rcmd);
        TEST_ASSERT(mrcmd->Reset(), "Reset readback");
        TEST_ASSERT(mrcmd->Begin(), "Begin readback");
        BufferTextureCopyRegion region{};
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = c;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {kW, kH, 1};
        mrcmd->CopyTextureToBuffer(rt, readback, &region, 1);
        TEST_ASSERT(mrcmd->End(), "End readback");
        TEST_ASSERT(mrcmd->Submit(0), "Submit readback");
        TEST_ASSERT(mrcmd->WaitForCompletion(), "WaitForCompletion readback");
        device.DestroyCommandBuffer(rcmd);

        void* mapped = device.MapBuffer(readback, 0, readbackDesc.size);
        TEST_ASSERT(mapped != nullptr, "MapBuffer readback");

        std::vector<u8> rgba8(size_t(kW) * kH * 4);
        et::Depth32ToRGBA8(static_cast<const u8*>(mapped), rgba8.data(), kW, kH);
        device.UnmapBuffer(readback);

        u32 fg = 0;
        u32 cx = kW / 2, cy = kH / 2;
        u32 centerIdx = (cy * kW + cx) * 4;
        u8 centerByte = rgba8[centerIdx];
        for (u32 i = 0; i < kW * kH; ++i) {
            if (rgba8[i * 4] < 255) ++fg;
        }
        std::cout << "[TestMetalShadowPass] layer " << c
                  << ": fg=" << fg << " center=" << int(centerByte) << std::endl;

        std::string refPath = "Assets/ReferenceImages/P4b-T3/shadow_sphere_metal_layer"
                              + std::to_string(c) + ".png";
        if (et::SavePNG(refPath.c_str(), rgba8.data(), kW, kH)) {
            std::cout << "[TestMetalShadowPass] saved " << refPath << std::endl;
        } else {
            std::cerr << "[TestMetalShadowPass] SavePNG failed for " << refPath << std::endl;
            return TestResult::Failed;
        }

        if (c == 0) {
            TEST_ASSERT(fg > (kW * kH) / 8, "Layer 0: sphere should cover > 1/8");
            TEST_ASSERT(centerByte < 250, "Layer 0: center visible");
            TEST_ASSERT(centerByte > 50, "Layer 0: center in front");
        }
    }

    // Cleanup
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
    device.DestroyCommandBuffer(cmd);
    device.Shutdown();
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("MetalShadowPassTests");
    suite->AddTestCase(TestCase("RenderSphere4Cascades", TestShadowPass_RenderSphere_Metal));
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    return 0;
}

#else // !__APPLE__

#include <iostream>

int main() {
    std::cout << "[TestMetalShadowPass] Not Apple platform — no-op." << std::endl;
    return 0;
}

#endif
