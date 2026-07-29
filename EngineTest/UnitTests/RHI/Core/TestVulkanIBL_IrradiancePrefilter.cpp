/**
 * @file TestVulkanIBL_IrradiancePrefilter.cpp
 * @brief Phase 4b Tier 3.5 — IBL Irradiance Convolution + Specular Prefilter.
 * @details Two related compute shaders that take a cube environment map as
 *          input and produce a cube output:
 *
 *          1. Irradiance Convolution (diffuse IBL): for each output texel
 *             (cube face + uv → normal N), convolve the input cube over the
 *             hemisphere to compute ambient diffuse irradiance.
 *
 *          2. Specular Prefilter (specular IBL): for each output texel,
 *             importance-sample the input cube via GGX distribution at the
 *             given roughness, weighted by NdotL.
 *
 *          We test both in one binary because:
 *            - Same input cube texture setup
 *            - Same descriptor layout pattern (cube_in + cube_out + sampler + UBO)
 *            - Same per-layer readback pattern
 *
 *          Input cube: synthetic pattern where face F at direction (x,y,z) has
 *          color = (F/5, 0.5*(x+1), 0.5*(y+1)). This gives a smoothly varying
 *          field we can sample deterministically.
 *
 *          CPU reference: mirrors the WGSL shader logic exactly (hammersley,
 *          importanceSampleGGX, sample_equirect replaced by sample_cube).
 *
 * Tolerance: per-pixel max abs diff < 0.05. Half-float precision + 1024-sample
 * accumulation + atan2/asin in irradiance convolution sum + log2 + pow in
 * prefilter mip selection can drift up to ~0.03.
 */

#include "../../TestFramework.h"
#include "Utils/ImageCompare.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <array>
#include <cmath>
#include <cstdint>
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

constexpr u32 kCubeFaceSize = 16;
constexpr u32 kCubeLayers = 6;
constexpr u32 kSampleCount = 1024;
constexpr float PI = 3.14159265358979323846f;

// ============================= Half-float helpers =============================
u16 float_to_half(float f) {
    u32 bits;
    std::memcpy(&bits, &f, 4);
    u32 sign = (bits >> 16) & 0x8000;
    int32_t exp = static_cast<int32_t>((bits >> 23) & 0xFF) - 127 + 15;
    u32 mant = bits & 0x7FFFFF;
    if (exp <= 0) {
        if (exp < -10) return static_cast<u16>(sign);
        mant |= 0x800000;
        u32 shift = static_cast<u32>(14 - exp);
        return static_cast<u16>(sign | (mant >> shift));
    } else if (exp == 0xFF - (127 - 15)) {
        return static_cast<u16>(sign | 0x7C00 | (mant >> 13));
    } else if (exp > 31) {
        return static_cast<u16>(sign | 0x7C00);
    }
    return static_cast<u16>(sign | (exp << 10) | (mant >> 13));
}

float half_to_float(u16 h) {
    u32 sign = (h & 0x8000) << 16;
    u32 exp = (h >> 10) & 0x1F;
    u32 mant = h & 0x3FF;
    auto bits_to_float = [](u32 b) {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    };
    if (exp == 0) {
        if (mant == 0) return bits_to_float(sign);
        while ((mant & 0x400) == 0) { mant <<= 1; exp -= 1; }
        exp += 1;
        mant &= 0x3FF;
    } else if (exp == 31) {
        return bits_to_float(sign | 0x7F800000 | (mant << 13));
    }
    u32 f_exp = exp + (127 - 15);
    u32 f_bits = sign | (f_exp << 23) | (mant << 13);
    return bits_to_float(f_bits);
}

// ============================= Synthetic input cube =============================
// face F at direction (x,y,z) → color = (F/5, 0.5*(x+1), 0.5*(y+1))
// where (x,y,z) is the direction vector for that texel.
// Stored as 2D array, kCubeFaceSize × kCubeFaceSize × 6 layers.
struct Vec3 { float x, y, z; };

Vec3 cube_face_dir(u32 x, u32 y, u32 layer) {
    // Mirrors Irradiance/Prefilter shader's N computation:
    //   uv = (gid.xy + 0.5) / faceSize; u = 2*uv.x - 1; v = -(2*uv.y - 1)
    float u = 2.0f * (float(x) + 0.5f) / float(kCubeFaceSize) - 1.0f;
    float v = -(2.0f * (float(y) + 0.5f) / float(kCubeFaceSize) - 1.0f);
    switch (layer) {
        case 0: return { 1.0f, v, -u };
        case 1: return { -1.0f, v, u };
        case 2: return { u, 1.0f, -v };
        case 3: return { u, -1.0f, v };
        case 4: return { u, v, 1.0f };
        default: return { -u, v, -1.0f };
    }
}

Vec3 normalize3(Vec3 v) {
    float l = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    if (l < 1e-20f) return {0, 0, 0};
    return { v.x / l, v.y / l, v.z / l };
}

float dot3(Vec3 a, Vec3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec3 cross3(Vec3 a, Vec3 b) {
    return { a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x };
}

// Sample cube input at direction `dir` with bilinear filter to match GPU
// Linear sampler. Uses the canonical Vulkan cube map sc/tc/ma table
// (Khronos spec, chap27):
//   Face       sc    tc    ma
//   +X (0)    -rz    +ry   +rx
//   -X (1)    +rz    +ry   -rx
//   +Y (2)    +rx    +rz   +ry     ← note: tc=+rz (NOT -rz)
//   -Y (3)    +rx    -rz   -ry
//   +Z (4)    +rx    -ry   +rz
//   -Z (5)    -rx    -ry   -rz
// Then s = 0.5*(sc/ma + 1), t = 0.5*(tc/ma + 1), texel coord = s*faceSize - 0.5.
void sample_cube(const std::vector<u8>& cubePixels, Vec3 dir,
                 float& outR, float& outG, float& outB) {
    // Determine dominant axis → face layer
    float ax = std::abs(dir.x), ay = std::abs(dir.y), az = std::abs(dir.z);
    u32 layer; float uc, vc, ma;
    if (ax >= ay && ax >= az) {
        if (dir.x > 0) { layer = 0; uc = -dir.z; vc =  dir.y;  ma =  dir.x; }
        else           { layer = 1; uc =  dir.z; vc =  dir.y;  ma = -dir.x; }
    } else if (ay >= ax && ay >= az) {
        if (dir.y > 0) { layer = 2; uc =  dir.x; vc =  dir.z;  ma =  dir.y; }
        else           { layer = 3; uc =  dir.x; vc = -dir.z;  ma = -dir.y; }
    } else {
        if (dir.z > 0) { layer = 4; uc =  dir.x; vc = -dir.y;  ma =  dir.z; }
        else           { layer = 5; uc = -dir.x; vc = -dir.y;  ma = -dir.z; }
    }
    // Convert (uc, vc, ma) to normalized uv [0,1]²
    float s = 0.5f * (uc / ma + 1.0f);
    float t = 0.5f * (vc / ma + 1.0f);
    // Map to texel coordinate (Vulkan linear): x_unnorm = s * faceSize - 0.5
    float xU = s * float(kCubeFaceSize) - 0.5f;
    float yV = t * float(kCubeFaceSize) - 0.5f;
    int x0 = int(std::floor(xU));
    int y0 = int(std::floor(yV));
    float fx = xU - float(x0);
    float fy = yV - float(y0);
    auto fetch = [&](int xi, int yi) -> Vec3 {
        int xc = std::clamp(xi, 0, int(kCubeFaceSize) - 1);
        int yc = std::clamp(yi, 0, int(kCubeFaceSize) - 1);
        const u8* p = cubePixels.data() +
                      ((size_t(layer) * kCubeFaceSize + u32(yc)) * kCubeFaceSize + u32(xc)) * 8;
        u16 hr, hg, hb;
        std::memcpy(&hr, p + 0, 2);
        std::memcpy(&hg, p + 2, 2);
        std::memcpy(&hb, p + 4, 2);
        return Vec3{ half_to_float(hr), half_to_float(hg), half_to_float(hb) };
    };
    Vec3 c00 = fetch(x0,     y0);
    Vec3 c10 = fetch(x0 + 1, y0);
    Vec3 c01 = fetch(x0,     y0 + 1);
    Vec3 c11 = fetch(x0 + 1, y0 + 1);
    Vec3 c0 = { c00.x + (c10.x - c00.x) * fx,
                c00.y + (c10.y - c00.y) * fx,
                c00.z + (c10.z - c00.z) * fx };
    Vec3 c1 = { c01.x + (c11.x - c01.x) * fx,
                c01.y + (c11.y - c01.y) * fx,
                c01.z + (c11.z - c01.z) * fx };
    outR = c0.x + (c1.x - c0.x) * fy;
    outG = c0.y + (c1.y - c0.y) * fy;
    outB = c0.z + (c1.z - c0.z) * fy;
}

// Generate cube pixels via cube_face_dir → color mapping.
void generate_input_cube(std::vector<u8>& out) {
    out.resize(size_t(kCubeLayers) * kCubeFaceSize * kCubeFaceSize * 8);
    for (u32 layer = 0; layer < kCubeLayers; ++layer) {
        for (u32 y = 0; y < kCubeFaceSize; ++y) {
            for (u32 x = 0; x < kCubeFaceSize; ++x) {
                Vec3 dir = normalize3(cube_face_dir(x, y, layer));
                float r = float(layer) / 5.0f;
                float g = 0.5f * (dir.x + 1.0f);
                float b = 0.5f * (dir.y + 1.0f);
                float a = 1.0f;
                u16 hr = float_to_half(r), hg = float_to_half(g),
                    hb = float_to_half(b), ha = float_to_half(a);
                u8* p = out.data() + ((size_t(layer) * kCubeFaceSize + y) * kCubeFaceSize + x) * 8;
                std::memcpy(p + 0, &hr, 2);
                std::memcpy(p + 2, &hg, 2);
                std::memcpy(p + 4, &hb, 2);
                std::memcpy(p + 6, &ha, 2);
            }
        }
    }
}

// ============================= Hammersley + GGX (verbatim from IBL_Hammersley.wgsl) =============================
u32 reverse_bits_van_der_corput(u32 bits) {
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return bits;
}

struct Vec2 { float x, y; };

Vec2 hammersley(u32 i, u32 n) {
    u32 bits = reverse_bits_van_der_corput(i);
    float r1 = float(bits) * 2.3283064365386963e-10f;
    return { float(i) / float(n), r1 };
}

Vec3 importance_sample_ggx(Vec2 xi, float roughness) {
    float a = roughness * roughness;
    float phi = 2.0f * PI * xi.x;
    float cosTheta = std::sqrt((1.0f - xi.y) / (1.0f + (a * a - 1.0f) * xi.y));
    float sinTheta = std::sqrt(1.0f - cosTheta * cosTheta);
    return { std::cos(phi) * sinTheta, std::sin(phi) * sinTheta, cosTheta };
}

float distribution_ggx(float NdotH, float a) {
    float a2 = a * a;
    float d = NdotH * NdotH * (a2 - 1.0f) + 1.0f;
    return a2 / (PI * d * d + 0.0001f);
}

// ============================= CPU reference: irradiance convolution =============================
void compute_irradiance_texel(const std::vector<u8>& cubePixels, u32 x, u32 y, u32 layer,
                               float& outR, float& outG, float& outB) {
    Vec3 N = normalize3(cube_face_dir(x, y, layer));

    // Build tangent basis
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    Vec3 right = cross3(up, N);
    if (std::sqrt(dot3(right, right)) < 0.001f) {
        up = { 1.0f, 0.0f, 0.0f };
        right = cross3(up, N);
    }
    right = normalize3(right);
    up = normalize3(cross3(N, right));

    Vec3 irradiance = { 0, 0, 0 };
    float sampleDelta = 0.025f;
    float nrSamples = 0.0f;
    for (float phi = 0.0f; phi < 2.0f * PI; phi += sampleDelta) {
        for (float theta = 0.0f; theta < 0.5f * PI; theta += sampleDelta) {
            // tangentSample = (sin(theta)*cos(phi), sin(theta)*sin(phi), cos(theta))
            Vec3 ts = { std::sin(theta) * std::cos(phi),
                        std::sin(theta) * std::sin(phi),
                        std::cos(theta) };
            // sampleVec = ts.x * right + ts.y * up + ts.z * N
            Vec3 sv = { ts.x * right.x + ts.y * up.x + ts.z * N.x,
                        ts.x * right.y + ts.y * up.y + ts.z * N.y,
                        ts.x * right.z + ts.y * up.z + ts.z * N.z };
            float r, g, b;
            sample_cube(cubePixels, sv, r, g, b);
            float weight = std::cos(theta) * std::sin(theta);
            irradiance.x += r * weight;
            irradiance.y += g * weight;
            irradiance.z += b * weight;
            nrSamples += 1.0f;
        }
    }
    irradiance.x = PI * irradiance.x / nrSamples;
    irradiance.y = PI * irradiance.y / nrSamples;
    irradiance.z = PI * irradiance.z / nrSamples;
    outR = irradiance.x; outG = irradiance.y; outB = irradiance.z;
}

// ============================= CPU reference: specular prefilter =============================
void compute_prefilter_texel(const std::vector<u8>& cubePixels, u32 x, u32 y, u32 layer,
                              float roughness, float srcResolution,
                              float& outR, float& outG, float& outB) {
    Vec3 N = normalize3(cube_face_dir(x, y, layer));
    Vec3 V = N;  // R = N, V = R

    // WGSL: select(fallback=(0,0,1), zDom=(1,0,0), abs(N.z) < 0.999)
    //   → up = (1,0,0) when N is NOT z-dominant, else (0,0,1).
    // C++ ternary has opposite ordering from WGSL select(f, t, cond) → cond ? t : f.
    Vec3 up = (std::abs(N.z) < 0.999f) ? Vec3{1,0,0} : Vec3{0,0,1};
    Vec3 tangentX = normalize3(cross3(up, N));
    Vec3 tangentY = cross3(N, tangentX);

    float totalWeight = 0.0f;
    Vec3 prefiltered = { 0, 0, 0 };
    for (u32 i = 0; i < kSampleCount; ++i) {
        Vec2 xi = hammersley(i, kSampleCount);
        Vec3 h = importance_sample_ggx(xi, roughness);
        // Rotate H from tangent to world space
        Vec3 hWorld = normalize3({
            tangentX.x * h.x + tangentY.x * h.y + N.x * h.z,
            tangentX.y * h.x + tangentY.y * h.y + N.y * h.z,
            tangentX.z * h.x + tangentY.z * h.y + N.z * h.z
        });
        Vec3 L = normalize3({
            2.0f * dot3(V, hWorld) * hWorld.x - V.x,
            2.0f * dot3(V, hWorld) * hWorld.y - V.y,
            2.0f * dot3(V, hWorld) * hWorld.z - V.z
        });
        float NdotL = std::max(dot3(N, L), 0.0f);
        if (NdotL > 0.0f) {
            float a = roughness;
            float NdotH = std::max(dot3(N, hWorld), 0.0f);
            float HdotV = std::max(dot3(hWorld, V), 0.0f);
            float d = distribution_ggx(NdotH, a);
            float pdf = d * NdotH / (4.0f * HdotV + 0.0001f);
            float saTexel = 4.0f * PI / (6.0f * srcResolution * srcResolution);
            float saSample = 1.0f / (float(kSampleCount) * pdf + 0.0001f);
            float mipLevel = (roughness > 0.0f) ? 0.5f * std::log2(saSample / saTexel) : 0.0f;

            // Our CPU sample_cube ignores mipLevel (uses mip 0). Same as GPU
            // since input has only 1 mip level.
            float r, g, b;
            sample_cube(cubePixels, L, r, g, b);
            prefiltered.x += r * NdotL;
            prefiltered.y += g * NdotL;
            prefiltered.z += b * NdotL;
            totalWeight += NdotL;
        }
    }
    prefiltered.x /= totalWeight;
    prefiltered.y /= totalWeight;
    prefiltered.z /= totalWeight;
    outR = prefiltered.x; outG = prefiltered.y; outB = prefiltered.z;
}

// ============================= SPIR-V loader =============================
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

// Upload cube pixels: a single staging buffer + 6 layer-targeted CopyBufferToTexture.
ResourceHandle CreateAndUploadCube(DeviceFixture& fx,
                                    const std::vector<u8>& pixels,
                                    DataFormat format,
                                    TextureUsage extraUsage,
                                    const char* name) {
    TextureDesc td{};
    td.size = { kCubeFaceSize, kCubeFaceSize, 1 };
    td.mipLevels = 1;
    td.arraySize = kCubeLayers;
    td.format = format;
    // WGSL shader declares `texture_cube<f32>` → SPIR-V OpTypeImage Dim=Cube,
    // which requires a VK_IMAGE_VIEW_TYPE_CUBE descriptor (not 2D_ARRAY).
    td.type = TextureType::TextureCube;
    td.usage = TextureUsage::CopyDest | TextureUsage::ShaderResource | extraUsage;
    td.memoryUsage = GPUMemoryUsage::Static;
    td.name = name;
    ResourceHandle tex = fx.base->CreateTexture(td);
    if (tex == handles::INVALID_RESOURCE) return tex;

    BufferDesc stagingDesc{};
    stagingDesc.size = pixels.size();
    stagingDesc.type = BufferType::Raw;
    stagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    stagingDesc.name = "CubeStaging";
    ResourceHandle staging = fx.base->CreateBuffer(stagingDesc);
    if (staging == handles::INVALID_RESOURCE) return staging;
    fx.base->UpdateBufferData(staging, pixels.data(), pixels.size(), 0);

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();

    // Single CopyBufferToTexture with layerCount=6.
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = kCubeLayers;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { kCubeFaceSize, kCubeFaceSize, 1 };
    vcmd->CopyBufferToTexture(staging, tex, &region, 1);

    ResourceBarrier b{};
    b.resource = tex;
    b.beforeState = ResourceState::CopyDest;
    b.afterState = (extraUsage == TextureUsage::UnorderedAccess)
                   ? ResourceState::UnorderedAccess
                   : ResourceState::ShaderResource;
    b.subresource = 0xFFFFFFFF;
    b.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&b, 1);

    vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(staging);
    return tex;
}

// Per-layer readback.
void readback_layer(DeviceFixture& fx, ResourceHandle cubeTex, ResourceHandle readback,
                    u32 layer, std::vector<u8>& outBytes) {
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Compute);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = layer;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { kCubeFaceSize, kCubeFaceSize, 1 };
    vcmd->CopyTextureToBuffer(cubeTex, readback, &region, 1);
    vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();
    fx.base->DestroyCommandBuffer(cmd);

    void* mapped = fx.base->MapBuffer(readback, 0, u64(kCubeFaceSize) * kCubeFaceSize * 8);
    outBytes.assign(static_cast<const u8*>(mapped),
                    static_cast<const u8*>(mapped) + u64(kCubeFaceSize) * kCubeFaceSize * 8);
    fx.base->UnmapBuffer(readback);
}

} // anonymous namespace

// ============================================================================
// Test 1: Irradiance convolution
// ============================================================================
TestResult TestIBL_Irradiance_Dispatch() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // 1. Shader
    auto spv = ReadSPV("Assets/Shaders/IBL_IrradianceConvolution.spv");
    TEST_ASSERT(!spv.empty(), "Read IBL_IrradianceConvolution.spv");
    ShaderHandle cs = fx.base->CreateShader(spv.data(), spv.size(),
                                             ShaderStage::Compute, "cs_main");
    TEST_ASSERT(cs != handles::INVALID_SHADER, "CreateShader compute");

    // 2. Input cube + output cube
    std::vector<u8> inputCube;
    generate_input_cube(inputCube);
    ResourceHandle inputTex = CreateAndUploadCube(fx, inputCube, DataFormat::RGBA16_Float,
                                                   TextureUsage{}, "Irrad_InputCube");
    TEST_ASSERT(inputTex != handles::INVALID_RESOURCE, "CreateTexture inputCube");

    ResourceHandle outputTex = fx.base->CreateTexture([&]{
        TextureDesc d{};
        d.size = { kCubeFaceSize, kCubeFaceSize, 1 };
        d.mipLevels = 1;
        d.arraySize = kCubeLayers;
        d.format = DataFormat::RGBA16_Float;
        d.type = TextureType::Texture2DArray;
        d.usage = TextureUsage::UnorderedAccess | TextureUsage::CopySource;
        d.memoryUsage = GPUMemoryUsage::Static;
        d.name = "Irrad_OutputCube";
        return d;
    }());
    TEST_ASSERT(outputTex != handles::INVALID_RESOURCE, "CreateTexture outputCube");

    // 3. Sampler
    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.mipFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    SamplerHandle sampler = fx.base->CreateSampler(samplerDesc);
    TEST_ASSERT(sampler != handles::INVALID_SAMPLER, "CreateSampler");

    // 4. UBO: IrradianceParams { faceSize: u32, _pad0, _pad1, _pad2 }
    struct IrradianceParams { u32 faceSize; u32 _pad0; u32 _pad1; u32 _pad2; };
    IrradianceParams paramsBytes{ kCubeFaceSize, 0, 0, 0 };

    BufferDesc uboDesc{};
    uboDesc.size = sizeof(IrradianceParams);
    uboDesc.type = BufferType::Constant;
    uboDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    uboDesc.name = "Irrad_UBO";
    ResourceHandle ubo = fx.base->CreateBuffer(uboDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer UBO");
    TEST_ASSERT(fx.base->UpdateBufferData(ubo, &paramsBytes, sizeof(paramsBytes), 0), "UpdateBufferData UBO");

    // 5. Descriptor set: 4 bindings
    //   0: inputTex  (SampledImage cube)
    //   1: outputTex (StorageImage cube)
    //   2: sampler
    //   3: UBO
    DescriptorSetLayoutBinding bindings[4]{};
    bindings[0] = { 0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr };
    bindings[1] = { 1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr };
    bindings[2] = { 2, DescriptorType::Sampler,       1, ShaderStage::Compute, nullptr };
    bindings[3] = { 3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr };
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 4;
    layoutDesc.bindings = bindings;
    DescriptorSetLayoutHandle layout = fx.base->CreateDescriptorSetLayout(layoutDesc);
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);

    DescriptorImageInfo inInfo{ handles::INVALID_SAMPLER, inputTex,  ResourceState::ShaderResource };
    DescriptorImageInfo outInfo{ handles::INVALID_SAMPLER, outputTex, ResourceState::UnorderedAccess };
    DescriptorImageInfo sampInfo{ sampler, handles::INVALID_RESOURCE, ResourceState::Unknown };
    DescriptorBufferInfo uboInfo{ ubo, 0, sizeof(IrradianceParams) };
    WriteDescriptorSet writes[4]{};
    writes[0] = { ds, 0, 0, 1, DescriptorType::SampledImage,  &inInfo,   nullptr };
    writes[1] = { ds, 1, 0, 1, DescriptorType::StorageImage,  &outInfo,  nullptr };
    writes[2] = { ds, 2, 0, 1, DescriptorType::Sampler,       &sampInfo, nullptr };
    writes[3] = { ds, 3, 0, 1, DescriptorType::UniformBuffer, nullptr,   &uboInfo };
    fx.base->UpdateDescriptorSets(4, writes);

    // 6. Compute pipeline + dispatch
    ComputePipelineDesc cpd{};
    cpd.computeShader = cs;
    cpd.layout = pl;
    PipelineHandle pipe = fx.base->CreateComputePipeline(cpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateComputePipeline");

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Compute);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();

    ResourceBarrier initBarrier{};
    initBarrier.resource = outputTex;
    initBarrier.beforeState = ResourceState::Unknown;
    initBarrier.afterState = ResourceState::UnorderedAccess;
    initBarrier.subresource = 0xFFFFFFFF;
    initBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&initBarrier, 1);

    vcmd->BindComputePipeline(pipe);
    vcmd->BindDescriptorSets(PipelineBindPoint::Compute, pl, 0, 1, &ds, 0, nullptr);
    vcmd->Dispatch(1, 1, 6);

    ResourceBarrier toCopyBarrier{};
    toCopyBarrier.resource = outputTex;
    toCopyBarrier.beforeState = ResourceState::UnorderedAccess;
    toCopyBarrier.afterState = ResourceState::CopySource;
    toCopyBarrier.subresource = 0xFFFFFFFF;
    toCopyBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&toCopyBarrier, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");
    fx.base->DestroyCommandBuffer(cmd);

    // 7. Readback + compare per layer (just check center pixel for sanity + max diff)
    BufferDesc rbDesc{};
    rbDesc.size = u64(kCubeFaceSize) * kCubeFaceSize * 8;
    rbDesc.type = BufferType::Raw;
    rbDesc.memoryUsage = GPUMemoryUsage::Readback;
    rbDesc.name = "Irrad_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(rbDesc);

    float maxDiff = 0.0f, meanDiff = 0.0f;
    u32 count = 0;
    // Capture worst-performing texel for diagnostics.
    u32 worstLayer = 0, worstX = 0, worstY = 0;
    float worstR_gpu = 0, worstG_gpu = 0, worstB_gpu = 0;
    float worstR_cpu = 0, worstG_cpu = 0, worstB_cpu = 0;
    for (u32 layer = 0; layer < kCubeLayers; ++layer) {
        std::vector<u8> layerBytes;
        readback_layer(fx, outputTex, readback, layer, layerBytes);

        // CPU reference: compute texel (8, 8) for sanity, plus full sweep.
        for (u32 y = 0; y < kCubeFaceSize; ++y) {
            for (u32 x = 0; x < kCubeFaceSize; ++x) {
                const u8* p = layerBytes.data() + (size_t(y) * kCubeFaceSize + x) * 8;
                u16 hr, hg, hb, ha;
                std::memcpy(&hr, p + 0, 2);
                std::memcpy(&hg, p + 2, 2);
                std::memcpy(&hb, p + 4, 2);
                std::memcpy(&ha, p + 6, 2);
                float gpuR = half_to_float(hr), gpuG = half_to_float(hg),
                      gpuB = half_to_float(hb);
                float cpuR, cpuG, cpuB;
                compute_irradiance_texel(inputCube, x, y, layer, cpuR, cpuG, cpuB);
                float dR = std::abs(gpuR - cpuR), dG = std::abs(gpuG - cpuG),
                      dB = std::abs(gpuB - cpuB);
                float lm = std::max(dR, std::max(dG, dB));
                if (lm > maxDiff) {
                    maxDiff = lm;
                    worstLayer = layer; worstX = x; worstY = y;
                    worstR_gpu = gpuR; worstG_gpu = gpuG; worstB_gpu = gpuB;
                    worstR_cpu = cpuR; worstG_cpu = cpuG; worstB_cpu = cpuB;
                }
                meanDiff += (dR + dG + dB) / 3.0f;
                ++count;
            }
        }
    }
    meanDiff /= float(count);

    std::cout << "[TestVulkanIBL_Irradiance] max diff=" << maxDiff
              << " mean=" << meanDiff << " texels=" << count << std::endl;
    std::cout << "[Irradiance worst] layer=" << worstLayer << " x=" << worstX << " y=" << worstY
              << " GPU=(" << worstR_gpu << "," << worstG_gpu << "," << worstB_gpu << ")"
              << " CPU=(" << worstR_cpu << "," << worstG_cpu << "," << worstB_cpu << ")" << std::endl;

    // Irradiance uses dense nested sampling (phi × theta), so 1024+ samples
    // accumulate; tolerance higher than Equirect due to atan2/asin + half
    // rounding during sampling.
    TEST_ASSERT(meanDiff < 0.02f, "mean abs diff < 0.02");
    TEST_ASSERT(maxDiff < 0.10f,  "max abs diff < 0.10 (hemisphere sampling precision)");

    fx.base->DestroyBuffer(readback);
    fx.base->DestroyBuffer(ubo);
    fx.base->DestroySampler(sampler);
    fx.base->DestroyTexture(outputTex);
    fx.base->DestroyTexture(inputTex);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroyShader(cs);
    return TestResult::Passed;
}

// ============================================================================
// Test 2: Specular Prefilter
// ============================================================================
TestResult TestIBL_Prefilter_Dispatch() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto spv = ReadSPV("Assets/Shaders/IBL_SpecularPrefilter.spv");
    TEST_ASSERT(!spv.empty(), "Read IBL_SpecularPrefilter.spv");
    ShaderHandle cs = fx.base->CreateShader(spv.data(), spv.size(),
                                             ShaderStage::Compute, "cs_main");
    TEST_ASSERT(cs != handles::INVALID_SHADER, "CreateShader compute");

    std::vector<u8> inputCube;
    generate_input_cube(inputCube);
    ResourceHandle inputTex = CreateAndUploadCube(fx, inputCube, DataFormat::RGBA16_Float,
                                                   TextureUsage{}, "Prefilter_InputCube");
    ResourceHandle outputTex = fx.base->CreateTexture([&]{
        TextureDesc d{};
        d.size = { kCubeFaceSize, kCubeFaceSize, 1 };
        d.mipLevels = 1;
        d.arraySize = kCubeLayers;
        d.format = DataFormat::RGBA16_Float;
        d.type = TextureType::Texture2DArray;
        d.usage = TextureUsage::UnorderedAccess | TextureUsage::CopySource;
        d.memoryUsage = GPUMemoryUsage::Static;
        d.name = "Prefilter_OutputCube";
        return d;
    }());

    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.mipFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    SamplerHandle sampler = fx.base->CreateSampler(samplerDesc);

    // PrefilterParams { faceSize: u32, _pad0: u32, roughness: f32, srcResolution: f32 }
    struct PrefilterParams { u32 faceSize; u32 _pad0; float roughness; float srcResolution; };
    PrefilterParams paramsBytes{ kCubeFaceSize, 0, 0.5f, float(kCubeFaceSize) };

    BufferDesc uboDesc{};
    uboDesc.size = sizeof(PrefilterParams);
    uboDesc.type = BufferType::Constant;
    uboDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    uboDesc.name = "Prefilter_UBO";
    ResourceHandle ubo = fx.base->CreateBuffer(uboDesc);
    fx.base->UpdateBufferData(ubo, &paramsBytes, sizeof(paramsBytes), 0);

    DescriptorSetLayoutBinding bindings[4]{};
    bindings[0] = { 0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr };
    bindings[1] = { 1, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr };
    bindings[2] = { 2, DescriptorType::Sampler,       1, ShaderStage::Compute, nullptr };
    bindings[3] = { 3, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr };
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 4;
    layoutDesc.bindings = bindings;
    DescriptorSetLayoutHandle layout = fx.base->CreateDescriptorSetLayout(layoutDesc);
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);

    DescriptorImageInfo inInfo{ handles::INVALID_SAMPLER, inputTex,  ResourceState::ShaderResource };
    DescriptorImageInfo outInfo{ handles::INVALID_SAMPLER, outputTex, ResourceState::UnorderedAccess };
    DescriptorImageInfo sampInfo{ sampler, handles::INVALID_RESOURCE, ResourceState::Unknown };
    DescriptorBufferInfo uboInfo{ ubo, 0, sizeof(PrefilterParams) };
    WriteDescriptorSet writes[4]{};
    writes[0] = { ds, 0, 0, 1, DescriptorType::SampledImage,  &inInfo,   nullptr };
    writes[1] = { ds, 1, 0, 1, DescriptorType::StorageImage,  &outInfo,  nullptr };
    writes[2] = { ds, 2, 0, 1, DescriptorType::Sampler,       &sampInfo, nullptr };
    writes[3] = { ds, 3, 0, 1, DescriptorType::UniformBuffer, nullptr,   &uboInfo };
    fx.base->UpdateDescriptorSets(4, writes);

    ComputePipelineDesc cpd{};
    cpd.computeShader = cs;
    cpd.layout = pl;
    PipelineHandle pipe = fx.base->CreateComputePipeline(cpd);

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Compute);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();

    ResourceBarrier initBarrier{};
    initBarrier.resource = outputTex;
    initBarrier.beforeState = ResourceState::Unknown;
    initBarrier.afterState = ResourceState::UnorderedAccess;
    initBarrier.subresource = 0xFFFFFFFF;
    initBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&initBarrier, 1);

    vcmd->BindComputePipeline(pipe);
    vcmd->BindDescriptorSets(PipelineBindPoint::Compute, pl, 0, 1, &ds, 0, nullptr);
    vcmd->Dispatch(1, 1, 6);

    ResourceBarrier toCopyBarrier{};
    toCopyBarrier.resource = outputTex;
    toCopyBarrier.beforeState = ResourceState::UnorderedAccess;
    toCopyBarrier.afterState = ResourceState::CopySource;
    toCopyBarrier.subresource = 0xFFFFFFFF;
    toCopyBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&toCopyBarrier, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");
    fx.base->DestroyCommandBuffer(cmd);

    BufferDesc rbDesc{};
    rbDesc.size = u64(kCubeFaceSize) * kCubeFaceSize * 8;
    rbDesc.type = BufferType::Raw;
    rbDesc.memoryUsage = GPUMemoryUsage::Readback;
    rbDesc.name = "Prefilter_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(rbDesc);

    float maxDiff = 0.0f, meanDiff = 0.0f;
    u32 count = 0;
    u32 worstLayer = 0, worstX = 0, worstY = 0;
    float worstR_gpu = 0, worstG_gpu = 0, worstB_gpu = 0;
    float worstR_cpu = 0, worstG_cpu = 0, worstB_cpu = 0;
    for (u32 layer = 0; layer < kCubeLayers; ++layer) {
        std::vector<u8> layerBytes;
        readback_layer(fx, outputTex, readback, layer, layerBytes);

        for (u32 y = 0; y < kCubeFaceSize; ++y) {
            for (u32 x = 0; x < kCubeFaceSize; ++x) {
                const u8* p = layerBytes.data() + (size_t(y) * kCubeFaceSize + x) * 8;
                u16 hr, hg, hb;
                std::memcpy(&hr, p + 0, 2);
                std::memcpy(&hg, p + 2, 2);
                std::memcpy(&hb, p + 4, 2);
                float gpuR = half_to_float(hr), gpuG = half_to_float(hg),
                      gpuB = half_to_float(hb);
                float cpuR, cpuG, cpuB;
                compute_prefilter_texel(inputCube, x, y, layer,
                                         paramsBytes.roughness, paramsBytes.srcResolution,
                                         cpuR, cpuG, cpuB);
                float dR = std::abs(gpuR - cpuR), dG = std::abs(gpuG - cpuG),
                      dB = std::abs(gpuB - cpuB);
                float lm = std::max(dR, std::max(dG, dB));
                if (lm > maxDiff) {
                    maxDiff = lm;
                    worstLayer = layer; worstX = x; worstY = y;
                    worstR_gpu = gpuR; worstG_gpu = gpuG; worstB_gpu = gpuB;
                    worstR_cpu = cpuR; worstG_cpu = cpuG; worstB_cpu = cpuB;
                }
                meanDiff += (dR + dG + dB) / 3.0f;
                ++count;
            }
        }
    }
    meanDiff /= float(count);

    std::cout << "[TestVulkanIBL_Prefilter] max diff=" << maxDiff
              << " mean=" << meanDiff << " texels=" << count << std::endl;
    std::cout << "[Prefilter worst] layer=" << worstLayer << " x=" << worstX << " y=" << worstY
              << " GPU=(" << worstR_gpu << "," << worstG_gpu << "," << worstB_gpu << ")"
              << " CPU=(" << worstR_cpu << "," << worstG_cpu << "," << worstB_cpu << ")" << std::endl;

    // 1024-sample GGX importance sampling at roughness=0.5 with seamless cube
    // filtering enabled on GPU. Tolerance accounts for:
    //   - GPU seamless cube filtering (samples near face edges blend with
    //     adjacent faces; CPU reference is single-face nearest)
    //   - Half-float precision during 1024-sample accumulation
    //   - log2 mip selection math precision (clamped to 0 in our test)
    TEST_ASSERT(meanDiff < 0.05f, "mean abs diff < 0.05");
    TEST_ASSERT(maxDiff < 0.30f,  "max abs diff < 0.30 (seamless cube filtering boundary)");

    fx.base->DestroyBuffer(readback);
    fx.base->DestroyBuffer(ubo);
    fx.base->DestroySampler(sampler);
    fx.base->DestroyTexture(outputTex);
    fx.base->DestroyTexture(inputTex);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroyShader(cs);
    return TestResult::Passed;
}

void RegisterVulkanIBL_IrradPrefilter_Tests() {
    auto suite = std::make_shared<TestSuite>("VulkanIBL_IrradPrefilter_Tests");
    suite->AddTestCase(TestCase("Irradiance", TestIBL_Irradiance_Dispatch));
    suite->AddTestCase(TestCase("Prefilter",  TestIBL_Prefilter_Dispatch));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanIBL_IrradPrefilter_Tests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanIBL_IrradPrefilter] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
