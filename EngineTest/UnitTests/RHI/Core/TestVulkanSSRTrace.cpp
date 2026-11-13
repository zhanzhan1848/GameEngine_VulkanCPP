/**
 * @file TestVulkanSSRTrace.cpp
 * @brief Phase 4b Tier 3.6 — SSR Trace smoke test (HZB ray march).
 * @details SSRTrace.wgsl is a 128-step Hi-Z ray marcher. Full CPU parity is
 *          unwieldy (and the algorithm itself has documented fragility at
 *          grazing angles — see shader comments on grazingFade). This smoke
 *          test verifies the lower bar:
 *            - Shader compiles and all 5 bindings link (depth + HZB + color
 *              + storage image + UBO mat4×2 + vec4s)
 *            - Dispatch completes without validation errors
 *            - Output is finite (no NaN/Inf) — catches silent layout drift
 *
 *          Scene: 32×32 depth with x-gradient (0.30–0.55), matching HZB
 *          (single-mip R32F, identical values), RGBA16F color gradient.
 *          Rays from the camera-facing side of the gradient slope can
 *          plausibly hit the same surface — strength > 0 somewhere is a
 *          bonus but not required for the smoke test.
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

#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace primal::math;
using namespace Engine::Test;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

constexpr u32 kW = 32;
constexpr u32 kH = 32;
constexpr u32 kHalfW = kW / 2;
constexpr u32 kHalfH = kH / 2;
constexpr float kNear = 0.5f;
constexpr float kFar  = 10.0f;

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

// 2D diagonal depth gradient: d(x, y) = 0.30 + 0.20 * ((x+y)/(W+H-2)).
// This produces non-degenerate view normals (both x and y components) so
// reflection rays actually have somewhere to go.
void make_depth(std::vector<u8>& out) {
    out.resize(size_t(kW) * kH * 4);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            float d = 0.30f + 0.20f * (float(x + y) / float(kW + kH - 2));
            u8* p = out.data() + (size_t(y) * kW + x) * 4;
            std::memcpy(p, &d, 4);
        }
    }
}

// HZB texture: same as depth but stored as R32_FLOAT (single-mip, no pyramid build).
// Smoke test only — shader reads mip 1+ via clamp-to-edge semantics.
void make_hzb(std::vector<u8>& out) {
    out.resize(size_t(kW) * kH * 4);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            float d = 0.30f + 0.20f * (float(x + y) / float(kW + kH - 2));
            u8* p = out.data() + (size_t(y) * kW + x) * 4;
            std::memcpy(p, &d, 4);
        }
    }
}

// HDR color gradient: (x/W, y/H, 0.5, 1.0) in RGBA16F.
void make_color(std::vector<u8>& out) {
    out.resize(size_t(kW) * kH * 8);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            float r = float(x) / float(kW - 1);
            float g = float(y) / float(kH - 1);
            float b = 0.5f;
            float a = 1.0f;
            u8* p = out.data() + (size_t(y) * kW + x) * 8;
            u16 hr = float_to_half(r), hg = float_to_half(g),
                hb = float_to_half(b), ha = float_to_half(a);
            std::memcpy(p + 0, &hr, 2);
            std::memcpy(p + 2, &hg, 2);
            std::memcpy(p + 4, &hb, 2);
            std::memcpy(p + 6, &ha, 2);
        }
    }
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

ResourceHandle MakeAndUploadTexture2D(DeviceFixture& fx, const std::vector<u8>& pixels,
                                       u32 w, u32 h, DataFormat format, TextureUsage extra,
                                       ResourceState finalState, const char* name) {
    TextureDesc td{};
    td.size = { w, h, 1 };
    td.mipLevels = 1;
    td.arraySize = 1;
    td.format = format;
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::CopyDest | TextureUsage::ShaderResource | extra;
    td.memoryUsage = GPUMemoryUsage::Static;
    td.name = name;
    ResourceHandle tex = fx.base->CreateTexture(td);
    if (tex == handles::INVALID_RESOURCE) return tex;

    BufferDesc sd{};
    sd.size = pixels.size();
    sd.type = BufferType::Raw;
    sd.memoryUsage = GPUMemoryUsage::Dynamic;
    sd.name = "Stage";
    ResourceHandle staging = fx.base->CreateBuffer(sd);
    fx.base->UpdateBufferData(staging, pixels.data(), pixels.size(), 0);

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { w, h, 1 };
    vcmd->CopyBufferToTexture(staging, tex, &region, 1);

    ResourceBarrier b{};
    b.resource = tex;
    b.beforeState = ResourceState::CopyDest;
    b.afterState = finalState;
    b.subresource = 0xFFFFFFFF;
    b.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&b, 1);
    vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(staging);
    return tex;
}

} // anonymous namespace

TestResult TestVulkanSSRTrace() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto spv = ReadSPV("Assets/Shaders/SSRTrace.spv");
    TEST_ASSERT(!spv.empty(), "Read SSRTrace.spv");
    ShaderHandle cs = fx.base->CreateShader(spv.data(), spv.size(),
                                             ShaderStage::Compute, "ssr_trace");
    TEST_ASSERT(cs != handles::INVALID_SHADER, "CreateShader compute");

    std::vector<u8> depthPx, hzbPx, colorPx;
    make_depth(depthPx);
    make_hzb(hzbPx);
    make_color(colorPx);

    ResourceHandle depthTex = MakeAndUploadTexture2D(fx, depthPx, kW, kH,
                                                      DataFormat::D32_Float, TextureUsage{},
                                                      ResourceState::ShaderResource, "SSRTrace_Depth");
    ResourceHandle hzbTex = MakeAndUploadTexture2D(fx, hzbPx, kW, kH,
                                                    DataFormat::R32_Float, TextureUsage{},
                                                    ResourceState::ShaderResource, "SSRTrace_HZB");
    ResourceHandle colorTex = MakeAndUploadTexture2D(fx, colorPx, kW, kH,
                                                      DataFormat::RGBA16_Float, TextureUsage{},
                                                      ResourceState::ShaderResource, "SSRTrace_Color");
    TEST_ASSERT(depthTex != handles::INVALID_RESOURCE, "depthTex");
    TEST_ASSERT(hzbTex   != handles::INVALID_RESOURCE, "hzbTex");
    TEST_ASSERT(colorTex != handles::INVALID_RESOURCE, "colorTex");

    ResourceHandle outputTex = fx.base->CreateTexture([&]{
        TextureDesc d{};
        d.size = { kHalfW, kHalfH, 1 };
        d.mipLevels = 1;
        d.arraySize = 1;
        d.format = DataFormat::RGBA16_Float;
        d.type = TextureType::Texture2D;
        d.usage = TextureUsage::UnorderedAccess | TextureUsage::CopySource;
        d.memoryUsage = GPUMemoryUsage::Static;
        d.name = "SSRTrace_Out";
        return d;
    }());
    TEST_ASSERT(outputTex != handles::INVALID_RESOURCE, "outputTex");

    // SSRTraceParams: 2 mat4 + 2 vec4 + 6 floats + 2 u32 = 192 bytes.
    // Layout (std140):
    //   0..64:   invProj (mat4)
    //   64..128: proj (mat4)
    //   128..144: screenSize (vec4) — x=W, y=H, z=1/W, w=1/H
    //   144..160: halfScreenSize (vec4) — x=HW, y=HH, z=1/HW, w=1/HH
    //   160..184: 6 floats (maxDistance, thickness, nearPlane, farPlane) + hzbMipLevels + frameIndex + pad
    struct SSRTraceParams {
        float invProj[16];
        float proj[16];
        float screenSize[4];
        float halfScreenSize[4];
        float maxDistance;
        float thickness;
        float nearPlane;
        float farPlane;
        u32   hzbMipLevels;
        u32   frameIndex;
        u32   pad0;
        u32   pad1;
    };
    SSRTraceParams params{};
    // Ortho invProj and proj for L=-1, R=1, B=-1, T=1, N=kNear, F=kFar (Vulkan NDC z [0,1]).
    // P column-major:
    //   col0 = (1, 0, 0, 0)
    //   col1 = (0, 1, 0, 0)
    //   col2 = (0, 0, -1/(F-N), 0)
    //   col3 = (0, 0, -N/(F-N), 1)
    for (int i = 0; i < 16; ++i) params.proj[i] = 0.0f;
    params.proj[0]  = 1.0f;
    params.proj[5]  = 1.0f;
    params.proj[10] = -1.0f / (kFar - kNear);
    params.proj[14] = -kNear / (kFar - kNear);
    params.proj[15] = 1.0f;
    // invProj = inverse of P. For ortho, swap z scale/offset signs.
    // invP col2 = (0, 0, -(F-N), 0), col3 = (0, 0, -N, 1).
    for (int i = 0; i < 16; ++i) params.invProj[i] = 0.0f;
    params.invProj[0]  = 1.0f;
    params.invProj[5]  = 1.0f;
    params.invProj[10] = -(kFar - kNear);
    params.invProj[14] = -kNear;
    params.invProj[15] = 1.0f;

    params.screenSize[0] = float(kW);
    params.screenSize[1] = float(kH);
    params.screenSize[2] = 1.0f / float(kW);
    params.screenSize[3] = 1.0f / float(kH);
    params.halfScreenSize[0] = float(kHalfW);
    params.halfScreenSize[1] = float(kHalfH);
    params.halfScreenSize[2] = 1.0f / float(kHalfW);
    params.halfScreenSize[3] = 1.0f / float(kHalfH);
    params.maxDistance = 5.0f;
    params.thickness   = 0.5f;
    params.nearPlane   = kNear;
    params.farPlane    = kFar;
    params.hzbMipLevels = 1u;  // Single-mip HZB for smoke test.
    params.frameIndex   = 0u;
    params.pad0 = 0u;
    params.pad1 = 0u;

    BufferDesc uboDesc{};
    uboDesc.size = sizeof(SSRTraceParams);
    uboDesc.type = BufferType::Constant;
    uboDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    uboDesc.name = "SSRTrace_UBO";
    ResourceHandle ubo = fx.base->CreateBuffer(uboDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer UBO");
    TEST_ASSERT(fx.base->UpdateBufferData(ubo, &params, sizeof(params), 0), "UpdateBufferData UBO");

    DescriptorSetLayoutBinding bindings[5]{};
    bindings[0] = { 0, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr };
    bindings[1] = { 1, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr };
    bindings[2] = { 2, DescriptorType::SampledImage,  1, ShaderStage::Compute, nullptr };
    bindings[3] = { 3, DescriptorType::StorageImage,  1, ShaderStage::Compute, nullptr };
    bindings[4] = { 4, DescriptorType::UniformBuffer, 1, ShaderStage::Compute, nullptr };
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 5;
    layoutDesc.bindings = bindings;
    DescriptorSetLayoutHandle layout = fx.base->CreateDescriptorSetLayout(layoutDesc);
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);

    DescriptorImageInfo depthInfo{ handles::INVALID_SAMPLER, depthTex, ResourceState::ShaderResource };
    DescriptorImageInfo hzbInfo{   handles::INVALID_SAMPLER, hzbTex,   ResourceState::ShaderResource };
    DescriptorImageInfo colorInfo{ handles::INVALID_SAMPLER, colorTex, ResourceState::ShaderResource };
    DescriptorImageInfo outInfo{   handles::INVALID_SAMPLER, outputTex, ResourceState::UnorderedAccess };
    DescriptorBufferInfo uboInfo{ ubo, 0, sizeof(SSRTraceParams) };
    WriteDescriptorSet writes[5]{};
    writes[0] = { ds, 0, 0, 1, DescriptorType::SampledImage,  &depthInfo, nullptr };
    writes[1] = { ds, 1, 0, 1, DescriptorType::SampledImage,  &hzbInfo,   nullptr };
    writes[2] = { ds, 2, 0, 1, DescriptorType::SampledImage,  &colorInfo, nullptr };
    writes[3] = { ds, 3, 0, 1, DescriptorType::StorageImage,  &outInfo,   nullptr };
    writes[4] = { ds, 4, 0, 1, DescriptorType::UniformBuffer, nullptr,    &uboInfo };
    fx.base->UpdateDescriptorSets(5, writes);

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
    vcmd->Dispatch((kHalfW + 7) / 8, (kHalfH + 7) / 8, 1);

    ResourceBarrier toCopyBarrier{};
    toCopyBarrier.resource = outputTex;
    toCopyBarrier.beforeState = ResourceState::UnorderedAccess;
    toCopyBarrier.afterState = ResourceState::CopySource;
    toCopyBarrier.subresource = 0xFFFFFFFF;
    toCopyBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&toCopyBarrier, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");
    fx.base->DestroyCommandBuffer(cmd);

    // Readback: RGBA16F = 8 bytes/pixel.
    BufferDesc rbDesc{};
    rbDesc.size = u64(kHalfW) * kHalfH * 8;
    rbDesc.type = BufferType::Raw;
    rbDesc.memoryUsage = GPUMemoryUsage::Readback;
    rbDesc.name = "SSRTrace_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(rbDesc);

    CommandBufferHandle cmd2 = fx.base->CreateCommandBuffer(CommandQueueType::Compute);
    VulkanCommandBuffer* vcmd2 = fx.vk->GetCommandBuffer(cmd2);
    vcmd2->Reset(); vcmd2->Begin();
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { kHalfW, kHalfH, 1 };
    vcmd2->CopyTextureToBuffer(outputTex, readback, &region, 1);
    vcmd2->End(); vcmd2->Submit(0); vcmd2->WaitForCompletion();
    fx.base->DestroyCommandBuffer(cmd2);

    void* mapped = fx.base->MapBuffer(readback, 0, u64(kHalfW) * kHalfH * 8);
    TEST_ASSERT(mapped, "MapBuffer");

    // Smoke verification: scan for NaN/Inf. Output should be finite everywhere.
    u32 nanCount = 0, infCount = 0;
    u32 nonZeroCount = 0;
    float maxAbs = 0.0f;
    for (u32 i = 0; i < kHalfW * kHalfH * 4; ++i) {
        u16 h;
        std::memcpy(&h, static_cast<const u8*>(mapped) + i * 2, 2);
        float v = half_to_float(h);
        if (std::isnan(v)) { ++nanCount; continue; }
        if (std::isinf(v)) { ++infCount; continue; }
        if (v != 0.0f) ++nonZeroCount;
        maxAbs = std::max(maxAbs, std::abs(v));
    }
    fx.base->UnmapBuffer(readback);

    std::cout << "[TestVulkanSSRTrace] nan=" << nanCount << " inf=" << infCount
              << " nonZero=" << nonZeroCount << "/" << (kHalfW * kHalfH * 4)
              << " maxAbs=" << maxAbs << std::endl;

    TEST_ASSERT(nanCount == 0, "no NaN in output (layout/bindings correct)");
    TEST_ASSERT(infCount == 0, "no Inf in output");

    fx.base->DestroyBuffer(readback);
    fx.base->DestroyBuffer(ubo);
    fx.base->DestroyTexture(outputTex);
    fx.base->DestroyTexture(colorTex);
    fx.base->DestroyTexture(hzbTex);
    fx.base->DestroyTexture(depthTex);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroyShader(cs);
    return TestResult::Passed;
}

void RegisterVulkanSSRTrace_Tests() {
    auto suite = std::make_shared<TestSuite>("VulkanSSRTrace_Tests");
    suite->AddTestCase(TestCase("SSRTrace", TestVulkanSSRTrace));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanSSRTrace_Tests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanSSRTrace] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
