/**
 * @file TestVulkanForwardPBR_Lite.cpp
 * @brief Phase 4b Tier 3.7 — ForwardLightBuffer struct layout validation.
 * @details Validates the full engine ForwardLightBuffer (25808 bytes) — the
 *          most complex UBO in the renderer. SimplePBR already covers
 *          GlobalShaderData/PerObjectData/DirectionalLightParameters layouts,
 *          but those use a standalone DirectionalLightParameters UBO. The
 *          production shader wraps directional lights inside ForwardLightBuffer
 *          alongside the lights[128] punctual array.
 *
 *          This test substitutes ForwardPBR_Lite.{vert,frag}.spv (same lighting
 *          math as SimplePBR but reading through ForwardLightBuffer struct),
 *          reuses the SimplePBR Metal reference frame (sphere_metal.png), and
 *          validates SSIM ≥ 0.95.
 *
 *          Scene: sphere + 1 directional light + 1 dim punctual light (positioned
 *          out of range so contribution is ~0). The punctual light exists to
 *          force the shader to index lights[0] and validate the 192B stride
 *          inside the lights[] array.
 *
 * ForwardLightBuffer layout (25808B total):
 *   offset    0: u32 directionalLightCount
 *   offset    4: u32 punctualLightCount
 *   offset    8: u32 padding[2]
 *   offset   16: DirectionalLightParameters directionalLights[4]   (4 × 304B = 1216B)
 *   offset 1232: LightParameters lights[128]                       (128 × 192B = 24576B)
 *   offset 25808: END
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
        zaxis[2]*xaxis[0] - zaxis[0]*zaxis[2],
        zaxis[0]*xaxis[1] - zaxis[1]*xaxis[0],
    };

    m4x4 m = make_identity_m4x4();
    m.columns[0][0] = xaxis[0];
    m.columns[0][1] = yaxis[0];
    m.columns[0][2] = zaxis[0];
    m.columns[1][0] = xaxis[1];
    m.columns[1][1] = yaxis[1];
    m.columns[1][2] = zaxis[1];
    m.columns[2][0] = xaxis[2];
    m.columns[2][1] = yaxis[2];
    m.columns[2][2] = zaxis[2];
    m.columns[3][0] = -(xaxis[0]*eye[0] + xaxis[1]*eye[1] + xaxis[2]*eye[2]);
    m.columns[3][1] = -(yaxis[0]*eye[0] + yaxis[1]*eye[1] + yaxis[2]*eye[2]);
    m.columns[3][2] = -(zaxis[0]*eye[0] + zaxis[1]*eye[1] + zaxis[2]*eye[2]);
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

// ============================= Sphere generation (32B/vertex: pos12 + normal12 + uv8) =============================
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

// ============================= UBO data blobs =============================
struct GlobalShaderDataBytes {
    static constexpr u32 kSize = 480;
    u8 bytes[kSize];
    void Fill(const m4x4& view, const m4x4& proj) {
        std::memset(bytes, 0, kSize);
        std::memcpy(bytes + 0,   &view, 64);
        std::memcpy(bytes + 64,  &proj, 64);
        m4x4 vp = multiply_m4x4(proj, view);
        std::memcpy(bytes + 192, &vp, 64);
        u32 numDir = 1; u32 numPunct = 1;
        std::memcpy(bytes + 416, &numDir, 4);
        std::memcpy(bytes + 420, &numPunct, 4);
        u32 renderMode = 3;
        std::memcpy(bytes + 432, &renderMode, 4);
        float boost = 1.0f;
        std::memcpy(bytes + 456, &boost, 4);
    }
};

struct PerObjectDataBytes {
    static constexpr u32 kSize = 400;
    u8 bytes[kSize];
    void Fill(const m4x4& world, const m4x4& viewProj) {
        std::memset(bytes, 0, kSize);
        std::memcpy(bytes + 0,   &world, 64);
        m4x4 invW = make_identity_m4x4();
        std::memcpy(bytes + 64,  &invW, 64);
        m4x4 wvp = multiply_m4x4(viewProj, world);
        std::memcpy(bytes + 128, &wvp, 64);
        std::memcpy(bytes + 192, &wvp, 64);
    }
};

// ForwardLightBuffer: 25808 bytes.
//   offset 0:    u32 directionalLightCount
//   offset 4:    u32 punctualLightCount
//   offset 8:    u32 padding[2]
//   offset 16:   DirectionalLightParameters directionalLights[4]   (4 × 304B = 1216B)
//   offset 1232: LightParameters lights[128]                       (128 × 192B = 24576B)
//
// DirectionalLightParameters (304B):
//   mat4 viewProjections[4]        // 0..255
//   vec4 splits                    // 256..271
//   vec4 directionAndIntensity     // 272..287  (xyz=dir, w=intensity)
//   vec4 colorAndShadow            // 288..303
//
// LightParameters (192B):
//   vec4 position      // 0..15    (xyz=pos, w=intensity)
//   vec4 _pad0         // 16..31
//   vec4 direction     // 32..47   (xyz=dir, w=range)
//   vec4 _pad1         // 48..63
//   vec4 color         // 64..79   (xyz=color, w=cosUmbra)
//   vec4 _pad2         // 80..95
//   vec4 attenuation   // 96..111  (xyz=att, w=cosPenumbra)
//   vec4 _lt_sh_pad    // 112..127 (lightType, shadowIndex, padding, padding)
//   mat4 viewProjection // 128..191
struct ForwardLightBufferBytes {
    static constexpr u32 kSize = 25808;
    static constexpr u32 kDirLightOffset = 16;
    static constexpr u32 kPunctualLightOffset = 1232;
    static constexpr u32 kDirLightStride = 304;       // sizeof(DirectionalLightParameters)
    static constexpr u32 kPunctualLightStride = 192;  // sizeof(LightParameters)
    static_assert(kDirLightOffset + 4 * kDirLightStride == kPunctualLightOffset,
                  "directionalLights[4] must end where lights[128] begins");
    static_assert(kPunctualLightOffset + 128 * kPunctualLightStride == kSize,
                  "lights[128] must end at kSize");
    u8 bytes[kSize];

    void Fill(v3 dir, float intensity, v3 color,
              v3 plightPos, float plightIntensity, v3 plightColor,
              float plightRange, v3 plightAtt) {
        std::memset(bytes, 0, kSize);

        // Counts: 1 directional, 1 punctual.
        u32 dirCount = 1;
        u32 punctualCount = 1;
        std::memcpy(bytes + 0, &dirCount, 4);
        std::memcpy(bytes + 4, &punctualCount, 4);

        // ----- DirectionalLightParameters directionalLights[0] -----
        u8* dir0 = bytes + kDirLightOffset;
        // viewProjections[4]: identity (unused in Lite shader).
        for (int i = 0; i < 4; ++i) {
            m4x4 I = make_identity_m4x4();
            std::memcpy(dir0 + i * 64, &I, 64);
        }
        // splits at 256: leave 0.
        // directionAndIntensity at 272.
        std::memcpy(dir0 + 272, &dir, 12);
        std::memcpy(dir0 + 284, &intensity, 4);
        // colorAndShadow at 288.
        std::memcpy(dir0 + 288, &color, 12);
        float shadowEnabled = 0.0f;
        std::memcpy(dir0 + 300, &shadowEnabled, 4);

        // ----- LightParameters lights[0] -----
        u8* pl0 = bytes + kPunctualLightOffset;
        // position at 0..15 (xyz=pos, w=intensity).
        std::memcpy(pl0 + 0,  &plightPos, 12);
        std::memcpy(pl0 + 12, &plightIntensity, 4);
        // direction at 32..47 (xyz=dir, w=range).
        v3 pdir = {1.0f, 0.0f, 0.0f};
        std::memcpy(pl0 + 32, &pdir, 12);
        std::memcpy(pl0 + 44, &plightRange, 4);
        // color at 64..79 (xyz=color, w=cosUmbra).
        std::memcpy(pl0 + 64, &plightColor, 12);
        float cosUmbra = 0.0f;
        std::memcpy(pl0 + 76, &cosUmbra, 4);
        // attenuation at 96..111 (xyz=att, w=cosPenumbra).
        std::memcpy(pl0 + 96, &plightAtt, 12);
        float cosPenumbra = 0.0f;
        std::memcpy(pl0 + 108, &cosPenumbra, 4);
        // _lt_sh_pad at 112..127: lightType=1 (Point), shadowIndex=-1, then pad.
        int32_t lightType = 1;
        int32_t shadowIndex = -1;
        std::memcpy(pl0 + 112, &lightType, 4);
        std::memcpy(pl0 + 116, &shadowIndex, 4);
        // viewProjection at 128..191: identity (unused).
        m4x4 I = make_identity_m4x4();
        std::memcpy(pl0 + 128, &I, 64);
    }
};

constexpr u32 kW = 64;
constexpr u32 kH = 64;
constexpr u32 kAlbedoSize = 4;
constexpr float kSphereRadius = 1.0f;
constexpr u32 kSegments = 24;
constexpr u32 kRings = 12;

constexpr u32 kSet0_Bind_Global = 11;
constexpr u32 kSet0_Bind_Light  = 12;
constexpr u32 kSet1_Bind_Object = 0;
constexpr u32 kSet2_Bind_Albedo = 0;

} // anonymous namespace

TestResult TestForwardPBR_Lite_RenderSphere() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto vert = ReadSPV("Assets/Shaders/ForwardPBR_Lite.vert.spv");
    auto frag = ReadSPV("Assets/Shaders/ForwardPBR_Lite.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read ForwardPBR_Lite SPIR-V files");

    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel,  "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader vertex");
    TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader fragment");

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

    GlobalShaderDataBytes globalBytes;
    m4x4 view = make_view_m4x4(v3{0.0f, 0.0f, 3.0f}, v3{0.0f, 0.0f, 0.0f}, v3{0.0f, 1.0f, 0.0f});
    m4x4 proj = make_perspective_m4x4(0.785398f, 1.0f, 0.1f, 100.0f);
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

    // ForwardLightBuffer: directional light same as SimplePBR + 1 out-of-range punctual.
    ForwardLightBufferBytes lightBytes;
    lightBytes.Fill(
        /*dir*/          v3{0.0f, 1.0f, 0.0f},
        /*intensity*/    2.0f,
        /*color*/        v3{1.0f, 1.0f, 1.0f},
        /*plightPos*/    v3{10.0f, 10.0f, 10.0f},   // far from sphere at origin
        /*plightInt*/    1.0f,
        /*plightColor*/  v3{1.0f, 1.0f, 1.0f},
        /*plightRange*/  0.5f,                       // << sphere distance (≥1)
        /*plightAtt*/    v3{1.0f, 0.0f, 0.0f});

    BufferDesc lbufDesc{};
    lbufDesc.size = ForwardLightBufferBytes::kSize;
    lbufDesc.type = BufferType::Constant;
    lbufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    lbufDesc.name = "ForwardLightBufferUBO";
    ResourceHandle lbuf = fx.base->CreateBuffer(lbufDesc);
    TEST_ASSERT(lbuf != handles::INVALID_RESOURCE, "CreateBuffer ForwardLightBuffer");
    TEST_ASSERT(fx.base->UpdateBufferData(lbuf, lightBytes.bytes, ForwardLightBufferBytes::kSize, 0),
                "UpdateBufferData ForwardLightBuffer");

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

    // ----- Albedo texture (4×4 grey) -----
    std::array<u8, kAlbedoSize * kAlbedoSize * 4> albedoPixels;
    for (size_t i = 0; i < albedoPixels.size(); i += 4) {
        albedoPixels[i + 0] = 128;
        albedoPixels[i + 1] = 128;
        albedoPixels[i + 2] = 128;
        albedoPixels[i + 3] = 255;
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

    TextureDesc albedoDesc{};
    albedoDesc.size = { kAlbedoSize, kAlbedoSize, 1 };
    albedoDesc.mipLevels = 1;
    albedoDesc.arraySize = 1;
    albedoDesc.format = DataFormat::RGBA8_UNorm;
    albedoDesc.type = TextureType::Texture2D;
    albedoDesc.usage = TextureUsage::CopyDest | TextureUsage::ShaderResource;
    albedoDesc.memoryUsage = GPUMemoryUsage::Static;
    albedoDesc.name = "AlbedoTex";
    ResourceHandle albedoTex = fx.base->CreateTexture(albedoDesc);
    TEST_ASSERT(albedoTex != handles::INVALID_RESOURCE, "CreateTexture albedo");

    {
        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        vcmd->Reset(); vcmd->Begin();
        BufferTextureCopyRegion region{};
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = { kAlbedoSize, kAlbedoSize, 1 };
        vcmd->CopyBufferToTexture(staging, albedoTex, &region, 1);
        ResourceBarrier b{};
        b.resource = albedoTex;
        b.beforeState = ResourceState::CopyDest;
        b.afterState = ResourceState::ShaderResource;
        b.subresource = 0xFFFFFFFF;
        b.queueFamily = 0xFFFFFFFF;
        vcmd->InsertBarrier(&b, 1);
        vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();
        fx.base->DestroyCommandBuffer(cmd);
    }

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

    // ----- Descriptor set layouts -----
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

    DescriptorSetLayoutHandle sets[3] = { set0Layout, set1Layout, set2Layout };
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 3;
    plDesc.setLayouts = sets;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    DescriptorSetDesc ds0Desc{}; ds0Desc.layout = set0Layout;
    DescriptorSetHandle ds0 = fx.base->CreateDescriptorSet(ds0Desc);
    TEST_ASSERT(ds0 != handles::INVALID_RESOURCE, "CreateDescriptorSet set0");
    DescriptorSetDesc ds1Desc{}; ds1Desc.layout = set1Layout;
    DescriptorSetHandle ds1 = fx.base->CreateDescriptorSet(ds1Desc);
    TEST_ASSERT(ds1 != handles::INVALID_RESOURCE, "CreateDescriptorSet set1");
    DescriptorSetDesc ds2Desc{}; ds2Desc.layout = set2Layout;
    DescriptorSetHandle ds2 = fx.base->CreateDescriptorSet(ds2Desc);
    TEST_ASSERT(ds2 != handles::INVALID_RESOURCE, "CreateDescriptorSet set2");

    DescriptorBufferInfo globalInfo{};
    globalInfo.buffer = gbuf;
    globalInfo.offset = 0;
    globalInfo.range = GlobalShaderDataBytes::kSize;
    DescriptorBufferInfo lightInfo{};
    lightInfo.buffer = lbuf;
    lightInfo.offset = 0;
    lightInfo.range = ForwardLightBufferBytes::kSize;
    WriteDescriptorSet set0Writes[2];
    set0Writes[0].dstSet = ds0; set0Writes[0].dstBinding = kSet0_Bind_Global;
    set0Writes[0].dstArrayElement = 0; set0Writes[0].descriptorCount = 1;
    set0Writes[0].descriptorType = DescriptorType::UniformBuffer; set0Writes[0].bufferInfo = &globalInfo;
    set0Writes[1].dstSet = ds0; set0Writes[1].dstBinding = kSet0_Bind_Light;
    set0Writes[1].dstArrayElement = 0; set0Writes[1].descriptorCount = 1;
    set0Writes[1].descriptorType = DescriptorType::UniformBuffer; set0Writes[1].bufferInfo = &lightInfo;
    fx.base->UpdateDescriptorSets(2, set0Writes);

    DescriptorBufferInfo objectInfo{};
    objectInfo.buffer = obuf;
    objectInfo.offset = 0;
    objectInfo.range = PerObjectDataBytes::kSize;
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

    // ----- Pipeline -----
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
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

    TextureDesc rtDesc{};
    rtDesc.size = { kW, kH, 1 };
    rtDesc.mipLevels = 1;
    rtDesc.arraySize = 1;
    rtDesc.format = DataFormat::RGBA16_Float;
    rtDesc.type = TextureType::Texture2D;
    rtDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    rtDesc.memoryUsage = GPUMemoryUsage::Static;
    rtDesc.name = "ForwardPBR_Lite_RT";
    ResourceHandle rt = fx.base->CreateTexture(rtDesc);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture RT");

    BufferDesc readbackDesc{};
    readbackDesc.size = u64(kW) * kH * 8;
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "ForwardPBR_Lite_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

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

    DescriptorSetHandle setsToBind[3] = { ds0, ds1, ds2 };
    vcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 3, setsToBind, 0, nullptr);

    ResourceHandle vbArr[1] = { vb };
    u64 vbOffsets[1] = { 0 };
    vcmd->BindVertexBuffers(0, 1, vbArr, vbOffsets);
    vcmd->BindIndexBuffer(ib, DataFormat::R32_UInt, 0);

    vcmd->DrawIndexed(sphere.indexCount, 0, 0, 1, 0);
    vcmd->EndRenderPass();

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

    void* mapped = fx.base->MapBuffer(readback, 0, readbackDesc.size);
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");

    std::vector<u8> rgba8(size_t(kW) * kH * 4);
    et::RGBA16FToRGBA8(static_cast<const u8*>(mapped), rgba8.data(), kW, kH);
    et::FlipYInPlace(rgba8.data(), kW, kH);
    fx.base->UnmapBuffer(readback);

    u32 nonZero = 0;
    double lumSum = 0.0;
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
    std::cout << "[TestVulkanForwardPBR_Lite] non-zero pixels: " << nonZero << " / " << (kW * kH) << std::endl;
    std::cout << "[TestVulkanForwardPBR_Lite] mean luminance (sphere pixels): " << meanLum << std::endl;

    const char* outPath = "forwardpbr_lite_vulkan.png";
    if (!et::SavePNG(outPath, rgba8.data(), kW, kH)) {
        std::cerr << "[TestVulkanForwardPBR_Lite] SavePNG failed" << std::endl;
    } else {
        std::cout << "[TestVulkanForwardPBR_Lite] saved " << outPath << std::endl;
    }

    TEST_ASSERT(nonZero > (kW * kH) / 3, "Sphere should cover > 1/3 of frame");
    TEST_ASSERT(nonZero < (kW * kH * 9) / 10, "Sphere should not fill frame");
    TEST_ASSERT(meanLum > 0.05, "Mean luminance over sphere pixels > 0.05");

    // Parity: SSIM vs SimplePBR Metal reference. The ForwardPBR_Lite lighting math
    // matches SimplePBR (1 dir light + ambient + Reinhard + gamma). The punctual
    // light contributes ~0 (out-of-range), so output should be near-identical.
    const char* refPath = "Assets/ReferenceImages/P4b-T2/sphere_metal.png";
    std::vector<u8> refRgba;
    u32 refW = 0, refH = 0;
    if (et::LoadPNG(refPath, refRgba, refW, refH)) {
        if (refW == kW && refH == kH) {
            float ssim = et::ComputeSSIM(rgba8.data(), refRgba.data(), kW, kH);
            std::cout << "[TestVulkanForwardPBR_Lite] SSIM vs SimplePBR Metal reference: " << ssim << std::endl;
            TEST_ASSERT(ssim >= 0.95f, "Cross-backend SSIM >= 0.95 (Metal parity)");
        } else {
            std::cerr << "[TestVulkanForwardPBR_Lite] Metal reference dimensions mismatch — skipping SSIM" << std::endl;
        }
    } else {
        std::cerr << "[TestVulkanForwardPBR_Lite] Metal reference not found at "
                  << refPath << " — skipping SSIM" << std::endl;
    }

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

void RegisterVulkanForwardPBR_LiteTests() {
    auto suite = std::make_shared<TestSuite>("VulkanForwardPBR_LiteTests");
    suite->AddTestCase(TestCase("RenderSphere", TestForwardPBR_Lite_RenderSphere));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanForwardPBR_LiteTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanForwardPBR_Lite] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
