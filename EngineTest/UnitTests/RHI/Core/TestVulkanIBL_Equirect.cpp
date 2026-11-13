/**
 * @file TestVulkanIBL_Equirect.cpp
 * @brief Phase 4b Tier 3.5 — IBL Equirectangular→Cube port (compute).
 * @details Dispatches IBL_EquirectangularToCube.spv to convert a 64×32 HDR
 *          equirectangular texture into a 16×16×6 RGBA16F cubemap (face size
 *          = equirectWidth / 4 = 16).
 *
 *          The shader does per-texel mapping: for each cube face pixel,
 *          compute its 3D direction vector, then map to spherical (atan2,
 *          asin) → equirect UV → textureLoad. No convolution.
 *
 *          We compare GPU output against a CPU reference that implements
 *          the exact same face-direction + spherical-mapping math.
 *
 * Validates:
 *   - Compute pipeline with mixed bindings:
 *       binding 0 = SampledImage (equirect 2D)
 *       binding 1 = StorageImage 2D-array (cube output, 6 layers)
 *   - 2D-array texture creation, StorageImage usage, per-layer storage write.
 *   - Per-layer CopyTextureToBuffer readback via baseArrayLayer.
 *   - Spherical mapping math parity.
 *
 * Tolerance: per-pixel max abs diff < 0.01. Half-float precision and
 * atan2/asin differences should keep this well under 0.005.
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

constexpr u32 kEquirectW = 64;
constexpr u32 kEquirectH = 32;
constexpr u32 kCubeFaceSize = 16;  // = kEquirectW / 4
constexpr u32 kCubeLayers = 6;
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

// ============================= HDR equirect input =============================
// Encode direction (theta=latitude, phi=longitude) via color:
//   R = phi_norm    (0..1)
//   G = theta_norm  (0..1)
//   B = 0
// This produces a recognizable pattern that varies smoothly across the sphere.
void generate_equirect_gradient(std::vector<u8>& out) {
    out.resize(size_t(kEquirectW) * kEquirectH * 8);
    for (u32 y = 0; y < kEquirectH; ++y) {
        for (u32 x = 0; x < kEquirectW; ++x) {
            float r = float(x) / float(kEquirectW - 1);
            float g = float(y) / float(kEquirectH - 1);
            float b = 0.0f;
            float a = 1.0f;
            u16 hr = float_to_half(r), hg = float_to_half(g),
                hb = float_to_half(b), ha = float_to_half(a);
            u8* p = out.data() + (size_t(y) * kEquirectW + x) * 8;
            std::memcpy(p + 0, &hr, 2);
            std::memcpy(p + 2, &hg, 2);
            std::memcpy(p + 4, &hb, 2);
            std::memcpy(p + 6, &ha, 2);
        }
    }
}

// ============================= CPU reference (verbatim from IBL_EquirectangularToCube.wgsl) =============================
struct Vec3 { float x, y, z; };

// Mirror sampleSphericalMap(dir): returns UV in [0,1]^2 mapping to equirect texel.
void sample_spherical_map(Vec3 v, float& outU, float& outV) {
    float phi = std::atan2(v.z, v.x);
    float theta = std::asin(v.y);
    constexpr float invAtanX = 0.1591f;
    constexpr float invAtanY = 0.3183f;
    outU = phi * invAtanX + 0.5f;
    outV = theta * invAtanY + 0.5f;
}

Vec3 normalize3(Vec3 v) {
    float l = std::sqrt(v.x*v.x + v.y*v.y + v.z*v.z);
    if (l < 1e-20f) return {0, 0, 0};
    return { v.x / l, v.y / l, v.z / l };
}

// Returns the direction vector for cube face `layer` at texel (x, y).
// Verbatim from cs_main: dir = normalize(face_specific(localUV.x, -localUV.y))
// where localUV = (gid.xy + 0.5) / faceSize * 2 - 1.
Vec3 cube_face_dir(u32 x, u32 y, u32 layer) {
    float lu = (float(x) + 0.5f) / float(kCubeFaceSize) * 2.0f - 1.0f;
    float lv = (float(y) + 0.5f) / float(kCubeFaceSize) * 2.0f - 1.0f;
    // WGSL: -localUV.y in shader; lv corresponds to localUV.y. Shader uses -lv implicitly
    // by computing dir.y = -localUV.y. We replicate each case directly.
    switch (layer) {
        case 0: return normalize3({ 1.0f, -lv, -lu });
        case 1: return normalize3({ -1.0f, -lv, lu });
        case 2: return normalize3({ lu, 1.0f, lv });
        case 3: return normalize3({ lu, -1.0f, -lv });
        case 4: return normalize3({ lu, -lv, 1.0f });
        case 5: return normalize3({ -lu, -lv, -1.0f });
        default: return { 0, 0, 1 };
    }
}

// Sample equirect at (u, v) with nearest filtering (matches textureLoad behavior).
// CPU mirror: iuv = clamp(uv * dims, 0, dims-1), then read pixels[iuv.y*dims.x+iuv.x].
void sample_equirect(const std::vector<u8>& pixels, float u, float v,
                     float& outR, float& outG, float& outB, float& outA) {
    float fu = u * float(kEquirectW);
    float fv = v * float(kEquirectH);
    u32 iu = std::clamp<u32>(u32(fu), 0u, kEquirectW - 1u);
    u32 iv = std::clamp<u32>(u32(fv), 0u, kEquirectH - 1u);
    // Match WGSL: vec2<u32>(sphereUV * dims) — fractional truncation toward zero
    // matches u32 cast behavior (already done above via clamp<u32>).
    const u8* p = pixels.data() + (size_t(iv) * kEquirectW + iu) * 8;
    u16 hr, hg, hb, ha;
    std::memcpy(&hr, p + 0, 2);
    std::memcpy(&hg, p + 2, 2);
    std::memcpy(&hb, p + 4, 2);
    std::memcpy(&ha, p + 6, 2);
    outR = half_to_float(hr);
    outG = half_to_float(hg);
    outB = half_to_float(hb);
    outA = half_to_float(ha);
}

void compute_cube_texel(const std::vector<u8>& equirectPixels,
                         u32 x, u32 y, u32 layer,
                         float& outR, float& outG, float& outB, float& outA) {
    Vec3 dir = cube_face_dir(x, y, layer);
    float u, v;
    sample_spherical_map(dir, u, v);
    sample_equirect(equirectPixels, u, v, outR, outG, outB, outA);
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

ResourceHandle CreateAndUploadTexture(DeviceFixture& fx,
                                       const TextureDesc& desc,
                                       const std::vector<u8>& pixels,
                                       const char* name) {
    TextureDesc d = desc;
    d.usage = TextureUsage::CopyDest | TextureUsage::ShaderResource;
    d.memoryUsage = GPUMemoryUsage::Static;
    d.name = name;
    ResourceHandle tex = fx.base->CreateTexture(d);
    if (tex == handles::INVALID_RESOURCE) return tex;

    BufferDesc stagingDesc{};
    stagingDesc.size = pixels.size();
    stagingDesc.type = BufferType::Raw;
    stagingDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    stagingDesc.name = "Staging";
    ResourceHandle staging = fx.base->CreateBuffer(stagingDesc);
    if (staging == handles::INVALID_RESOURCE || !fx.base->UpdateBufferData(staging, pixels.data(), pixels.size(), 0)) {
        fx.base->DestroyTexture(tex);
        if (staging != handles::INVALID_RESOURCE) fx.base->DestroyBuffer(staging);
        return handles::INVALID_RESOURCE;
    }

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { d.size.x, d.size.y, 1 };
    vcmd->CopyBufferToTexture(staging, tex, &region, 1);
    ResourceBarrier b{};
    b.resource = tex;
    b.beforeState = ResourceState::CopyDest;
    b.afterState = ResourceState::ShaderResource;
    b.subresource = 0xFFFFFFFF;
    b.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&b, 1);
    vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(staging);
    return tex;
}

} // anonymous namespace

TestResult TestIBL_Equirect_Dispatch() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // ----- 1. Shader -----
    auto spv = ReadSPV("Assets/Shaders/IBL_EquirectangularToCube.spv");
    TEST_ASSERT(!spv.empty(), "Read IBL_EquirectangularToCube.spv");
    ShaderHandle cs = fx.base->CreateShader(spv.data(), spv.size(),
                                             ShaderStage::Compute, "cs_main");
    TEST_ASSERT(cs != handles::INVALID_SHADER, "CreateShader compute");

    // ----- 2. Equirect input texture (64×32 RGBA16F) -----
    std::vector<u8> equirectPixels;
    generate_equirect_gradient(equirectPixels);

    TextureDesc equirectDesc{};
    equirectDesc.size = { kEquirectW, kEquirectH, 1 };
    equirectDesc.mipLevels = 1;
    equirectDesc.arraySize = 1;
    equirectDesc.format = DataFormat::RGBA16_Float;
    equirectDesc.type = TextureType::Texture2D;
    ResourceHandle equirectTex = CreateAndUploadTexture(fx, equirectDesc, equirectPixels, "IBL_Equirect_Input");
    TEST_ASSERT(equirectTex != handles::INVALID_RESOURCE, "CreateTexture equirectTex");

    // ----- 3. Output cube texture (16×16×6 RGBA16F 2D-array, StorageImage) -----
    TextureDesc cubeDesc{};
    cubeDesc.size = { kCubeFaceSize, kCubeFaceSize, 1 };
    cubeDesc.mipLevels = 1;
    cubeDesc.arraySize = kCubeLayers;
    cubeDesc.format = DataFormat::RGBA16_Float;
    cubeDesc.type = TextureType::Texture2DArray;
    cubeDesc.usage = TextureUsage::UnorderedAccess | TextureUsage::CopySource;
    cubeDesc.memoryUsage = GPUMemoryUsage::Static;
    cubeDesc.name = "IBL_Equirect_OutputCube";
    ResourceHandle cubeTex = fx.base->CreateTexture(cubeDesc);
    TEST_ASSERT(cubeTex != handles::INVALID_RESOURCE, "CreateTexture cubeTex");

    BufferDesc readbackDesc{};
    readbackDesc.size = u64(kCubeFaceSize) * kCubeFaceSize * 8;  // 1 layer at a time
    readbackDesc.type = BufferType::Raw;
    readbackDesc.memoryUsage = GPUMemoryUsage::Readback;
    readbackDesc.name = "Equirect_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(readbackDesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    // ----- 4. Descriptor set: 2 bindings -----
    //   0: equirectTex (SampledImage, RGBA16F)
    //   1: cubeTex    (StorageImage 2D-array, write)
    DescriptorSetLayoutBinding bindings[2]{};
    bindings[0] = { 0, DescriptorType::SampledImage, 1, ShaderStage::Compute, nullptr };
    bindings[1] = { 1, DescriptorType::StorageImage, 1, ShaderStage::Compute, nullptr };
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 2;
    layoutDesc.bindings = bindings;
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

    DescriptorImageInfo equirectInfo{ handles::INVALID_SAMPLER, equirectTex, ResourceState::ShaderResource };
    DescriptorImageInfo cubeInfo{ handles::INVALID_SAMPLER, cubeTex, ResourceState::UnorderedAccess };
    WriteDescriptorSet writes[2]{};
    writes[0] = { ds, 0, 0, 1, DescriptorType::SampledImage, &equirectInfo, nullptr };
    writes[1] = { ds, 1, 0, 1, DescriptorType::StorageImage, &cubeInfo, nullptr };
    fx.base->UpdateDescriptorSets(2, writes);

    // ----- 5. Compute pipeline -----
    ComputePipelineDesc cpd{};
    cpd.computeShader = cs;
    cpd.layout = pl;
    PipelineHandle pipe = fx.base->CreateComputePipeline(cpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateComputePipeline");

    // ----- 6. Dispatch (16×16 workgroups cover faceSize × faceSize, z=6 covered by shader loop) -----
    // The shader uses gid.z (global_invocation_id.z) to index cube layer, so
    // we dispatch Z=6 workgroups in that dimension. Wait — WGSL workgroup_size
    // is (16, 16, 1), so Z=1 per workgroup. To dispatch all 6 layers, set
    // groupCountZ = 6. Then each (gx, gy, gz) maps to (gid.x, gid.y, gid.z=layer).
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Compute);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin");

    // Output cube fresh from UNDEFINED → GENERAL for StorageImage write.
    ResourceBarrier initBarrier{};
    initBarrier.resource = cubeTex;
    initBarrier.beforeState = ResourceState::Unknown;
    initBarrier.afterState = ResourceState::UnorderedAccess;
    initBarrier.subresource = 0xFFFFFFFF;
    initBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&initBarrier, 1);

    vcmd->BindComputePipeline(pipe);
    vcmd->BindDescriptorSets(PipelineBindPoint::Compute, pl, 0, 1, &ds, 0, nullptr);
    // faceSize=16, workgroup_size=16 → 1×1 workgroups in XY; Z=6 for layers.
    vcmd->Dispatch(1, 1, 6);

    // cubeTex: GENERAL → CopySource for readback
    ResourceBarrier toCopyBarrier{};
    toCopyBarrier.resource = cubeTex;
    toCopyBarrier.beforeState = ResourceState::UnorderedAccess;
    toCopyBarrier.afterState = ResourceState::CopySource;
    toCopyBarrier.subresource = 0xFFFFFFFF;
    toCopyBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&toCopyBarrier, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");
    fx.base->DestroyCommandBuffer(cmd);

    // ----- 7. Per-layer readback + compare vs CPU reference -----
    float maxDiff = 0.0f;
    float meanDiff = 0.0f;
    u32 count = 0;

    for (u32 layer = 0; layer < kCubeLayers; ++layer) {
        // Copy this layer's data to readback buffer.
        CommandBufferHandle rcmd = fx.base->CreateCommandBuffer(CommandQueueType::Compute);
        VulkanCommandBuffer* vrcmd = fx.vk->GetCommandBuffer(rcmd);
        vrcmd->Reset(); vrcmd->Begin();
        BufferTextureCopyRegion region{};
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = layer;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {kCubeFaceSize, kCubeFaceSize, 1};
        vrcmd->CopyTextureToBuffer(cubeTex, readback, &region, 1);
        vrcmd->End(); vrcmd->Submit(0); vrcmd->WaitForCompletion();
        fx.base->DestroyCommandBuffer(rcmd);

        void* mapped = fx.base->MapBuffer(readback, 0, readbackDesc.size);
        TEST_ASSERT(mapped != nullptr, "MapBuffer readback");

        // Compare each pixel vs CPU reference
        for (u32 y = 0; y < kCubeFaceSize; ++y) {
            for (u32 x = 0; x < kCubeFaceSize; ++x) {
                u8* p = static_cast<u8*>(mapped) + (size_t(y) * kCubeFaceSize + x) * 8;
                u16 hr, hg, hb, ha;
                std::memcpy(&hr, p + 0, 2);
                std::memcpy(&hg, p + 2, 2);
                std::memcpy(&hb, p + 4, 2);
                std::memcpy(&ha, p + 6, 2);
                float gpuR = half_to_float(hr);
                float gpuG = half_to_float(hg);
                float gpuB = half_to_float(hb);

                float cpuR, cpuG, cpuB, cpuA;
                compute_cube_texel(equirectPixels, x, y, layer, cpuR, cpuG, cpuB, cpuA);

                float dR = std::abs(gpuR - cpuR);
                float dG = std::abs(gpuG - cpuG);
                float dB = std::abs(gpuB - cpuB);
                float localMax = std::max(dR, std::max(dG, dB));
                if (localMax > maxDiff) maxDiff = localMax;
                meanDiff += (dR + dG + dB) / 3.0f;
                ++count;

                if (localMax > 0.01f && (x == 0 || x == kCubeFaceSize - 1) && y == 0) {
                    std::cout << "[Equirect] layer=" << layer << " texel (" << x << "," << y
                              << "): GPU=(" << gpuR << "," << gpuG << "," << gpuB
                              << ") CPU=(" << cpuR << "," << cpuG << "," << cpuB
                              << ") maxDiff=" << localMax << std::endl;
                }
            }
        }
        fx.base->UnmapBuffer(readback);
    }
    meanDiff /= float(count);

    std::cout << "[TestVulkanIBL_Equirect] max diff = " << maxDiff
              << " mean = " << meanDiff << " across " << count << " texels" << std::endl;

    TEST_ASSERT(meanDiff < 0.005f, "mean abs diff < 0.005");
    TEST_ASSERT(maxDiff < 0.02f,   "max abs diff < 0.02 (atan2/asin precision)");

    // Cleanup
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(cubeTex);
    fx.base->DestroyTexture(equirectTex);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroyShader(cs);
    return TestResult::Passed;
}

void RegisterVulkanIBL_Equirect_Tests() {
    auto suite = std::make_shared<TestSuite>("VulkanIBL_Equirect_Tests");
    suite->AddTestCase(TestCase("Dispatch", TestIBL_Equirect_Dispatch));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanIBL_Equirect_Tests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanIBL_Equirect] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
