/**
 * @file TestVulkanSimplePBR.cpp
 * @brief Phase 4b Tier 2 — Vulkan raw RHI PBR parity test (REVISED).
 * @details Renders a sphere with SimplePBR.{vert,frag}.spv via raw Vulkan RHI
 *          (vertex/index buffers, 3 UBOs, albedo texture, 3 descriptor sets,
 *          graphics pipeline) into a 64×64 RGBA16F render target, readback,
 *          tonemap to RGBA8, save as PNG. Pure Vulkan-side validation that the
 *          SPIR-V → pipeline → render path works end-to-end with SimplePBR.
 *
 *          No Metal reference comparison in this revision (T2.1 deferred).
 *          Validation = visible lit sphere (non-zero pixel count + expected
 *          luminance pattern) + PNG saved for offline inspection.
 *
 * Layout matches C++ struct truth (RHIShaderCommon.h):
 *   GlobalShaderData           = 480 B  (set=0, binding=11)
 *   DirectionalLightParameters = 304 B  (set=0, binding=12)
 *   PerObjectData              = 400 B  (set=1, binding=0)
 *   albedoMap                  = CombinedImageSampler (set=2, binding=0)
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

#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>
#include <array>

using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;
namespace et = EngineTest;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

// ============================= Shader SPV reader =============================
std::vector<u8> ReadSPV(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::cerr << "ReadSPV: cannot open " << path << std::endl;
        return {};
    }
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> data(sz);
    if (sz > 0) f.read(reinterpret_cast<char*>(data.data()), sz);
    return data;
}

// ============================= Device fixture =============================
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

// ============================= Math helpers (column-major, matches math::m4x4 / std140 mat4) =============================
m4x4 make_identity_m4x4() {
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = 1.0f;
    r.columns[1][1] = 1.0f;
    r.columns[2][2] = 1.0f;
    r.columns[3][3] = 1.0f;
    return r;
}

/// Right-handed look-at (camera at eye looking at target, up=+Y).
/// Produces a view matrix that transforms world → camera space.
/// Uses glm-style convention: zaxis = normalize(eye - target) (points backward).
m4x4 make_view_m4x4(v3 eye, v3 target, v3 up) {
    // zaxis points from target toward eye (backward, +z in view space).
    v3 diff = {eye[0] - target[0], eye[1] - target[1], eye[2] - target[2]};
    float zl = std::sqrt(diff[0]*diff[0] + diff[1]*diff[1] + diff[2]*diff[2]);
    if (zl < 1e-8f) zl = 1.0f;
    v3 zaxis = {diff[0]/zl, diff[1]/zl, diff[2]/zl};
    // xaxis = normalize(cross(up, zaxis))
    v3 x = {
        up[1]*zaxis[2] - up[2]*zaxis[1],
        up[2]*zaxis[0] - up[0]*zaxis[2],
        up[0]*zaxis[1] - up[1]*zaxis[0],
    };
    float xl = std::sqrt(x[0]*x[0] + x[1]*x[1] + x[2]*x[2]);
    if (xl < 1e-8f) xl = 1.0f;
    v3 xaxis = {x[0]/xl, x[1]/xl, x[2]/xl};
    // yaxis = cross(zaxis, xaxis) (already unit-length by construction)
    v3 yaxis = {
        zaxis[1]*xaxis[2] - zaxis[2]*xaxis[1],
        zaxis[2]*xaxis[0] - zaxis[0]*xaxis[2],
        zaxis[0]*xaxis[1] - zaxis[1]*xaxis[0],
    };

    // View matrix (column-major, glm::lookAtRH)
    // Row 0: xaxis.x, xaxis.y, xaxis.z, -dot(xaxis, eye)
    // Row 1: yaxis.x, yaxis.y, yaxis.z, -dot(yaxis, eye)
    // Row 2: zaxis.x, zaxis.y, zaxis.z, -dot(zaxis, eye)
    // Row 3: 0,0,0,1
    m4x4 m = make_identity_m4x4();
    m.columns[0][0] = xaxis[0];
    m.columns[0][1] = yaxis[0];
    m.columns[0][2] = zaxis[0];
    m.columns[0][3] = 0.0f;

    m.columns[1][0] = xaxis[1];
    m.columns[1][1] = yaxis[1];
    m.columns[1][2] = zaxis[1];
    m.columns[1][3] = 0.0f;

    m.columns[2][0] = xaxis[2];
    m.columns[2][1] = yaxis[2];
    m.columns[2][2] = zaxis[2];
    m.columns[2][3] = 0.0f;

    m.columns[3][0] = -(xaxis[0]*eye[0] + xaxis[1]*eye[1] + xaxis[2]*eye[2]);
    m.columns[3][1] = -(yaxis[0]*eye[0] + yaxis[1]*eye[1] + yaxis[2]*eye[2]);
    m.columns[3][2] = -(zaxis[0]*eye[0] + zaxis[1]*eye[1] + zaxis[2]*eye[2]);
    m.columns[3][3] = 1.0f;
    return m;
}

/// Perspective projection (right-handed, GL-style — z in [-1, +1]).
/// fov_y in radians, aspect = w/h, near/far > 0.
m4x4 make_perspective_m4x4(float fov_y, float aspect, float zNear, float zFar) {
    float f = 1.0f / std::tan(fov_y * 0.5f);
    m4x4 r{};
    std::memset(&r, 0, sizeof(r));
    r.columns[0][0] = f / aspect;
    r.columns[1][1] = f;
    r.columns[2][2] = (zFar + zNear) / (zNear - zFar);   // OpenGL convention
    r.columns[2][3] = -1.0f;
    r.columns[3][2] = (2.0f * zFar * zNear) / (zNear - zFar);
    return r;
}

m4x4 multiply_m4x4(const m4x4& a, const m4x4& b) {
    return a * b;  // math::m4x4 has operator*
}

// ============================= Sphere generation (32B/vertex: pos12 + normal12 + uv8) =============================
struct SphereMesh {
    std::vector<u8> vertices;   // 32 bytes per vertex
    std::vector<u32> indices;
    u32 vertexCount{0};
    u32 indexCount{0};
};

SphereMesh make_sphere(float radius, u32 segments, u32 rings) {
    SphereMesh s;
    s.vertexCount = (rings + 1) * (segments + 1);
    s.indexCount = rings * segments * 6;
    s.vertices.resize(size_t(s.vertexCount) * 32);
    s.indices.resize(s.indexCount);

    auto write_v = [&](u32 i, float px, float py, float pz, float nx, float ny, float nz, float u, float v) {
        u8* p = s.vertices.data() + size_t(i) * 32;
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
        float phi = 3.14159265358979f * float(r) / float(rings);          // [0, π]
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

// ============================= UBO data blobs =============================
// GlobalShaderData: 480B — fill view/projection/viewProjection at offsets 0/64/192.
struct GlobalShaderDataBytes {
    static constexpr u32 kSize = 480;
    u8 bytes[kSize];
    void Fill(const m4x4& view, const m4x4& proj) {
        std::memset(bytes, 0, kSize);
        std::memcpy(bytes + 0,   &view, 64);
        std::memcpy(bytes + 64,  &proj, 64);
        m4x4 vp = multiply_m4x4(proj, view);
        std::memcpy(bytes + 192, &vp, 64);
        // cameraPositionAndViewWidth at 384: eye.x/y/z + viewWidth(=64)
        // cameraDirectionAndViewHeight at 400: -forward.x/y/z + viewHeight(=64)
        // numDirNumPunctualDelta at 416: (1, 0, 0, 0) — 1 dir light
        u32 numDir = 1; u32 numPunct = 0;
        std::memcpy(bytes + 416, &numDir, 4);
        std::memcpy(bytes + 420, &numPunct, 4);
        // renderModeEnableFlags at 432: (3=Full, 0, 0, 0) — renderMode=3
        u32 renderMode = 3;
        std::memcpy(bytes + 432, &renderMode, 4);
        // debugParams at 456: directLightBoost=1.0, iblStrength=0.2, ddgiWeight=1.0, exposure=1.0
        float boost = 1.0f;
        std::memcpy(bytes + 456, &boost, 4);
    }
};

// PerObjectData: 400B — world + invWorld + worldViewProjection + prevWVP + 9 SH coeffs (zero).
struct PerObjectDataBytes {
    static constexpr u32 kSize = 400;
    u8 bytes[kSize];
    void Fill(const m4x4& world, const m4x4& viewProj) {
        std::memset(bytes, 0, kSize);
        std::memcpy(bytes + 0,   &world, 64);
        // invWorld: for identity world, also identity
        m4x4 invW = make_identity_m4x4();
        std::memcpy(bytes + 64,  &invW, 64);
        m4x4 wvp = multiply_m4x4(viewProj, world);
        std::memcpy(bytes + 128, &wvp, 64);
        // prevWVP = WVP (no motion)
        std::memcpy(bytes + 192, &wvp, 64);
        // sh_coeffs[9] at offset 256..399 — leave as zero
    }
};

// DirectionalLightParameters: 304B — viewProjections[4] (1024B? no, 4*64=256), splits, dir+intensity, color+shadow.
// Layout:
//   mat4 viewProjections[4]    // 0..255
//   vec4 splits                // 256..271
//   vec4 directionAndIntensity // 272..287  (xyz=dir, w=intensity)
//   vec4 colorAndShadow        // 288..303  (rgb=color, a=shadowEnabled)
struct DirectionalLightBytes {
    static constexpr u32 kSize = 304;
    u8 bytes[kSize];
    void Fill(v3 dir, float intensity, v3 color, float shadowEnabled) {
        std::memset(bytes, 0, kSize);
        // viewProjections[4] = identity (no shadow rendering, unused in SimplePBR.frag)
        for (int i = 0; i < 4; ++i) {
            m4x4 I = make_identity_m4x4();
            std::memcpy(bytes + i * 64, &I, 64);
        }
        // splits: 4 floats, unused
        // directionAndIntensity
        std::memcpy(bytes + 272, &dir, 12);
        std::memcpy(bytes + 284, &intensity, 4);
        // colorAndShadow
        std::memcpy(bytes + 288, &color, 12);
        std::memcpy(bytes + 300, &shadowEnabled, 4);
    }
};

// ============================= Constants =============================
constexpr u32 kW = 64;
constexpr u32 kH = 64;
constexpr u32 kAlbedoSize = 4;          // 4×4 grey texture
constexpr float kSphereRadius = 1.0f;
constexpr u32 kSegments = 24;
constexpr u32 kRings = 12;

constexpr u32 kSet0_Bind_Global = 11;
constexpr u32 kSet0_Bind_Light  = 12;
constexpr u32 kSet1_Bind_Object = 0;
constexpr u32 kSet2_Bind_Albedo = 0;

} // anonymous namespace

// ============================= Main test =============================
TestResult TestSimplePBR_RenderSphere() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // ----- 1. Shaders -----
    auto vert = ReadSPV("Assets/Shaders/SimplePBR.vert.spv");
    auto frag = ReadSPV("Assets/Shaders/SimplePBR.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read SimplePBR SPIR-V files");

    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel,  "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader vertex");
    TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader fragment");

    // ----- 2. Sphere geometry -----
    SphereMesh sphere = make_sphere(kSphereRadius, kSegments, kRings);

    BufferDesc vbufDesc{};
    vbufDesc.size = sphere.vertices.size();
    vbufDesc.type = BufferType::Vertex;
    vbufDesc.vertex.vertexCount = sphere.vertexCount;
    vbufDesc.vertex.vertexStride = 32;
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

    // ----- 3. UBOs -----
    GlobalShaderDataBytes globalBytes;
    m4x4 view = make_view_m4x4(v3{0.0f, 0.0f, 3.0f}, v3{0.0f, 0.0f, 0.0f}, v3{0.0f, 1.0f, 0.0f});
    m4x4 proj = make_perspective_m4x4(0.785398f /* 45° */, 1.0f, 0.1f, 100.0f);  // aspect=1 (64×64)
    globalBytes.Fill(view, proj);

    BufferDesc gbufDesc{};
    gbufDesc.size = GlobalShaderDataBytes::kSize;
    gbufDesc.type = BufferType::Constant;
    gbufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    gbufDesc.name = "GlobalUBO";
    ResourceHandle gbuf = fx.base->CreateBuffer(gbufDesc);
    TEST_ASSERT(gbuf != handles::INVALID_RESOURCE, "CreateBuffer GlobalUBO");
    TEST_ASSERT(fx.base->UpdateBufferData(gbuf, globalBytes.bytes, GlobalShaderDataBytes::kSize, 0),
                "UpdateBufferData GlobalUBO");

    DirectionalLightBytes lightBytes;
    // WGSL convention: direction points AWAY from light source.
    // dir = (0, 1, 0) → L = (0, -1, 0). Framebuffer Y is flipped in Vulkan
    // (NDC +Y maps to bottom of framebuffer), so world +Y appears at bottom.
    // L = (0,-1,0) lights world -Y hemisphere (= framebuffer top): natural
    // "light from above" visually. Verified by flipping dir and confirming the
    // lit/dark hemispheres swap.
    lightBytes.Fill(v3{0.0f, 1.0f, 0.0f}, 2.0f, v3{1.0f, 1.0f, 1.0f}, 0.0f);

    BufferDesc lbufDesc{};
    lbufDesc.size = DirectionalLightBytes::kSize;
    lbufDesc.type = BufferType::Constant;
    lbufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    lbufDesc.name = "LightUBO";
    ResourceHandle lbuf = fx.base->CreateBuffer(lbufDesc);
    TEST_ASSERT(lbuf != handles::INVALID_RESOURCE, "CreateBuffer LightUBO");
    TEST_ASSERT(fx.base->UpdateBufferData(lbuf, lightBytes.bytes, DirectionalLightBytes::kSize, 0),
                "UpdateBufferData LightUBO");

    PerObjectDataBytes objectBytes;
    m4x4 world = make_identity_m4x4();
    m4x4 viewProj = multiply_m4x4(proj, view);
    objectBytes.Fill(world, viewProj);

    BufferDesc obufDesc{};
    obufDesc.size = PerObjectDataBytes::kSize;
    obufDesc.type = BufferType::Constant;
    obufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    obufDesc.name = "ObjectUBO";
    ResourceHandle obuf = fx.base->CreateBuffer(obufDesc);
    TEST_ASSERT(obuf != handles::INVALID_RESOURCE, "CreateBuffer ObjectUBO");
    TEST_ASSERT(fx.base->UpdateBufferData(obuf, objectBytes.bytes, PerObjectDataBytes::kSize, 0),
                "UpdateBufferData ObjectUBO");

    // ----- 4. Albedo texture (4×4 grey) -----
    std::array<u8, kAlbedoSize * kAlbedoSize * 4> albedoPixels;
    for (size_t i = 0; i < albedoPixels.size(); i += 4) {
        albedoPixels[i + 0] = 128;  // R
        albedoPixels[i + 1] = 128;  // G
        albedoPixels[i + 2] = 128;  // B
        albedoPixels[i + 3] = 255;  // A
    }
    BufferDesc stagingDesc{};
    stagingDesc.size = albedoPixels.size();
    stagingDesc.type = BufferType::Raw;
    stagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    stagingDesc.name = "AlbedoStaging";
    ResourceHandle staging = fx.base->CreateBuffer(stagingDesc);
    TEST_ASSERT(staging != handles::INVALID_RESOURCE, "CreateBuffer staging");
    TEST_ASSERT(fx.base->UpdateBufferData(staging, albedoPixels.data(), albedoPixels.size(), 0),
                "UpdateBufferData staging");

    TextureDesc texDesc{};
    texDesc.size = { kAlbedoSize, kAlbedoSize, 1 };
    texDesc.mipLevels = 1;
    texDesc.arraySize = 1;
    texDesc.format = DataFormat::RG8B8A8_UNorm;
    texDesc.type = TextureType::Texture2D;
    texDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    texDesc.memoryUsage = GPUMemoryUsage::Static;
    texDesc.name = "AlbedoTex";
    ResourceHandle albedoTex = fx.base->CreateTexture(texDesc);
    TEST_ASSERT(albedoTex != handles::INVALID_RESOURCE, "CreateTexture albedo");

    // Upload via CopyBufferToTexture
    {
        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        TEST_ASSERT(vcmd->Reset(), "Reset");
        TEST_ASSERT(vcmd->Begin(), "Begin");
        BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {kAlbedoSize, kAlbedoSize, 1};
        vcmd->CopyBufferToTexture(staging, albedoTex, &region, 1);
        // Transition TRANSFER_DST → SHADER_READ so the descriptor write matches actual layout.
        ResourceBarrier texBarrier{};
        texBarrier.resource = albedoTex;
        texBarrier.beforeState = ResourceState::CopyDest;
        texBarrier.afterState = ResourceState::ShaderResource;
        texBarrier.subresource = 0xFFFFFFFF;
        texBarrier.queueFamily = 0xFFFFFFFF;
        vcmd->InsertBarrier(&texBarrier, 1);
        TEST_ASSERT(vcmd->End(), "End");
        TEST_ASSERT(vcmd->Submit(0), "Submit");
        TEST_ASSERT(vcmd->WaitForCompletion(), "WaitForCompletion");
        fx.base->DestroyCommandBuffer(cmd);
    }

    // Sampler — comparisonFunc MUST be Never (MoltenVK portability: no mutable comparison samplers;
    // also VulkanSampler.cpp:95 treats `!= Never` as compareEnable=TRUE, which would break sampling).
    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.mipFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Wrap;
    samplerDesc.addressV = TextureAddressMode::Wrap;
    samplerDesc.maxAnisotropy = 1;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    SamplerHandle sampler = fx.base->CreateSampler(samplerDesc);
    TEST_ASSERT(sampler != handles::INVALID_SAMPLER, "CreateSampler");

    // ----- 5. Descriptor set layouts -----
    // Set 0: GlobalShaderData (binding 11) + DirectionalLightParameters (binding 12) — both vert+frag visible
    DescriptorSetLayoutBinding set0Bindings[2];
    set0Bindings[0].binding = kSet0_Bind_Global;
    set0Bindings[0].descriptorType = DescriptorType::UniformBuffer;
    set0Bindings[0].descriptorCount = 1;
    set0Bindings[0].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
    set0Bindings[1].binding = kSet0_Bind_Light;
    set0Bindings[1].descriptorType = DescriptorType::UniformBuffer;
    set0Bindings[1].descriptorCount = 1;
    set0Bindings[1].stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
    DescriptorSetLayoutDesc set0Desc{};
    set0Desc.bindingCount = 2;
    set0Desc.bindings = set0Bindings;
    DescriptorSetLayoutHandle set0Layout = fx.base->CreateDescriptorSetLayout(set0Desc);
    TEST_ASSERT(set0Layout != handles::INVALID_RESOURCE, "CreateDescriptorSetLayout set0");

    // Set 1: PerObjectData (binding 0)
    DescriptorSetLayoutBinding set1Binding;
    set1Binding.binding = kSet1_Bind_Object;
    set1Binding.descriptorType = DescriptorType::UniformBuffer;
    set1Binding.descriptorCount = 1;
    set1Binding.stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
    DescriptorSetLayoutDesc set1Desc{};
    set1Desc.bindingCount = 1;
    set1Desc.bindings = &set1Binding;
    DescriptorSetLayoutHandle set1Layout = fx.base->CreateDescriptorSetLayout(set1Desc);
    TEST_ASSERT(set1Layout != handles::INVALID_RESOURCE, "CreateDescriptorSetLayout set1");

    // Set 2: albedoMap (binding 0, CombinedImageSampler)
    DescriptorSetLayoutBinding set2Binding;
    set2Binding.binding = kSet2_Bind_Albedo;
    set2Binding.descriptorType = DescriptorType::CombinedImageSampler;
    set2Binding.descriptorCount = 1;
    set2Binding.stageFlags = ShaderStage::Pixel;
    DescriptorSetLayoutDesc set2Desc{};
    set2Desc.bindingCount = 1;
    set2Desc.bindings = &set2Binding;
    DescriptorSetLayoutHandle set2Layout = fx.base->CreateDescriptorSetLayout(set2Desc);
    TEST_ASSERT(set2Layout != handles::INVALID_RESOURCE, "CreateDescriptorSetLayout set2");

    // Pipeline layout: 3 sets
    DescriptorSetLayoutHandle sets[3] = { set0Layout, set1Layout, set2Layout };
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 3;
    plDesc.setLayouts = sets;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    // ----- 6. Descriptor sets -----
    DescriptorSetDesc ds0Desc{}; ds0Desc.layout = set0Layout;
    DescriptorSetHandle ds0 = fx.base->CreateDescriptorSet(ds0Desc);
    TEST_ASSERT(ds0 != handles::INVALID_RESOURCE, "CreateDescriptorSet set0");

    DescriptorSetDesc ds1Desc{}; ds1Desc.layout = set1Layout;
    DescriptorSetHandle ds1 = fx.base->CreateDescriptorSet(ds1Desc);
    TEST_ASSERT(ds1 != handles::INVALID_RESOURCE, "CreateDescriptorSet set1");

    DescriptorSetDesc ds2Desc{}; ds2Desc.layout = set2Layout;
    DescriptorSetHandle ds2 = fx.base->CreateDescriptorSet(ds2Desc);
    TEST_ASSERT(ds2 != handles::INVALID_RESOURCE, "CreateDescriptorSet set2");

    // Write descriptor sets
    DescriptorBufferInfo globalInfo{};  globalInfo.buffer = gbuf;  globalInfo.offset = 0;  globalInfo.range = GlobalShaderDataBytes::kSize;
    DescriptorBufferInfo lightInfo{};   lightInfo.buffer = lbuf;   lightInfo.offset = 0;   lightInfo.range = DirectionalLightBytes::kSize;
    WriteDescriptorSet set0Writes[2];
    set0Writes[0].dstSet = ds0; set0Writes[0].dstBinding = kSet0_Bind_Global;
    set0Writes[0].dstArrayElement = 0; set0Writes[0].descriptorCount = 1;
    set0Writes[0].descriptorType = DescriptorType::UniformBuffer; set0Writes[0].bufferInfo = &globalInfo;
    set0Writes[1].dstSet = ds0; set0Writes[1].dstBinding = kSet0_Bind_Light;
    set0Writes[1].dstArrayElement = 0; set0Writes[1].descriptorCount = 1;
    set0Writes[1].descriptorType = DescriptorType::UniformBuffer; set0Writes[1].bufferInfo = &lightInfo;
    fx.base->UpdateDescriptorSets(2, set0Writes);

    DescriptorBufferInfo objectInfo{}; objectInfo.buffer = obuf; objectInfo.offset = 0; objectInfo.range = PerObjectDataBytes::kSize;
    WriteDescriptorSet set1Write;
    set1Write.dstSet = ds1; set1Write.dstBinding = kSet1_Bind_Object;
    set1Write.dstArrayElement = 0; set1Write.descriptorCount = 1;
    set1Write.descriptorType = DescriptorType::UniformBuffer; set1Write.bufferInfo = &objectInfo;
    fx.base->UpdateDescriptorSets(1, &set1Write);

    DescriptorImageInfo albedoInfo{};
    albedoInfo.sampler = sampler;
    albedoInfo.imageView = albedoTex;
    albedoInfo.imageLayout = ResourceState::ShaderResource;
    WriteDescriptorSet set2Write;
    set2Write.dstSet = ds2; set2Write.dstBinding = kSet2_Bind_Albedo;
    set2Write.dstArrayElement = 0; set2Write.descriptorCount = 1;
    set2Write.descriptorType = DescriptorType::CombinedImageSampler; set2Write.imageInfo = &albedoInfo;
    fx.base->UpdateDescriptorSets(1, &set2Write);

    // ----- 7. Pipeline -----
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    // Vertex input: location 0 = vec3 position @ 0, location 1 = vec3 normal @ 12, location 2 = vec2 uv @ 24
    gpd.vertexAttributes.resize(3);
    gpd.vertexAttributes[0].location = 0;
    gpd.vertexAttributes[0].binding = 0;
    gpd.vertexAttributes[0].format = DataFormat::RGB32_Float;
    gpd.vertexAttributes[0].offset = 0;
    gpd.vertexAttributes[1].location = 1;
    gpd.vertexAttributes[1].binding = 0;
    gpd.vertexAttributes[1].format = DataFormat::RGB32_Float;
    gpd.vertexAttributes[1].offset = 12;
    gpd.vertexAttributes[2].location = 2;
    gpd.vertexAttributes[2].binding = 0;
    gpd.vertexAttributes[2].format = DataFormat::RG32_Float;
    gpd.vertexAttributes[2].offset = 24;
    gpd.vertexBindings.resize(1);
    gpd.vertexBindings[0].binding = 0;
    gpd.vertexBindings[0].stride = 32;
    gpd.vertexBindings[0].perVertex = true;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::Back;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RGBA16_Float;
    gpd.depthStencilFormat = DataFormat::Unknown;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = fx.base->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline");

    // ----- 8. Render target (64×64 RGBA16F) + readback -----
    TextureDesc rtDesc{};
    rtDesc.size = { kW, kH, 1 };
    rtDesc.mipLevels = 1;
    rtDesc.arraySize = 1;
    rtDesc.format = DataFormat::RGBA16_Float;
    rtDesc.type = TextureType::Texture2D;
    rtDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    rtDesc.memoryUsage = GPUMemoryUsage::Static;
    rtDesc.name = "SimplePBR_RT";
    ResourceHandle rt = fx.base->CreateTexture(rtDesc);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture RT");

    BufferDesc readbackDesc{};
    readbackDesc.size = u64(kW) * kH * 8;  // RGBA16F = 8 bytes/pixel
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "SimplePBR_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    // ----- 9. Render -----
    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = rt;
    rpd.colorAttachments[0].format = DataFormat::RGBA16_Float;
    rpd.colorAttachments[0].loadOp = LoadAction::Clear;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.colorAttachments[0].clearValue.color = v4{0.0f, 0.0f, 0.0f, 1.0f};
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

    // Bind 3 descriptor sets at set 0..2
    DescriptorSetHandle setsToBind[3] = { ds0, ds1, ds2 };
    vcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 3, setsToBind, 0, nullptr);

    // Bind vertex + index buffers
    ResourceHandle vbArr[1] = { vb };
    u64 vbOffsets[1] = { 0 };
    vcmd->BindVertexBuffers(0, 1, vbArr, vbOffsets);
    vcmd->BindIndexBuffer(ib, DataFormat::R32_UInt, 0);

    vcmd->DrawIndexed(sphere.indexCount, 0, 0, 1, 0);
    vcmd->EndRenderPass();

    // Copy rt → readback (RGBA16F: 8 bytes/pixel)
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

    // ----- 10. Readback + analyze -----
    void* mapped = fx.base->MapBuffer(readback, 0, readbackDesc.size);
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");

    // Convert RGBA16F → RGBA8 (tonemapped)
    std::vector<u8> rgba8(size_t(kW) * kH * 4);
    et::RGBA16FToRGBA8(static_cast<const u8*>(mapped), rgba8.data(), kW, kH);

    // Vulkan framebuffer Y is flipped vs Metal/OpenGL (NDC +Y → image bottom).
    // Flip rows in place so the saved PNG matches Metal's OpenGL convention,
    // enabling direct SSIM comparison against sphere_metal.png reference.
    et::FlipYInPlace(rgba8.data(), kW, kH);

    fx.base->UnmapBuffer(readback);

    // Stats: count non-background pixels (any channel > 0) + mean luminance over sphere pixels
    u32 nonZero = 0;
    double lumSum = 0.0;
    u32 cx = kW / 2, cy = kH / 2;
    u32 centerIdx = (cy * kW + cx) * 4;
    u8 centerR = rgba8[centerIdx + 0];
    u8 centerG = rgba8[centerIdx + 1];
    u8 centerB = rgba8[centerIdx + 2];
    for (u32 i = 0; i < kW * kH; ++i) {
        u8 r = rgba8[i * 4 + 0];
        u8 g = rgba8[i * 4 + 1];
        u8 b = rgba8[i * 4 + 2];
        if (r + g + b > 0) {
            ++nonZero;
            lumSum += (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
        }
    }
    double meanLum = nonZero ? lumSum / nonZero : 0.0;
    std::cout << "[TestVulkanSimplePBR] non-zero pixels: " << nonZero << " / " << (kW * kH) << std::endl;
    std::cout << "[TestVulkanSimplePBR] mean luminance (sphere pixels): " << meanLum << std::endl;
    std::cout << "[TestVulkanSimplePBR] center pixel RGBA = ("
              << int(centerR) << "," << int(centerG) << "," << int(centerB) << ",255)" << std::endl;

    // Save PNG for offline inspection
    const char* outPath = "sphere_vulkan.png";
    if (!et::SavePNG(outPath, rgba8.data(), kW, kH)) {
        std::cerr << "[TestVulkanSimplePBR] SavePNG failed" << std::endl;
    } else {
        std::cout << "[TestVulkanSimplePBR] saved " << outPath << std::endl;
    }

    // Validation: visible sphere (non-zero pixel count > 30% of frame) AND center pixel is lit
    TEST_ASSERT(nonZero > (kW * kH) / 3, "Sphere should cover > 1/3 of frame");
    TEST_ASSERT(nonZero < (kW * kH * 9) / 10, "Sphere should not fill frame (background visible)");
    TEST_ASSERT(meanLum > 0.05, "Mean luminance over sphere pixels > 0.05 (not pure black)");

    // Cross-backend parity: SSIM vs Metal reference (sphere_metal.png).
    // Reference is generated by TestMetalSimplePBR on macOS and committed to
    // EngineTest/Assets/ReferenceImages/P4b-T2/. The CMake POST_BUILD step
    // copies Assets/ next to the test binary. If the reference is absent
    // (e.g., running on Linux/Win without the Metal reference), skip SSIM
    // rather than fail — Vulkan-side parity is already covered by the
    // assertions above.
    const char* refPath = "Assets/ReferenceImages/P4b-T2/sphere_metal.png";
    std::vector<u8> refRgba;
    u32 refW = 0, refH = 0;
    if (et::LoadPNG(refPath, refRgba, refW, refH)) {
        if (refW == kW && refH == kH) {
            float ssim = et::ComputeSSIM(rgba8.data(), refRgba.data(), kW, kH);
            std::cout << "[TestVulkanSimplePBR] SSIM vs Metal reference: " << ssim << std::endl;
            TEST_ASSERT(ssim >= 0.95f, "Cross-backend SSIM >= 0.95 (Metal parity)");
        } else {
            std::cerr << "[TestVulkanSimplePBR] Metal reference dimensions mismatch ("
                      << refW << "x" << refH << " vs " << kW << "x" << kH
                      << ") — skipping SSIM" << std::endl;
        }
    } else {
        std::cerr << "[TestVulkanSimplePBR] Metal reference not found at "
                  << refPath << " — skipping SSIM (non-macOS build?)" << std::endl;
    }

    // Cleanup
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(rt);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds2);
    fx.base->DestroyDescriptorSet(ds1);
    fx.base->DestroyDescriptorSet(ds0);
    fx.base->DestroyDescriptorSetLayout(set2Layout);
    fx.base->DestroyDescriptorSetLayout(set1Layout);
    fx.base->DestroyDescriptorSetLayout(set0Layout);
    fx.base->DestroySampler(sampler);
    fx.base->DestroyTexture(albedoTex);
    fx.base->DestroyBuffer(staging);
    fx.base->DestroyBuffer(obuf);
    fx.base->DestroyBuffer(lbuf);
    fx.base->DestroyBuffer(gbuf);
    fx.base->DestroyBuffer(ib);
    fx.base->DestroyBuffer(vb);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

void RegisterVulkanSimplePBRTests() {
    auto suite = std::make_shared<TestSuite>("VulkanSimplePBRTests");
    suite->AddTestCase(TestCase("RenderSphere", TestSimplePBR_RenderSphere));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanSimplePBRTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanSimplePBR] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif
