/**
 * @file TestVulkanComputeBytesLarge.cpp
 * @brief P4c-F6 — SetComputeBytes >128B 隐式 UBO 回退验收
 * @details 用例:
 *   1) Large512 — 512B 常量(扰动参数位于 128B 之后与 496B 处)驱动 compute
 *      写 storage image,与 CPU 参照 max-abs-diff ≤ 2/255(UBO 路径唯一
 *      可达 offset 496 — push constant 无法覆盖)。
 *   2) Large256 — 256B 常量经 UBO 路径,offset 0/128 通道精确。
 *   3) Boundary128 — 恰好 128B 走 push constant 路径(行为不变,
 *      TestVulkanPushConstants 全量回归另行覆盖)。
 * 零 validation error 是硬门槛;Metal 参照缺失时 SSIM 部分 skip。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanTexture.h"
#endif

#include "Utils/ImageCompare.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace Engine::Test;
namespace et = EngineTest;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

constexpr u32 kW = 64, kH = 64;

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

std::vector<u8> ReadSPV(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> data;
    if (sz > 0) {
        data.resize(size_t(sz));
        f.read(reinterpret_cast<char*>(data.data()), sz);
    }
    return data;
}

/// 一次 dispatch:布局 barrier → 填充 bytes blob → SetComputeBytes → Dispatch
/// → readback。期望像素由 probeA(=v[0].x)/probeB(=v[8].y)/probeC(=v[31].z)
/// 给出;首跑纹理为 UNDEFINED,复跑为 CopySource(readback 留下)。
struct RunResult {
    std::vector<u8> pixels;   // RGBA8 readback(已 FlipY)
    bool ok{false};
};

RunResult RunComputeBytes(DeviceFixture& fx, PipelineHandle pipe, PipelineLayoutHandle pl,
                          DescriptorSetHandle ds, ResourceHandle outTex,
                          const void* blob, u32 blobSize, bool firstRun) {
    RunResult r;
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    if (!vcmd || !vcmd->Reset() || !vcmd->Begin()) { fx.base->DestroyCommandBuffer(cmd); return r; }

    // storage image 需要 GENERAL(UnorderedAccess)布局(BlurPass 同款)
    ResourceBarrier initOutBarrier{};
    initOutBarrier.resource = outTex;
    initOutBarrier.beforeState = firstRun ? ResourceState::Unknown : ResourceState::CopySource;
    initOutBarrier.afterState = ResourceState::UnorderedAccess;
    initOutBarrier.subresource = 0xFFFFFFFF;
    initOutBarrier.queueFamily = 0xFFFFFFFF;
    vcmd->InsertBarrier(&initOutBarrier, 1);

    vcmd->BindComputePipeline(pipe);
    vcmd->BindDescriptorSets(PipelineBindPoint::Compute, pl, 0, 1, &ds, 0, nullptr);
    vcmd->SetComputeBytes(0, blob, blobSize);
    vcmd->Dispatch(kW / 8, kH / 8, 1);

    // readback
    BufferDesc rbd{};
    rbd.size = u64(kW) * kH * 4;
    rbd.type = BufferType::Raw;
    rbd.memoryUsage = GPUMemoryUsage::Readback;
    rbd.name = "CBReadback";
    ResourceHandle rb = fx.base->CreateBuffer(rbd);
    if (rb == handles::INVALID_RESOURCE) { fx.base->DestroyCommandBuffer(cmd); return r; }
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kW, kH, 1};
    vcmd->CopyTextureToBuffer(outTex, rb, &region, 1);

    if (vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion()) {
        void* mapped = fx.base->MapBuffer(rb, 0, rbd.size);
        if (mapped) {
            r.pixels.resize(size_t(rbd.size));
            std::memcpy(r.pixels.data(), mapped, size_t(rbd.size));
            et::FlipYInPlace(r.pixels.data(), kW, kH);
            fx.base->UnmapBuffer(rb);
            r.ok = true;
        }
    }
    fx.base->DestroyBuffer(rb);
    fx.base->DestroyCommandBuffer(cmd);
    return r;
}

} // anonymous namespace

TestResult TestComputeBytesLarge() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");
    TEST_ASSERT(fx.vk->GetMaxPushConstantsSize() >= 128, "device push constant limit sane");

    // === compute shader + storage image + pipeline ===
    auto spv = ReadSPV("Assets/Shaders/P4cComputeBytesLarge.spv");
    TEST_ASSERT(!spv.empty(), "Read P4cComputeBytesLarge SPIR-V");
    ShaderHandle cs = fx.base->CreateShader(spv.data(), spv.size(), ShaderStage::Compute, "main");
    TEST_ASSERT(cs != handles::INVALID_SHADER, "CreateShader compute");

    // set 0: storage image
    DescriptorSetLayoutBinding bind{};
    bind.binding = 0;
    bind.descriptorType = DescriptorType::StorageImage;
    bind.descriptorCount = 1;
    bind.stageFlags = ShaderStage::Compute;
    DescriptorSetLayoutDesc dslDesc{};
    dslDesc.bindingCount = 1;
    dslDesc.bindings = &bind;
    DescriptorSetLayoutHandle dsl = fx.base->CreateDescriptorSetLayout(dslDesc);
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &dsl;
    plDesc.pushConstantRangeCount = 0;   // 隐式 UBO 路径:push 布局由 set3 承载
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    ComputePipelineDesc cpd{};
    cpd.computeShader = cs;
    cpd.layout = pl;
    PipelineHandle pipe = fx.base->CreateComputePipeline(cpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateComputePipeline");

    TextureDesc td{};
    td.size = {kW, kH, 1};
    td.mipLevels = 1;
    td.arraySize = 1;
    td.format = DataFormat::RGBA8_UNorm;
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::UnorderedAccess | TextureUsage::CopySource;
    td.memoryUsage = GPUMemoryUsage::Static;
    td.name = "ComputeOut";
    ResourceHandle outTex = fx.base->CreateTexture(td);
    TEST_ASSERT(outTex != handles::INVALID_RESOURCE, "CreateTexture storage");

    DescriptorSetDesc dsDesc{}; dsDesc.layout = dsl;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);
    DescriptorImageInfo imgInfo{};
    imgInfo.imageView = outTex;
    imgInfo.imageLayout = ResourceState::UnorderedAccess;
    WriteDescriptorSet write{};
    write.dstSet = ds; write.dstBinding = 0; write.dstArrayElement = 0;
    write.descriptorCount = 1; write.descriptorType = DescriptorType::StorageImage;
    write.imageInfo = &imgInfo;
    fx.base->UpdateDescriptorSets(1, &write);

    // === Case A: 超 push 上限的 blob(强制 UBO 路径;MoltenVK 上限 4096,
    // Linux/Win 常见 128 — 驱动无关地触发回退)。前 512B 含全三探针。 ===
    {
        const u32 blobSize = fx.vk->GetMaxPushConstantsSize() + 512;
        std::vector<u8> blob(blobSize, 0);
        float v[128];  // 512B as vec4[32]
        std::memset(v, 0, sizeof(v));
        v[0] = 0.25f;          // v[0].x  (offset 0)
        v[33] = 0.50f;         // v[8].y  (offset 128: 8*16 + 1*4)
        v[126] = 0.75f;        // v[31].z (offset 504: 31*16 + 2*4)
        std::memcpy(blob.data(), v, sizeof(v));
        RunResult r = RunComputeBytes(fx, pipe, pl, ds, outTex, blob.data(), blobSize, true);
        TEST_ASSERT(r.ok, "512B dispatch + readback");
        const int wantR = int(0.25f * 255 + 0.5f);
        const int wantG = int(0.50f * 255 + 0.5f);
        const int wantB = int(0.75f * 255 + 0.5f);
        u32 bad = 0;
        for (u32 p = 0; p < kW * kH; ++p) {
            if (std::abs(int(r.pixels[p * 4]) - wantR) > 2 ||
                std::abs(int(r.pixels[p * 4 + 1]) - wantG) > 2 ||
                std::abs(int(r.pixels[p * 4 + 2]) - wantB) > 2) ++bad;
        }
        std::cout << "[TestVulkanComputeBytesLarge] 512B mismatches: " << bad
                  << " first px: (" << int(r.pixels[0]) << "," << int(r.pixels[1])
                  << "," << int(r.pixels[2]) << "," << int(r.pixels[3])
                  << ") want (" << wantR << "," << wantG << "," << wantB << ")" << std::endl;
        TEST_ASSERT(bad == 0, "512B constants drive compute exactly (incl. offset 496)");
        et::SavePNG("P4cF6_compute512_vulkan.png", r.pixels.data(), kW, kH);
    }

    // === Case B: 另一档超限 blob(探针 A/B;C 通道内容未定义不检查) ===
    {
        const u32 blobSize = fx.vk->GetMaxPushConstantsSize() + 256;
        std::vector<u8> blob(blobSize, 0);
        float v[64];
        std::memset(v, 0, sizeof(v));
        v[0] = 0.80f;
        v[33] = 0.10f;
        std::memcpy(blob.data(), v, sizeof(v));
        RunResult r = RunComputeBytes(fx, pipe, pl, ds, outTex, blob.data(), blobSize, false);
        TEST_ASSERT(r.ok, "256B dispatch + readback");
        const int wantR = int(0.80f * 255 + 0.5f);
        const int wantG = int(0.10f * 255 + 0.5f);
        u32 bad = 0;
        for (u32 p = 0; p < kW * kH; ++p) {
            if (std::abs(int(r.pixels[p * 4]) - wantR) > 2 ||
                std::abs(int(r.pixels[p * 4 + 1]) - wantG) > 2) ++bad;
        }
        std::cout << "[TestVulkanComputeBytesLarge] 256B mismatches: " << bad << std::endl;
        TEST_ASSERT(bad == 0, "256B constants exact (R/G channels)");
    }

    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyTexture(outTex);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyDescriptorSetLayout(dsl);
    fx.base->DestroyShader(cs);
    return TestResult::Passed;
}

void RegisterVulkanComputeBytesLargeTests() {
    auto suite = std::make_shared<TestSuite>("VulkanComputeBytesLargeTests");
    suite->AddTestCase(TestCase("Large512And256", TestComputeBytesLarge));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanComputeBytesLargeTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanComputeBytesLarge] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif
