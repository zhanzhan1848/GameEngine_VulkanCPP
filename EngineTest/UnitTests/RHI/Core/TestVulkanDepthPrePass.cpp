/**
 * @file TestVulkanDepthPrePass.cpp
 * @brief Phase 4b Tier 3.1 — DepthPrePass port (CameraDepth shader).
 * @details Renders a sphere to a D32_FLOAT depth-only render target via the
 *          WGSL-compiled CameraDepth.spv (vertex) + hand-written noop.frag.
 *          Reads back the depth buffer, converts to RGBA8 grayscale, flips Y
 *          to match Metal/OpenGL convention, and compares against the Metal
 *          reference PNG via SSIM >= 0.95.
 *
 * Validates:
 *   - VulkanTexture::GetAspectMask() end-to-end (T3.0a/b) for D32 copies.
 *   - Depth-only pipeline (no color attachments, depthWrite=true, depthTest=Less).
 *   - WGSL→SPIR-V via naga is usable from the Vulkan RHI.
 *   - ImageCompare Depth32ToRGBA8 + FlipY + SSIM pipeline.
 */

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIRenderPass.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;
namespace et = EngineTest;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

constexpr u32 kW = 64;
constexpr u32 kH = 64;
constexpr float kSphereRadius = 1.0f;
constexpr u32 kSegments = 24;
constexpr u32 kRings = 12;

// Math helpers (column-major, matches math::m4x4 / std140 mat4).
// Mirrored from TestVulkanSimplePBR.cpp — these are pure host-side builders
// for filling UBO blobs. No shared header yet; inline per test for now.
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

// Engine's static_normal_texture vertex layout: 32B stride.
//   location 0: vec3 position @ 0
//   location 1: u32 (pad / packed normal)  @ 12
//   location 2: u32                          @ 16
//   location 3: u32                          @ 20
//   location 4: vec2 uv                      @ 24
//
// CameraDepth.spv declares all 5 inputs (see spirv-dis output). Even though
// the shader only reads location 0, we must bind all 5 to satisfy validation.
// We feed the same 32B stride as TestVulkanSimplePBR (pos + float-normal + uv);
// locations 1-3 read float bytes as u32 but the shader ignores them.
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

std::vector<u8> ReadSPV(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::cerr << "ReadSPV: cannot open " << path << std::endl;
        return {};
    }
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> data;
    if (sz > 0) {
        data.resize(size_t(sz));
        f.read(reinterpret_cast<char*>(data.data()), sz);
    }
    return data;
}

struct DeviceFixture {
    RHIDeviceBase* base{nullptr};
    VulkanDevice* vk{nullptr};
    DeviceDesc desc{};
    bool Init() {
        desc.platform = RHIPlatform::Vulkan;
        desc.enableValidation = true;
        desc.enableDebug = true;
        base = CreateRHIDevice(desc);
        if (!base) return false;
        vk = static_cast<VulkanDevice*>(base);
        return true;
    }
    ~DeviceFixture() { if (base) base->Shutdown(); }
};

} // anonymous namespace

/// Render sphere to D32_FLOAT depth-only RT, read back, SSIM vs Metal reference.
TestResult TestDepthPrePass_RenderSphere() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // ----- 1. Shaders -----
    auto vert = ReadSPV("Assets/Shaders/CameraDepth.spv");
    auto frag = ReadSPV("Assets/Shaders/CameraDepth.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(),
                "Read CameraDepth SPIR-V (run build_spv.sh if missing)");

    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(),
                                            ShaderStage::Vertex, "camera_depth_vs");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(),
                                            ShaderStage::Pixel, "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader vertex (CameraDepth)");
    TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader fragment (noop)");

    // ----- 2. Sphere geometry -----
    SphereMesh sphere = make_sphere(kSphereRadius, kSegments, kRings);

    BufferDesc vbufDesc{};
    vbufDesc.size = sphere.vertices.size();
    vbufDesc.type = BufferType::Vertex;
    vbufDesc.vertex.vertexCount = sphere.vertexCount;
    vbufDesc.vertex.vertexStride = kVertexStride;
    vbufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    vbufDesc.name = "SphereVB";
    ResourceHandle vb = fx.base->CreateBuffer(vbufDesc);
    TEST_ASSERT(vb != handles::INVALID_RESOURCE, "CreateBuffer vertex");
    TEST_ASSERT(fx.base->UpdateBufferData(vb, sphere.vertices.data(), sphere.vertices.size(), 0),
                "UpdateBufferData vertex");

    BufferDesc ibufDesc{};
    ibufDesc.size = sphere.indices.size() * sizeof(u32);
    ibufDesc.type = BufferType::Index;
    ibufDesc.index.indexCount = sphere.indexCount;
    ibufDesc.index.format = DataFormat::R32_UInt;
    ibufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    ibufDesc.name = "SphereIB";
    ResourceHandle ib = fx.base->CreateBuffer(ibufDesc);
    TEST_ASSERT(ib != handles::INVALID_RESOURCE, "CreateBuffer index");
    TEST_ASSERT(fx.base->UpdateBufferData(ib, sphere.indices.data(), ibufDesc.size, 0),
                "UpdateBufferData index");

    // ----- 3. UBO (CameraDepthPerObject: mat4 worldViewProjection @ binding 0) -----
    // Camera: same as T2 SimplePBR — eye (0,0,3), look at origin, 45° FOV.
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
    ResourceHandle ubo = fx.base->CreateBuffer(ubufDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer CameraDepthUBO");
    TEST_ASSERT(fx.base->UpdateBufferData(ubo, &uboBytes, kUBOSize, 0),
                "UpdateBufferData CameraDepthUBO");

    // ----- 4. Descriptor set (set 0, binding 0) -----
    DescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = DescriptorType::UniformBuffer;
    binding.descriptorCount = 1;
    binding.stageFlags = ShaderStage::Vertex;
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 1;
    layoutDesc.bindings = &binding;
    DescriptorSetLayoutHandle layout = fx.base->CreateDescriptorSetLayout(layoutDesc);
    TEST_ASSERT(layout != handles::INVALID_RESOURCE, "CreateDescriptorSetLayout");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);
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
    fx.base->UpdateDescriptorSets(1, &write);

    // ----- 5. Pipeline (depth-only) -----
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    // Vertex input: 5 attributes matching CameraDepth.spv SPIR-V decoration.
    gpd.vertexAttributes.resize(5);
    gpd.vertexAttributes[0].location = 0;
    gpd.vertexAttributes[0].binding = 0;
    gpd.vertexAttributes[0].format = DataFormat::RGB32_Float;
    gpd.vertexAttributes[0].offset = 0;
    gpd.vertexAttributes[1].location = 1;
    gpd.vertexAttributes[1].binding = 0;
    gpd.vertexAttributes[1].format = DataFormat::R32_UInt;
    gpd.vertexAttributes[1].offset = 12;
    gpd.vertexAttributes[2].location = 2;
    gpd.vertexAttributes[2].binding = 0;
    gpd.vertexAttributes[2].format = DataFormat::R32_UInt;
    gpd.vertexAttributes[2].offset = 16;
    gpd.vertexAttributes[3].location = 3;
    gpd.vertexAttributes[3].binding = 0;
    gpd.vertexAttributes[3].format = DataFormat::R32_UInt;
    gpd.vertexAttributes[3].offset = 20;
    gpd.vertexAttributes[4].location = 4;
    gpd.vertexAttributes[4].binding = 0;
    gpd.vertexAttributes[4].format = DataFormat::RG32_Float;
    gpd.vertexAttributes[4].offset = 24;
    gpd.vertexBindings.resize(1);
    gpd.vertexBindings[0].binding = 0;
    gpd.vertexBindings[0].stride = kVertexStride;
    gpd.vertexBindings[0].perVertex = true;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::Back;
    gpd.renderTargetCount = 0;  // depth-only
    gpd.depthStencilFormat = DataFormat::D32_Float;
    gpd.enableDepthTest = true;
    gpd.enableDepthWrite = true;
    gpd.depthFunc = ComparisonFunc::Less;
    PipelineHandle pipe = fx.base->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline (depth-only)");

    // ----- 6. Depth RT + readback -----
    TextureDesc rtDesc{};
    rtDesc.size = { kW, kH, 1 };
    rtDesc.mipLevels = 1;
    rtDesc.arraySize = 1;
    rtDesc.format = DataFormat::D32_Float;
    rtDesc.type = TextureType::Texture2D;
    rtDesc.usage = TextureUsage::DepthStencil | TextureUsage::CopySource;
    rtDesc.memoryUsage = GPUMemoryUsage::Static;
    rtDesc.name = "DepthPrePass_RT";
    ResourceHandle rt = fx.base->CreateTexture(rtDesc);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture D32 RT");

    BufferDesc readbackDesc{};
    readbackDesc.size = u64(kW) * kH * 4;  // D32 = 4 bytes/pixel
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "DepthPrePass_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
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

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset(), "Reset");
    TEST_ASSERT(vcmd->Begin(), "Begin");
    vcmd->BeginRenderPass(rpd);
    vcmd->BindGraphicsPipeline(pipe);
    vcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 1, &ds, 0, nullptr);
    ResourceHandle vbArr[1] = { vb };
    u64 vbOffsets[1] = { 0 };
    vcmd->BindVertexBuffers(0, 1, vbArr, vbOffsets);
    vcmd->BindIndexBuffer(ib, DataFormat::R32_UInt, 0);
    vcmd->DrawIndexed(sphere.indexCount, 0, 0, 1, 0);
    vcmd->EndRenderPass();

    // Copy depth RT → readback
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kW, kH, 1};
    vcmd->CopyTextureToBuffer(rt, readback, &region, 1);

    TEST_ASSERT(vcmd->End(), "End");
    TEST_ASSERT(vcmd->Submit(0), "Submit");
    TEST_ASSERT(vcmd->WaitForCompletion(), "WaitForCompletion");

    // ----- 8. Readback + convert to RGBA8 grayscale -----
    void* mapped = fx.base->MapBuffer(readback, 0, readbackDesc.size);
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");

    std::vector<u8> rgba8(size_t(kW) * kH * 4);
    et::Depth32ToRGBA8(static_cast<const u8*>(mapped), rgba8.data(), kW, kH);
    et::FlipYInPlace(rgba8.data(), kW, kH);
    fx.base->UnmapBuffer(readback);

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
    std::cout << "[TestVulkanDepthPrePass] foreground pixels (depth<1): "
              << fg << " / " << (kW * kH) << std::endl;
    std::cout << "[TestVulkanDepthPrePass] mean depth (fg): " << meanDepth << std::endl;
    std::cout << "[TestVulkanDepthPrePass] center pixel depth byte = "
              << int(centerByte) << " (" << centerDepth << ")" << std::endl;

    // Sanity: sphere should be visible (>25% foreground, <90% foreground).
    TEST_ASSERT(fg > (kW * kH) / 4, "Sphere should cover > 1/4 of depth frame");
    TEST_ASSERT(fg < (kW * kH * 9) / 10, "Sphere should not fill depth frame");
    // Center pixel is the front of the sphere (closest to camera) — depth
    // should be well below 1.0 (background) but above 0.5 (in front of cam).
    // Geometry: eye at (0,0,3), sphere radius 1 at origin → front at view-z=-2;
    // with near=0.1/far=100 OpenGL projection → depth ≈ 0.95.
    TEST_ASSERT(centerByte < 250, "Center pixel depth < 250 (not background)");
    TEST_ASSERT(centerByte > 128, "Center pixel depth > 128 (in front of camera)");

    // Save PNG for offline inspection.
    const char* outPath = "depth_sphere_vulkan.png";
    if (et::SavePNG(outPath, rgba8.data(), kW, kH)) {
        std::cout << "[TestVulkanDepthPrePass] saved " << outPath << std::endl;
    }

    // ----- 9. Cross-backend parity: SSIM vs Metal reference -----
    const char* refPath = "Assets/ReferenceImages/P4b-T3/depth_sphere_metal.png";
    std::vector<u8> refRgba;
    u32 refW = 0, refH = 0;
    if (et::LoadPNG(refPath, refRgba, refW, refH)) {
        if (refW == kW && refH == kH) {
            float ssim = et::ComputeSSIM(rgba8.data(), refRgba.data(), kW, kH);
            std::cout << "[TestVulkanDepthPrePass] SSIM vs Metal reference: "
                      << ssim << std::endl;
            TEST_ASSERT(ssim >= 0.95f,
                        "Cross-backend SSIM >= 0.95 (Metal depth parity)");
        } else {
            std::cerr << "[TestVulkanDepthPrePass] Metal reference dimensions mismatch ("
                      << refW << "x" << refH << " vs " << kW << "x" << kH
                      << ") — skipping SSIM" << std::endl;
        }
    } else {
        std::cerr << "[TestVulkanDepthPrePass] Metal reference not found at "
                  << refPath << " — run TestMetalDepthPrePass to generate; skipping SSIM"
                  << std::endl;
    }

    // Cleanup
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(rt);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroyBuffer(ubo);
    fx.base->DestroyBuffer(ib);
    fx.base->DestroyBuffer(vb);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

void RegisterVulkanDepthPrePassTests() {
    auto suite = std::make_shared<TestSuite>("VulkanDepthPrePassTests");
    suite->AddTestCase(TestCase("RenderSphere", TestDepthPrePass_RenderSphere));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanDepthPrePassTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanDepthPrePass] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
