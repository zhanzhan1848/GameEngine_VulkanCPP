/**
 * @file TestMetalSimplePBR.cpp
 * @brief Phase 4b Tier 2 — Metal raw RHI PBR parity test (REVISED).
 * @details Mirrors TestVulkanSimplePBR.cpp on the Metal backend. Renders a sphere
 *          with an embedded MSL SimplePBR shader via raw Metal RHI (vertex/index
 *          buffers, 3 UBOs, albedo texture, 3 descriptor sets, graphics pipeline)
 *          into a 64×64 RGBA16F render target, readback, tonemap to RGBA8, save
 *          as sphere_metal.png. This PNG is the parity reference for the Vulkan
 *          test's SSIM check.
 *
 * Metal binding convention:
 *   - Vertex array:   [[buffer(20)]]  (avoids collision with PerObjectData at slot 0)
 *   - GlobalShaderData:           [[buffer(11)]] (set 0, binding 11)
 *   - DirectionalLightParameters: [[buffer(12)]] (set 0, binding 12)
 *   - PerObjectData:              [[buffer(0)]]  (set 1, binding 0)
 *   - albedoMap:                  [[texture(0)]] + [[sampler(0)]] (set 2, binding 0)
 *
 * Layout matches C++ struct truth (RHIShaderCommon.h):
 *   GlobalShaderData           = 480 B
 *   DirectionalLightParameters = 304 B
 *   PerObjectData              = 400 B
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
#include <array>

using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;
namespace et = EngineTest;

namespace {

// ============================= Embedded MSL shader source =============================
static const char* kVertexShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GlobalShaderData {
    float4x4 view;                    //   0- 63
    float4x4 projection;              //  64-127
    float4x4 invProjection;           // 128-191
    float4x4 viewProjection;          // 192-255
    float4x4 previousViewProjection;  // 256-319
    float4x4 invViewProjection;       // 320-383
    float4 cameraPositionAndViewWidth;    // 384-399
    float4 cameraDirectionAndViewHeight;  // 400-415
    uint4  numDirNumPunctualDelta;    // 416-431
    uint4  renderModeEnableFlags;     // 432-447
    float2 jitterOffset;              // 448-455
    float  debug_directLightBoost;    // 456-459
    float  debug_iblStrength;         // 460-463
    float  debug_ddgiIndirectWeight;  // 464-467
    float  debug_exposure;            // 468-471
    float2 _pad_after_debug;          // 472-479
};

struct PerObjectData {
    float4x4 world;                   //   0- 63
    float4x4 invWorld;                //  64-127
    float4x4 worldViewProjection;     // 128-191
    float4x4 prevWorldViewProjection; // 192-255
    float4   sh_coeffs[9];            // 256-399
};

struct VertexIn {
    packed_float3 position;  // 12B
    packed_float3 normal;    // 12B
    float2         uv;       //  8B
};

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float2 uv;
};

vertex VertexOut vertexMain(
    uint vid [[vertex_id]],
    constant GlobalShaderData& global [[buffer(11)]],
    constant PerObjectData&    object [[buffer(0)]],
    constant VertexIn*         vertices [[buffer(20)]]
) {
    VertexIn v = vertices[vid];
    VertexOut out;
    float4 worldPos = object.world * float4(float3(v.position), 1.0);
    out.worldPos    = worldPos.xyz;
    out.worldNormal = normalize((object.world * float4(float3(v.normal), 0.0)).xyz);
    out.uv          = v.uv;
    out.position    = global.viewProjection * worldPos;
    return out;
}
)MSL";

static const char* kFragmentShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct GlobalShaderData {
    float4x4 view;
    float4x4 projection;
    float4x4 invProjection;
    float4x4 viewProjection;
    float4x4 previousViewProjection;
    float4x4 invViewProjection;
    float4 cameraPositionAndViewWidth;
    float4 cameraDirectionAndViewHeight;
    uint4  numDirNumPunctualDelta;
    uint4  renderModeEnableFlags;
    float2 jitterOffset;
    float  debug_directLightBoost;
    float  debug_iblStrength;
    float  debug_ddgiIndirectWeight;
    float  debug_exposure;
    float2 _pad_after_debug;
};

struct DirectionalLightParameters {
    float4x4 viewProjections[4];
    float4   splits;
    float4   directionAndIntensity;  // xyz=dir, w=intensity
    float4   colorAndShadow;         // rgb=color, a=shadow
};

struct VertexOut {
    float4 position [[position]];
    float3 worldPos;
    float3 worldNormal;
    float2 uv;
};

fragment float4 fragmentMain(
    VertexOut in [[stage_in]],
    constant GlobalShaderData&           global [[buffer(11)]],
    constant DirectionalLightParameters& light  [[buffer(12)]],
    texture2d<float>                     albedoMap [[texture(0)]],
    sampler                              albedoSampler [[sampler(0)]]
) {
    float3 albedo = albedoMap.sample(albedoSampler, in.uv).rgb;
    float3 N      = normalize(in.worldNormal);

    // WGSL convention: direction points AWAY from light source. Negate to get L.
    float3 L       = normalize(-light.directionAndIntensity.xyz);
    float  NdotL   = max(dot(N, L), 0.0);

    float3 lightColor = light.colorAndShadow.rgb * light.directionAndIntensity.w;
    float3 ambient    = float3(0.03) * albedo;
    float3 lit        = albedo * lightColor * NdotL;

    float3 color = ambient + lit;

    // Reinhard tonemap + sRGB gamma (mirrors SimplePBR.frag).
    color = color / (1.0 + color);
    color = pow(color, float3(1.0 / 2.2));

    return float4(color, 1.0);
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
    m.columns[0][0] = xaxis[0]; m.columns[0][1] = yaxis[0]; m.columns[0][2] = zaxis[0]; m.columns[0][3] = 0.0f;
    m.columns[1][0] = xaxis[1]; m.columns[1][1] = yaxis[1]; m.columns[1][2] = zaxis[1]; m.columns[1][3] = 0.0f;
    m.columns[2][0] = xaxis[2]; m.columns[2][1] = yaxis[2]; m.columns[2][2] = zaxis[2]; m.columns[2][3] = 0.0f;
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

// ============================= UBO blobs =============================
struct GlobalShaderDataBytes {
    static constexpr u32 kSize = 480;
    u8 bytes[kSize];
    void Fill(const m4x4& view, const m4x4& proj) {
        std::memset(bytes, 0, kSize);
        std::memcpy(bytes + 0,   &view, 64);
        std::memcpy(bytes + 64,  &proj, 64);
        m4x4 vp = multiply_m4x4(proj, view);
        std::memcpy(bytes + 192, &vp, 64);
        u32 numDir = 1; u32 numPunct = 0;
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

struct DirectionalLightBytes {
    static constexpr u32 kSize = 304;
    u8 bytes[kSize];
    void Fill(v3 dir, float intensity, v3 color, float shadowEnabled) {
        std::memset(bytes, 0, kSize);
        for (int i = 0; i < 4; ++i) {
            m4x4 I = make_identity_m4x4();
            std::memcpy(bytes + i * 64, &I, 64);
        }
        std::memcpy(bytes + 272, &dir, 12);
        std::memcpy(bytes + 284, &intensity, 4);
        std::memcpy(bytes + 288, &color, 12);
        std::memcpy(bytes + 300, &shadowEnabled, 4);
    }
};

// ============================= Constants =============================
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
constexpr u32 kVertexBufferSlot = 20;  // avoids collision with PerObjectData at slot 0

} // anonymous namespace

// ============================= Main test =============================
TestResult TestSimplePBR_RenderSphere_Metal() {
    DeviceDesc deviceDesc;
    deviceDesc.platform = RHIPlatform::Metal;
    deviceDesc.enableDebug = true;

    MetalDevice device(deviceDesc);
    if (!device.Initialize()) {
        std::cerr << "[TestMetalSimplePBR] MetalDevice::Initialize failed" << std::endl;
        return TestResult::Failed;
    }

    // ----- 1. Shaders (from embedded source) -----
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
    vbufDesc.vertex.vertexStride = 32;
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

    // ----- 3. UBOs -----
    GlobalShaderDataBytes globalBytes;
    m4x4 view = make_view_m4x4(v3{0.0f, 0.0f, 3.0f}, v3{0.0f, 0.0f, 0.0f}, v3{0.0f, 1.0f, 0.0f});
    m4x4 proj = make_perspective_m4x4(0.785398f /* 45° */, 1.0f, 0.1f, 100.0f);
    globalBytes.Fill(view, proj);

    BufferDesc gbufDesc{};
    gbufDesc.size = GlobalShaderDataBytes::kSize;
    gbufDesc.type = BufferType::Constant;
    gbufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    gbufDesc.name = "GlobalUBO";
    ResourceHandle gbuf = device.CreateBuffer(gbufDesc);
    TEST_ASSERT(gbuf != handles::INVALID_RESOURCE, "CreateBuffer GlobalUBO");
    TEST_ASSERT(device.UpdateBufferData(gbuf, globalBytes.bytes, GlobalShaderDataBytes::kSize, 0),
                "UpdateBufferData GlobalUBO");

    DirectionalLightBytes lightBytes;
    // Same direction as Vulkan test: dir=(0,1,0). Metal framebuffer Y is NOT flipped
    // relative to NDC (Metal uses top-left origin viewport by default), so world +Y
    // appears at framebuffer TOP. L = -dir = (0,-1,0); surface at world +Y has normal
    // (0,1,0), N·L = -1 → UNLIT; surface at world -Y has normal (0,-1,0), N·L = +1 → LIT.
    // World -Y is at framebuffer BOTTOM on Metal → framebuffer BOTTOM is lit. (Opposite
    // hemisphere from Vulkan due to Y-flip difference.)
    lightBytes.Fill(v3{0.0f, 1.0f, 0.0f}, 2.0f, v3{1.0f, 1.0f, 1.0f}, 0.0f);

    BufferDesc lbufDesc{};
    lbufDesc.size = DirectionalLightBytes::kSize;
    lbufDesc.type = BufferType::Constant;
    lbufDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    lbufDesc.name = "LightUBO";
    ResourceHandle lbuf = device.CreateBuffer(lbufDesc);
    TEST_ASSERT(lbuf != handles::INVALID_RESOURCE, "CreateBuffer LightUBO");
    TEST_ASSERT(device.UpdateBufferData(lbuf, lightBytes.bytes, DirectionalLightBytes::kSize, 0),
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
    ResourceHandle obuf = device.CreateBuffer(obufDesc);
    TEST_ASSERT(obuf != handles::INVALID_RESOURCE, "CreateBuffer ObjectUBO");
    TEST_ASSERT(device.UpdateBufferData(obuf, objectBytes.bytes, PerObjectDataBytes::kSize, 0),
                "UpdateBufferData ObjectUBO");

    // ----- 4. Albedo texture (4×4 grey) — staging buffer + CopyBufferToTexture (mirrors Vulkan path) -----
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
    ResourceHandle staging = device.CreateBuffer(stagingDesc);
    TEST_ASSERT(staging != handles::INVALID_RESOURCE, "CreateBuffer staging");
    TEST_ASSERT(device.UpdateBufferData(staging, albedoPixels.data(), albedoPixels.size(), 0),
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
    ResourceHandle albedoTex = device.CreateTexture(texDesc);
    TEST_ASSERT(albedoTex != handles::INVALID_RESOURCE, "CreateTexture albedo");

    {
        CommandBufferHandle stagingCmd = device.CreateCommandBuffer(CommandQueueType::Graphics);
        TEST_ASSERT(stagingCmd != handles::INVALID_COMMAND_BUFFER, "CreateCommandBuffer staging");
        MetalCommandBuffer* smcmd = device.GetCommandBuffer(stagingCmd);
        TEST_ASSERT(smcmd->Reset(), "Reset staging");
        TEST_ASSERT(smcmd->Begin(), "Begin staging");
        BufferTextureCopyRegion region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {kAlbedoSize, kAlbedoSize, 1};
        smcmd->CopyBufferToTexture(staging, albedoTex, &region, 1);
        TEST_ASSERT(smcmd->End(), "End staging");
        TEST_ASSERT(smcmd->Submit(0), "Submit staging");
        TEST_ASSERT(smcmd->WaitForCompletion(), "WaitForCompletion staging");
        device.DestroyCommandBuffer(stagingCmd);
    }

    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.mipFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Wrap;
    samplerDesc.addressV = TextureAddressMode::Wrap;
    samplerDesc.maxAnisotropy = 1;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    SamplerHandle sampler = device.CreateSampler(samplerDesc);
    TEST_ASSERT(sampler != handles::INVALID_SAMPLER, "CreateSampler");

    // ----- 5. Descriptor set layouts -----
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
    DescriptorSetLayoutHandle set0Layout = device.CreateDescriptorSetLayout(set0Desc);
    TEST_ASSERT(set0Layout != handles::INVALID_RESOURCE, "CreateDescriptorSetLayout set0");

    DescriptorSetLayoutBinding set1Binding;
    set1Binding.binding = kSet1_Bind_Object;
    set1Binding.descriptorType = DescriptorType::UniformBuffer;
    set1Binding.descriptorCount = 1;
    set1Binding.stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
    DescriptorSetLayoutDesc set1Desc{};
    set1Desc.bindingCount = 1;
    set1Desc.bindings = &set1Binding;
    DescriptorSetLayoutHandle set1Layout = device.CreateDescriptorSetLayout(set1Desc);
    TEST_ASSERT(set1Layout != handles::INVALID_RESOURCE, "CreateDescriptorSetLayout set1");

    DescriptorSetLayoutBinding set2Binding;
    set2Binding.binding = kSet2_Bind_Albedo;
    set2Binding.descriptorType = DescriptorType::CombinedImageSampler;
    set2Binding.descriptorCount = 1;
    set2Binding.stageFlags = ShaderStage::Pixel;
    DescriptorSetLayoutDesc set2Desc{};
    set2Desc.bindingCount = 1;
    set2Desc.bindings = &set2Binding;
    DescriptorSetLayoutHandle set2Layout = device.CreateDescriptorSetLayout(set2Desc);
    TEST_ASSERT(set2Layout != handles::INVALID_RESOURCE, "CreateDescriptorSetLayout set2");

    DescriptorSetLayoutHandle sets[3] = { set0Layout, set1Layout, set2Layout };
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 3;
    plDesc.setLayouts = sets;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = device.CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    // ----- 6. Descriptor sets -----
    DescriptorSetDesc ds0Desc{}; ds0Desc.layout = set0Layout;
    DescriptorSetHandle ds0 = device.CreateDescriptorSet(ds0Desc);
    TEST_ASSERT(ds0 != handles::INVALID_RESOURCE, "CreateDescriptorSet set0");

    DescriptorSetDesc ds1Desc{}; ds1Desc.layout = set1Layout;
    DescriptorSetHandle ds1 = device.CreateDescriptorSet(ds1Desc);
    TEST_ASSERT(ds1 != handles::INVALID_RESOURCE, "CreateDescriptorSet set1");

    DescriptorSetDesc ds2Desc{}; ds2Desc.layout = set2Layout;
    DescriptorSetHandle ds2 = device.CreateDescriptorSet(ds2Desc);
    TEST_ASSERT(ds2 != handles::INVALID_RESOURCE, "CreateDescriptorSet set2");

    DescriptorBufferInfo globalInfo{};  globalInfo.buffer = gbuf;  globalInfo.offset = 0;  globalInfo.range = GlobalShaderDataBytes::kSize;
    DescriptorBufferInfo lightInfo{};   lightInfo.buffer = lbuf;   lightInfo.offset = 0;   lightInfo.range = DirectionalLightBytes::kSize;
    WriteDescriptorSet set0Writes[2];
    set0Writes[0].dstSet = ds0; set0Writes[0].dstBinding = kSet0_Bind_Global;
    set0Writes[0].dstArrayElement = 0; set0Writes[0].descriptorCount = 1;
    set0Writes[0].descriptorType = DescriptorType::UniformBuffer; set0Writes[0].bufferInfo = &globalInfo;
    set0Writes[1].dstSet = ds0; set0Writes[1].dstBinding = kSet0_Bind_Light;
    set0Writes[1].dstArrayElement = 0; set0Writes[1].descriptorCount = 1;
    set0Writes[1].descriptorType = DescriptorType::UniformBuffer; set0Writes[1].bufferInfo = &lightInfo;
    device.UpdateDescriptorSets(2, set0Writes);

    DescriptorBufferInfo objectInfo{}; objectInfo.buffer = obuf; objectInfo.offset = 0; objectInfo.range = PerObjectDataBytes::kSize;
    WriteDescriptorSet set1Write;
    set1Write.dstSet = ds1; set1Write.dstBinding = kSet1_Bind_Object;
    set1Write.dstArrayElement = 0; set1Write.descriptorCount = 1;
    set1Write.descriptorType = DescriptorType::UniformBuffer; set1Write.bufferInfo = &objectInfo;
    device.UpdateDescriptorSets(1, &set1Write);

    DescriptorImageInfo albedoInfo{};
    albedoInfo.sampler = sampler;
    albedoInfo.imageView = albedoTex;
    albedoInfo.imageLayout = ResourceState::ShaderResource;
    WriteDescriptorSet set2Write;
    set2Write.dstSet = ds2; set2Write.dstBinding = kSet2_Bind_Albedo;
    set2Write.dstArrayElement = 0; set2Write.descriptorCount = 1;
    set2Write.descriptorType = DescriptorType::CombinedImageSampler; set2Write.imageInfo = &albedoInfo;
    device.UpdateDescriptorSets(1, &set2Write);

    // ----- 7. Pipeline -----
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    // Metal vertex fetch is manual via [[buffer(20)]]; no vertex descriptor needed.
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::Back;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RGBA16_Float;
    gpd.depthStencilFormat = DataFormat::Unknown;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = device.CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline");

    // ----- 8. Render target + readback buffer -----
    TextureDesc rtDesc{};
    rtDesc.size = { kW, kH, 1 };
    rtDesc.mipLevels = 1;
    rtDesc.arraySize = 1;
    rtDesc.format = DataFormat::RGBA16_Float;
    rtDesc.type = TextureType::Texture2D;
    rtDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    rtDesc.memoryUsage = GPUMemoryUsage::Static;
    rtDesc.name = "SimplePBR_RT";
    ResourceHandle rt = device.CreateTexture(rtDesc);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture RT");

    BufferDesc readbackDesc{};
    readbackDesc.size = u64(kW) * kH * 8;  // RGBA16F = 8 bytes/pixel
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "SimplePBR_Readback";
    ResourceHandle readback = device.CreateBuffer(readbackDesc);
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

    CommandBufferHandle cmd = device.CreateCommandBuffer(CommandQueueType::Graphics);
    TEST_ASSERT(cmd != handles::INVALID_COMMAND_BUFFER, "CreateCommandBuffer");
    MetalCommandBuffer* mcmd = device.GetCommandBuffer(cmd);
    TEST_ASSERT(mcmd != nullptr, "GetCommandBuffer");
    TEST_ASSERT(mcmd->Reset(), "Reset");
    TEST_ASSERT(mcmd->Begin(), "Begin");
    mcmd->BeginRenderPass(rpd);
    mcmd->BindGraphicsPipeline(pipe);

    DescriptorSetHandle setsToBind[3] = { ds0, ds1, ds2 };
    mcmd->BindDescriptorSets(PipelineBindPoint::Graphics, pl, 0, 3, setsToBind, 0, nullptr);

    ResourceHandle vbArr[1] = { vb };
    u64 vbOffsets[1] = { 0 };
    mcmd->BindVertexBuffers(kVertexBufferSlot, 1, vbArr, vbOffsets);
    mcmd->BindIndexBuffer(ib, DataFormat::R32_UInt, 0);

    mcmd->DrawIndexed(sphere.indexCount, 0, 0, 1, 0);
    mcmd->EndRenderPass();

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

    // ----- 10. Readback + analyze -----
    void* mapped = device.MapBuffer(readback, 0, readbackDesc.size);
    TEST_ASSERT(mapped != nullptr, "MapBuffer readback");

    std::vector<u8> rgba8(size_t(kW) * kH * 4);
    et::RGBA16FToRGBA8(static_cast<const u8*>(mapped), rgba8.data(), kW, kH);

    device.UnmapBuffer(readback);

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
    std::cout << "[TestMetalSimplePBR] non-zero pixels: " << nonZero << " / " << (kW * kH) << std::endl;
    std::cout << "[TestMetalSimplePBR] mean luminance (sphere pixels): " << meanLum << std::endl;
    std::cout << "[TestMetalSimplePBR] center pixel RGBA = ("
              << int(centerR) << "," << int(centerG) << "," << int(centerB) << ",255)" << std::endl;

    const char* outPath = "sphere_metal.png";
    if (!et::SavePNG(outPath, rgba8.data(), kW, kH)) {
        std::cerr << "[TestMetalSimplePBR] SavePNG failed" << std::endl;
    } else {
        std::cout << "[TestMetalSimplePBR] saved " << outPath << std::endl;
    }

    TEST_ASSERT(nonZero > (kW * kH) / 3, "Sphere should cover > 1/3 of frame");
    TEST_ASSERT(nonZero < (kW * kH * 9) / 10, "Sphere should not fill frame");
    TEST_ASSERT(meanLum > 0.05, "Mean luminance over sphere pixels > 0.05");

    // Cleanup
    device.DestroyCommandBuffer(cmd);
    device.DestroyBuffer(readback);
    device.DestroyTexture(rt);
    device.DestroyPipeline(pipe);
    device.DestroyPipelineLayout(pl);
    device.DestroyDescriptorSet(ds2);
    device.DestroyDescriptorSet(ds1);
    device.DestroyDescriptorSet(ds0);
    device.DestroyDescriptorSetLayout(set2Layout);
    device.DestroyDescriptorSetLayout(set1Layout);
    device.DestroyDescriptorSetLayout(set0Layout);
    device.DestroySampler(sampler);
    device.DestroyTexture(albedoTex);
    device.DestroyBuffer(staging);
    device.DestroyBuffer(obuf);
    device.DestroyBuffer(lbuf);
    device.DestroyBuffer(gbuf);
    device.DestroyBuffer(ib);
    device.DestroyBuffer(vb);
    device.DestroyShader(fs);
    device.DestroyShader(vs);
    device.Shutdown();
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("MetalSimplePBRTests");
    suite->AddTestCase(TestCase("RenderSphere", TestSimplePBR_RenderSphere_Metal));
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    return 0;
}

#else // !__APPLE__

#include <iostream>

int main() {
    std::cout << "[TestMetalSimplePBR] Not Apple platform — no-op." << std::endl;
    return 0;
}

#endif
