/**
 * @file TestVulkanSSRPass.cpp
 * @brief Phase 4b Tier 3.6 — SSR Pass placeholder parity test.
 * @details SSRPass.wgsl is currently a 1:1 color copy (placeholder for real
 *          SSR compute). It exercises:
 *            - Multi-texture descriptor set (SampledImage color/depth/normal)
 *            - StorageImage output (rgba32float write)
 *            - mat4 UBO binding
 *
 *          Input: a 16×16 RGBA16F gradient + D32 depth + RGBA16F normal.
 *          Output: rgba32float, equals input color (alpha=1.0).
 *
 * Tolerance: per-pixel max abs diff < 0.01. Half→float precision only.
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

constexpr u32 kW = 16;
constexpr u32 kH = 16;

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

// Generate RGBA16F color gradient where pixel (x, y) = (x/W, y/H, 0.5, 1.0).
void make_color_gradient(std::vector<u8>& out) {
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

// Generate D32 depth buffer: depth = 0.5 constant (placeholder).
void make_depth(std::vector<u8>& out) {
    out.resize(size_t(kW) * kH * 4);
    for (u32 i = 0; i < kW * kH; ++i) {
        float d = 0.5f;
        std::memcpy(out.data() + size_t(i) * 4, &d, 4);
    }
}

// Generate RGBA16F normal map: all (0, 0, 1, 0) (placeholder).
void make_normals(std::vector<u8>& out) {
    out.resize(size_t(kW) * kH * 8);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            u8* p = out.data() + (size_t(y) * kW + x) * 8;
            u16 hx = float_to_half(0.0f), hy = float_to_half(0.0f),
                hz = float_to_half(1.0f), hw = float_to_half(0.0f);
            std::memcpy(p + 0, &hx, 2);
            std::memcpy(p + 2, &hy, 2);
            std::memcpy(p + 4, &hz, 2);
            std::memcpy(p + 6, &hw, 2);
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

// Create a 2D texture, upload pixels via staging buffer, transition to given state.
ResourceHandle MakeAndUploadTexture2D(DeviceFixture& fx, const std::vector<u8>& pixels,
                                       DataFormat format, TextureUsage extra,
                                       ResourceState finalState, const char* name) {
    TextureDesc td{};
    td.size = { kW, kH, 1 };
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
    region.imageExtent = { kW, kH, 1 };
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

TestResult TestVulkanSSRPass() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto spv = ReadSPV("Assets/Shaders/SSRPass.spv");
    TEST_ASSERT(!spv.empty(), "Read SSRPass.spv");
    ShaderHandle cs = fx.base->CreateShader(spv.data(), spv.size(),
                                             ShaderStage::Compute, "ssrCS");
    TEST_ASSERT(cs != handles::INVALID_SHADER, "CreateShader compute");

    std::vector<u8> colorPx, depthPx, normalPx;
    make_color_gradient(colorPx);
    make_depth(depthPx);
    make_normals(normalPx);

    ResourceHandle colorTex = MakeAndUploadTexture2D(fx, colorPx, DataFormat::RGBA16_Float,
                                                      TextureUsage{}, ResourceState::ShaderResource,
                                                      "SSRPass_ColorIn");
    ResourceHandle depthTex = MakeAndUploadTexture2D(fx, depthPx, DataFormat::D32_Float,
                                                       TextureUsage{}, ResourceState::ShaderResource,
                                                       "SSRPass_DepthIn");
    ResourceHandle normalTex = MakeAndUploadTexture2D(fx, normalPx, DataFormat::RGBA16_Float,
                                                        TextureUsage{}, ResourceState::ShaderResource,
                                                        "SSRPass_NormalIn");
    TEST_ASSERT(colorTex != handles::INVALID_RESOURCE, "colorTex");
    TEST_ASSERT(depthTex != handles::INVALID_RESOURCE, "depthTex");
    TEST_ASSERT(normalTex != handles::INVALID_RESOURCE, "normalTex");

    ResourceHandle outputTex = fx.base->CreateTexture([&]{
        TextureDesc d{};
        d.size = { kW, kH, 1 };
        d.mipLevels = 1;
        d.arraySize = 1;
        d.format = DataFormat::RGBA32_Float;
        d.type = TextureType::Texture2D;
        d.usage = TextureUsage::UnorderedAccess | TextureUsage::CopySource;
        d.memoryUsage = GPUMemoryUsage::Static;
        d.name = "SSRPass_Out";
        return d;
    }());
    TEST_ASSERT(outputTex != handles::INVALID_RESOURCE, "outputTex");

    // SSRParams: viewProj + invViewProj + 4 floats. All zeros for the
    // placeholder pass (the shader doesn't actually use them, just reads).
    struct SSRParams {
        float viewProj[16];
        float invViewProj[16];
        float screenWidth;
        float screenHeight;
        float pad0;
        float pad1;
    };
    SSRParams paramsBytes{};
    paramsBytes.screenWidth = float(kW);
    paramsBytes.screenHeight = float(kH);
    // Identity matrices
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            paramsBytes.viewProj[i*4 + j] = (i == j) ? 1.0f : 0.0f;
    std::memcpy(paramsBytes.invViewProj, paramsBytes.viewProj, 64);

    BufferDesc uboDesc{};
    uboDesc.size = sizeof(SSRParams);
    uboDesc.type = BufferType::Constant;
    uboDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    uboDesc.name = "SSRPass_UBO";
    ResourceHandle ubo = fx.base->CreateBuffer(uboDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer UBO");
    TEST_ASSERT(fx.base->UpdateBufferData(ubo, &paramsBytes, sizeof(paramsBytes), 0), "UpdateBufferData UBO");

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

    DescriptorImageInfo colorInfo{ handles::INVALID_SAMPLER, colorTex,  ResourceState::ShaderResource };
    DescriptorImageInfo depthInfo{ handles::INVALID_SAMPLER, depthTex,  ResourceState::ShaderResource };
    DescriptorImageInfo normalInfo{ handles::INVALID_SAMPLER, normalTex, ResourceState::ShaderResource };
    DescriptorImageInfo outInfo{   handles::INVALID_SAMPLER, outputTex,  ResourceState::UnorderedAccess };
    DescriptorBufferInfo uboInfo{ ubo, 0, sizeof(SSRParams) };
    WriteDescriptorSet writes[5]{};
    writes[0] = { ds, 0, 0, 1, DescriptorType::SampledImage,  &colorInfo,  nullptr };
    writes[1] = { ds, 1, 0, 1, DescriptorType::SampledImage,  &depthInfo,  nullptr };
    writes[2] = { ds, 2, 0, 1, DescriptorType::SampledImage,  &normalInfo, nullptr };
    writes[3] = { ds, 3, 0, 1, DescriptorType::StorageImage,  &outInfo,    nullptr };
    writes[4] = { ds, 4, 0, 1, DescriptorType::UniformBuffer, nullptr,     &uboInfo };
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
    vcmd->Dispatch(kW / 8, kH / 8, 1);

    ResourceBarrier toCopyBarrier{};
    toCopyBarrier.resource = outputTex;
    toCopyBarrier.beforeState = ResourceState::UnorderedAccess;
    toCopyBarrier.afterState = ResourceState::CopySource;
    toCopyBarrier.subresource = 0xFFFFFFFF;
    toCopyBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&toCopyBarrier, 1);

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");
    fx.base->DestroyCommandBuffer(cmd);

    // Readback: RGBA32F = 16 bytes/pixel.
    BufferDesc rbDesc{};
    rbDesc.size = u64(kW) * kH * 16;
    rbDesc.type = BufferType::Raw;
    rbDesc.memoryUsage = GPUMemoryUsage::Readback;
    rbDesc.name = "SSRPass_Readback";
    ResourceHandle readback = fx.base->CreateBuffer(rbDesc);

    CommandBufferHandle cmd2 = fx.base->CreateCommandBuffer(CommandQueueType::Compute);
    VulkanCommandBuffer* vcmd2 = fx.vk->GetCommandBuffer(cmd2);
    vcmd2->Reset(); vcmd2->Begin();
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = { kW, kH, 1 };
    vcmd2->CopyTextureToBuffer(outputTex, readback, &region, 1);
    vcmd2->End(); vcmd2->Submit(0); vcmd2->WaitForCompletion();
    fx.base->DestroyCommandBuffer(cmd2);

    void* mapped = fx.base->MapBuffer(readback, 0, u64(kW) * kH * 16);
    TEST_ASSERT(mapped, "MapBuffer");

    float maxDiff = 0.0f, meanDiff = 0.0f;
    u32 count = 0;
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const float* gpu = reinterpret_cast<const float*>(
                static_cast<const u8*>(mapped) + (size_t(y) * kW + x) * 16);
            // Expected = input color (r, g, b, 1.0).
            float expR = float(x) / float(kW - 1);
            float expG = float(y) / float(kH - 1);
            float expB = 0.5f;
            float expA = 1.0f;
            float dR = std::abs(gpu[0] - expR);
            float dG = std::abs(gpu[1] - expG);
            float dB = std::abs(gpu[2] - expB);
            float dA = std::abs(gpu[3] - expA);
            float lm = std::max(dR, std::max(dG, std::max(dB, dA)));
            if (lm > maxDiff) maxDiff = lm;
            meanDiff += (dR + dG + dB + dA) / 4.0f;
            ++count;
        }
    }
    meanDiff /= float(count);
    fx.base->UnmapBuffer(readback);

    std::cout << "[TestVulkanSSRPass] max diff=" << maxDiff
              << " mean=" << meanDiff << " texels=" << count << std::endl;

    TEST_ASSERT(meanDiff < 0.005f, "mean abs diff < 0.005");
    TEST_ASSERT(maxDiff < 0.02f,   "max abs diff < 0.02 (half→float precision)");

    fx.base->DestroyBuffer(readback);
    fx.base->DestroyBuffer(ubo);
    fx.base->DestroyTexture(outputTex);
    fx.base->DestroyTexture(normalTex);
    fx.base->DestroyTexture(depthTex);
    fx.base->DestroyTexture(colorTex);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroyShader(cs);
    return TestResult::Passed;
}

void RegisterVulkanSSRPass_Tests() {
    auto suite = std::make_shared<TestSuite>("VulkanSSRPass_Tests");
    suite->AddTestCase(TestCase("SSRPass", TestVulkanSSRPass));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanSSRPass_Tests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanSSRPass] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
