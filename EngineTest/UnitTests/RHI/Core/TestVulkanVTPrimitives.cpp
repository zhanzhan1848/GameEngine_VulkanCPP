/**
 * @file TestVulkanVTPrimitives.cpp
 * @brief VT M0 — Virtual Texture RHI 原语验证(Vulkan/MoltenVK)。
 * @details 覆盖 openspec virtual-texture-system M0 的三项验收:
 *
 *   1. ReadbackRing roundtrip(M0.5):CS 写模式 → CopyBuffer 到 ring slot →
 *      Submit(signalFence) → WaitForSync → Map 校验;连续多帧不同 tag 轮转,
 *      验证 ring 复用无竞争,并记录 submit→fence 延迟。
 *   2. R16_UINT 整数纹理 texelFetch(M0.6):页表格式采样路径。
 *   3. UpdateTextureRegion(M0.7):两个不相交子区域上传后 texelFetch 全图
 *      回读,校验区域内数据正确 + 区域外(初始清零)未被误写(no-clobber)。
 *   4. ASTC 能力查询(M0.8):打印 supportsASTC(Apple Silicon 预期 true,
 *      离散 GPU 预期 false — 平台相关,仅日志不硬断言)。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIReadbackRing.h"
#include "Graphics/Lumen/DDGI/VarianceFenceTicket.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <chrono>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

using namespace primal;
using namespace primal::graphics::rhi;
using namespace Engine::Test;

namespace {

// ============================= SPIR-V loader(BlurPass 同款) =============================
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

constexpr u32 kRingElems = 4096;   // readback ring 元素数

// vtWriteCS 参数块(shader 内 Params)
struct WriteParams { u32 tag; u32 count; u32 pad0; u32 pad1; };

} // anonymous namespace

// =====================================================================================
// M0.5: readback ring roundtrip(多帧轮转 + 延迟测量)
// =====================================================================================
TestResult TestVT_ReadbackRing() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto cs = ReadSPV(VT_PRIMITIVES_WRITE_SPV);
    TEST_ASSERT(!cs.empty(), "Read VTTest_Write SPIR-V (run build_spv.sh if missing)");
    ShaderHandle computeShader = fx.base->CreateShader(cs.data(), cs.size(), ShaderStage::Compute, "main");
    TEST_ASSERT(computeShader != handles::INVALID_SHADER, "CreateShader vtWriteCS");

    // descriptor: b0 UBO / b1 storage out
    DescriptorSetLayoutBinding bindings[2]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = DescriptorType::UniformBuffer;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = ShaderStage::Compute;
    bindings[1].binding = 1;
    bindings[1].descriptorType = DescriptorType::StorageBuffer;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = ShaderStage::Compute;
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 2;
    layoutDesc.bindings = bindings;
    DescriptorSetLayoutHandle layout = fx.base->CreateDescriptorSetLayout(layoutDesc);
    TEST_ASSERT(layout != handles::INVALID_DESCRIPTOR_SET_LAYOUT, "CreateDescriptorSetLayout");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    ComputePipelineDesc cpd{};
    cpd.computeShader = computeShader;
    cpd.layout = pl;
    cpd.threadGroupSize = {64, 1, 1};
    PipelineHandle pipe = fx.base->CreateComputePipeline(cpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateComputePipeline");

    const u64 bufBytes = u64(kRingElems) * sizeof(u32);
    BufferDesc gpuDesc{};
    gpuDesc.size = bufBytes;
    gpuDesc.type = BufferType::Raw;              // → STORAGE_BUFFER usage
    gpuDesc.memoryUsage = GPUMemoryUsage::Static; // device-local, GPU 写
    gpuDesc.name = "VTTest_GpuOut";
    ResourceHandle gpuOut = fx.base->CreateBuffer(gpuDesc);
    TEST_ASSERT(gpuOut != handles::INVALID_RESOURCE, "CreateBuffer gpuOut");

    BufferDesc uboDesc{};
    uboDesc.size = sizeof(WriteParams);
    uboDesc.type = BufferType::Constant;
    uboDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    uboDesc.name = "VTTest_WriteParams";
    ResourceHandle ubo = fx.base->CreateBuffer(uboDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer params");

    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);
    TEST_ASSERT(ds != handles::INVALID_DESCRIPTOR_SET, "CreateDescriptorSet");
    DescriptorBufferInfo uboInfo{ubo, 0, sizeof(WriteParams)};
    DescriptorBufferInfo outInfo{gpuOut, 0, bufBytes};
    WriteDescriptorSet writes[2]{};
    writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].dstArrayElement = 0;
    writes[0].descriptorCount = 1; writes[0].descriptorType = DescriptorType::UniformBuffer;
    writes[0].bufferInfo = &uboInfo;
    writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].dstArrayElement = 0;
    writes[1].descriptorCount = 1; writes[1].descriptorType = DescriptorType::StorageBuffer;
    writes[1].bufferInfo = &outInfo;
    fx.base->UpdateDescriptorSets(2, writes);

    rhi_readback_ring ring;
    TEST_ASSERT(ring.initialize(fx.base, bufBytes, rhi_readback_ring::kDefaultRingSize),
                "readback ring initialize");

    primal::graphics::lumen::VarianceFenceTicket varianceTicket;
    primal::graphics::rhi::FrameSlotCompletion completion;
    const u32 kFrames = 7;  // > ringSize(3),覆盖 slot 复用
    for (u32 frame = 0; frame < kFrames; ++frame) {
        const u32 tag = 0xA0 + frame;
        WriteParams params{tag, kRingElems, 0, 0};
        TEST_ASSERT(fx.base->UpdateBufferData(ubo, &params, sizeof(params), 0),
                    "UpdateBufferData params");

        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        const u32 slot = ring.slot_for_frame(frame);
        completion.Invalidate();TEST_ASSERT(completion.Publish(slot),"publish completed host slot");
        TEST_ASSERT(varianceTicket.Begin(completion,slot,cmd,ring.fence(slot),frame,u64(frame)+1),"begin certified ticket");
        const auto* prior=varianceTicket.Take(ring.buffer(slot),bufBytes);
        TEST_ASSERT((frame<3 && !prior) || (frame>=3 && prior && prior->source_frame==frame-3),"only prior completed same-slot copy eligible");
        if(prior) {
            const auto* old=static_cast<const u32*>(ring.map(fx.base,slot));TEST_ASSERT(old,"map authenticated prior");
            for(u32 i=0;i<kRingElems;++i)TEST_ASSERT(old[i]==(0xA0u+frame-3u)*0x01010101u+i,"authenticated prior bits");
            ring.unmap(fx.base,slot);
        }
        TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin cmd");

        vcmd->BindComputePipeline(pipe);
        vcmd->BindDescriptorSets(PipelineBindPoint::Compute, pl, 0, 1, &ds, 0, nullptr);
        vcmd->Dispatch((kRingElems + 63) / 64, 1, 1);

        vcmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::Transfer,
                            AccessFlag::ShaderWrite, AccessFlag::TransferRead);

        TEST_ASSERT(varianceTicket.Record(ring.buffer(slot),bufBytes,gpuOut,frame),"record exact ticket");
        vcmd->CopyBuffer(gpuOut, ring.buffer(slot), 0, 0, bufBytes);
        vcmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,AccessFlag::TransferWrite,AccessFlag::HostRead);
        TEST_ASSERT(!varianceTicket.Take(ring.buffer(slot),bufBytes),"unsubmitted copy cannot be read");
        TEST_ASSERT(vcmd->End(), "End cmd");

        QueueSubmitInfo si{};
        si.cmdBuffer = cmd;
        ring.attach_fence(si, slot);
        auto t0 = std::chrono::steady_clock::now();
        const bool submitted=fx.base->Submit(si);
        TEST_ASSERT(varianceTicket.Commit(slot,cmd,ring.fence(slot),frame,u64(frame)+1,submitted),"commit exact successful submit");
        TEST_ASSERT(!varianceTicket.Take(ring.buffer(slot),bufBytes),"submission alone cannot authorize map");
        TEST_ASSERT(submitted, "Submit(frame)");
        TEST_ASSERT(ring.wait(fx.base, slot, 5000), "ring.wait (fence 5s)");
        auto t1 = std::chrono::steady_clock::now();
        double waitMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        const u32* mapped = static_cast<const u32*>(ring.map(fx.base, slot));
        TEST_ASSERT(mapped != nullptr, "ring.map");
        u32 mismatches = 0;
        for (u32 i = 0; i < kRingElems; ++i) {
            u32 expect = tag * 0x01010101u + i;
            if (mapped[i] != expect) {
                if (mismatches < 4) {
                    std::cerr << "[VTReadback] frame " << frame << " elem " << i
                              << ": got " << mapped[i] << " expect " << expect << std::endl;
                }
                ++mismatches;
            }
        }
        ring.unmap(fx.base, slot);
        fx.base->DestroyCommandBuffer(cmd);
        std::cout << "[VTReadback] frame " << frame << " (slot " << slot
                  << ") tag=0x" << std::hex << tag << std::dec
                  << " mismatches=" << mismatches << " fence-wait=" << waitMs << "ms" << std::endl;
        TEST_ASSERT(mismatches == 0, "ring data matches tag pattern");
    }

    ring.shutdown(fx.base);
    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyBuffer(gpuOut);
    fx.base->DestroyBuffer(ubo);
    fx.base->DestroyShader(computeShader);
    return TestResult::Passed;
}

// =====================================================================================
// M0.6 + M0.7: R16_UINT texelFetch + 子区域上传(区域内容 + no-clobber)
// =====================================================================================
TestResult TestVT_RegionUploadTexelFetch() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    constexpr u32 kTexW = 256, kTexH = 256;

    auto cs = ReadSPV(VT_PRIMITIVES_FETCH_SPV);
    TEST_ASSERT(!cs.empty(), "Read VTTest_TexelFetch SPIR-V (run build_spv.sh if missing)");
    ShaderHandle computeShader = fx.base->CreateShader(cs.data(), cs.size(), ShaderStage::Compute, "main");
    TEST_ASSERT(computeShader != handles::INVALID_SHADER, "CreateShader vtFetchCS");

    // 页表式纹理:R16_UINT
    TextureDesc texDesc{};
    texDesc.size = {kTexW, kTexH, 1};
    texDesc.mipLevels = 1;
    texDesc.arraySize = 1;
    texDesc.format = DataFormat::R16_UInt;
    texDesc.type = TextureType::Texture2D;
    texDesc.usage = TextureUsage::ShaderResource;
    texDesc.memoryUsage = GPUMemoryUsage::Static;
    texDesc.name = "VTTest_PageTable";
    ResourceHandle tex = fx.base->CreateTexture(texDesc);
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture R16_UINT");

    // 先整图清零(经 updateData 全图路径),再写两个不相交子区域
    {
        std::vector<u16> zeros(size_t(kTexW) * kTexH, 0);
        // RHIResource::UpdateData → updateDataImpl(整图,行粒度 offset=0)
        TextureRegionUpdate full{};
        full.width = kTexW; full.height = kTexH;
        TEST_ASSERT(fx.base->UpdateTextureRegion(tex, zeros.data(), zeros.size() * sizeof(u16), full),
                    "full clear via UpdateTextureRegion");
    }

    // 区域 A: (0,0) 128x128,值 = 0x1000 | (x*8+y) & 0xFFF
    // 区域 B: (128,128) 128x128,值 = 0x2000 | (x*8+y) & 0xFFF
    auto fillRegion = [](u32 x0, u32 y0, u32 w, u32 h, u16 base, std::vector<u16>& out) {
        out.resize(size_t(w) * h);
        for (u32 y = 0; y < h; ++y)
            for (u32 x = 0; x < w; ++x)
                out[size_t(y) * w + x] = u16(base | ((x0 + x) * 8 + (y0 + y)) & 0xFFF);
    };
    std::vector<u16> regionA, regionB;
    fillRegion(0, 0, 128, 128, 0x1000, regionA);
    fillRegion(128, 128, 128, 128, 0x2000, regionB);

    TextureRegionUpdate ra{};
    ra.offsetX = 0; ra.offsetY = 0; ra.width = 128; ra.height = 128;
    TEST_ASSERT(fx.base->UpdateTextureRegion(tex, regionA.data(), regionA.size() * sizeof(u16), ra),
                "UpdateTextureRegion A");

    TextureRegionUpdate rb{};
    rb.offsetX = 128; rb.offsetY = 128; rb.width = 128; rb.height = 128;
    TEST_ASSERT(fx.base->UpdateTextureRegion(tex, regionB.data(), regionB.size() * sizeof(u16), rb),
                "UpdateTextureRegion B");

    // 越界/尺寸不符负路径
    {
        TextureRegionUpdate bad = rb;
        bad.offsetX = 200; bad.width = 128;  // 200+128 > 256
        TEST_ASSERT(!fx.base->UpdateTextureRegion(tex, regionB.data(), regionB.size() * sizeof(u16), bad),
                    "region out-of-bounds rejected");
        TextureRegionUpdate badSize = ra;
        TEST_ASSERT(!fx.base->UpdateTextureRegion(tex, regionA.data(), regionA.size() * sizeof(u16) - 2, badSize),
                    "wrong size rejected");
    }

    // CS:全图 texelFetch → storage buffer → readback
    struct FetchParams { u32 width; u32 height; u32 pad0; u32 pad1; };
    FetchParams fp{kTexW, kTexH, 0, 0};

    const u64 outBytes = u64(kTexW) * kTexH * sizeof(u32);
    BufferDesc outDesc{};
    outDesc.size = outBytes;
    outDesc.type = BufferType::Raw;
    outDesc.memoryUsage = GPUMemoryUsage::Static;
    outDesc.name = "VTTest_FetchOut";
    ResourceHandle out = fx.base->CreateBuffer(outDesc);
    TEST_ASSERT(out != handles::INVALID_RESOURCE, "CreateBuffer fetchOut");

    BufferDesc uboDesc{};
    uboDesc.size = sizeof(FetchParams);
    uboDesc.type = BufferType::Constant;
    uboDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    uboDesc.name = "VTTest_FetchParams";
    ResourceHandle ubo = fx.base->CreateBuffer(uboDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer fetchParams");
    TEST_ASSERT(fx.base->UpdateBufferData(ubo, &fp, sizeof(fp), 0), "UpdateBufferData fetchParams");

    DescriptorSetLayoutBinding bindings[3]{};
    bindings[0].binding = 0;
    bindings[0].descriptorType = DescriptorType::UniformBuffer;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags = ShaderStage::Compute;
    bindings[1].binding = 1;
    bindings[1].descriptorType = DescriptorType::StorageBuffer;
    bindings[1].descriptorCount = 1;
    bindings[1].stageFlags = ShaderStage::Compute;
    bindings[2].binding = 2;
    bindings[2].descriptorType = DescriptorType::SampledImage;
    bindings[2].descriptorCount = 1;
    bindings[2].stageFlags = ShaderStage::Compute;
    DescriptorSetLayoutDesc layoutDesc{};
    layoutDesc.bindingCount = 3;
    layoutDesc.bindings = bindings;
    DescriptorSetLayoutHandle layout = fx.base->CreateDescriptorSetLayout(layoutDesc);
    TEST_ASSERT(layout != handles::INVALID_DESCRIPTOR_SET_LAYOUT, "CreateDescriptorSetLayout");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    ComputePipelineDesc cpd{};
    cpd.computeShader = computeShader;
    cpd.layout = pl;
    cpd.threadGroupSize = {8, 8, 1};
    PipelineHandle pipe = fx.base->CreateComputePipeline(cpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateComputePipeline vtFetchCS");

    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = fx.base->CreateDescriptorSet(dsDesc);
    TEST_ASSERT(ds != handles::INVALID_DESCRIPTOR_SET, "CreateDescriptorSet");
    DescriptorBufferInfo uboInfo{ubo, 0, sizeof(FetchParams)};
    DescriptorBufferInfo outInfo{out, 0, outBytes};
    DescriptorImageInfo texInfo{};
    texInfo.imageView = tex;
    texInfo.imageLayout = ResourceState::ShaderResource;
    WriteDescriptorSet writes[3]{};
    writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].dstArrayElement = 0;
    writes[0].descriptorCount = 1; writes[0].descriptorType = DescriptorType::UniformBuffer;
    writes[0].bufferInfo = &uboInfo;
    writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].dstArrayElement = 0;
    writes[1].descriptorCount = 1; writes[1].descriptorType = DescriptorType::StorageBuffer;
    writes[1].bufferInfo = &outInfo;
    writes[2].dstSet = ds; writes[2].dstBinding = 2; writes[2].dstArrayElement = 0;
    writes[2].descriptorCount = 1; writes[2].descriptorType = DescriptorType::SampledImage;
    writes[2].imageInfo = &texInfo;
    fx.base->UpdateDescriptorSets(3, writes);

    rhi_readback_ring ring;
    TEST_ASSERT(ring.initialize(fx.base, outBytes, rhi_readback_ring::kDefaultRingSize),
                "readback ring initialize");

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin cmd");

    vcmd->BindComputePipeline(pipe);
    vcmd->BindDescriptorSets(PipelineBindPoint::Compute, pl, 0, 1, &ds, 0, nullptr);
    vcmd->Dispatch(kTexW / 8, kTexH / 8, 1);

    vcmd->MemoryBarrier(PipelineStage::ComputeShader, PipelineStage::Transfer,
                        AccessFlag::ShaderWrite, AccessFlag::TransferRead);
    vcmd->CopyBuffer(out, ring.buffer(0), 0, 0, outBytes);
    TEST_ASSERT(vcmd->End(), "End cmd");

    QueueSubmitInfo si{};
    si.cmdBuffer = cmd;
    ring.attach_fence(si, 0);
    TEST_ASSERT(fx.base->Submit(si), "Submit");
    TEST_ASSERT(ring.wait(fx.base, 0, 5000), "ring.wait");

    const u32* mapped = static_cast<const u32*>(ring.map(fx.base, 0));
    TEST_ASSERT(mapped != nullptr, "ring.map");

    u32 inA = 0, inB = 0, outside = 0;
    u32 errA = 0, errB = 0, errOutside = 0;
    auto inRect = [](u32 x, u32 y, u32 rx, u32 ry, u32 rw, u32 rh) {
        return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
    };
    for (u32 y = 0; y < kTexH; ++y) {
        for (u32 x = 0; x < kTexW; ++x) {
            const u32 got = mapped[size_t(y) * kTexW + x];
            if (inRect(x, y, 0, 0, 128, 128)) {
                ++inA;
                u16 expect = u16(0x1000 | ((x * 8 + y) & 0xFFF));
                if (got != expect) { if (errA < 4) std::cerr << "[VTRegion] A(" << x << "," << y << ") got "
                    << got << " expect " << expect << std::endl; ++errA; }
            } else if (inRect(x, y, 128, 128, 128, 128)) {
                ++inB;
                u16 expect = u16(0x2000 | ((x * 8 + y) & 0xFFF));
                if (got != expect) { if (errB < 4) std::cerr << "[VTRegion] B(" << x << "," << y << ") got "
                    << got << " expect " << expect << std::endl; ++errB; }
            } else {
                ++outside;
                if (got != 0) { if (errOutside < 4) std::cerr << "[VTRegion] outside(" << x << "," << y
                    << ") got " << got << " expect 0" << std::endl; ++errOutside; }
            }
        }
    }
    ring.unmap(fx.base, 0);
    fx.base->DestroyCommandBuffer(cmd);
    ring.shutdown(fx.base);

    std::cout << "[VTRegion] A=" << inA << " (err " << errA << ")  B=" << inB
              << " (err " << errB << ")  outside=" << outside << " (err " << errOutside << ")" << std::endl;
    TEST_ASSERT(inA == 128 * 128 && inB == 128 * 128, "region coverage");
    TEST_ASSERT(errA == 0, "region A values correct");
    TEST_ASSERT(errB == 0, "region B values correct");
    TEST_ASSERT(errOutside == 0, "no-clobber outside regions");

    fx.base->DestroyDescriptorSet(ds);
    fx.base->DestroyDescriptorSetLayout(layout);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyBuffer(out);
    fx.base->DestroyBuffer(ubo);
    fx.base->DestroyTexture(tex);
    fx.base->DestroyShader(computeShader);
    return TestResult::Passed;
}

// =====================================================================================
// M0.8: ASTC 能力查询(平台相关,日志输出)
// =====================================================================================
TestResult TestVT_ASTCCapability() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");
    const DeviceInfo& info = fx.base->GetDeviceInfo();
    std::cout << "[VTASTC] device=" << info.deviceName << " supportsASTC="
              << (info.supportsASTC ? "true" : "false") << std::endl;
    // Apple Silicon + MoltenVK 预期 true;离散 GPU 预期 false。仅验证字段
    // 被填充(不硬断言具体值,CI 硬件无关)。
    fx.base->Shutdown();
    fx.base = nullptr;
    return TestResult::Passed;
}

void RegisterVulkanVTPrimitivesTests() {
    auto suite = std::make_shared<TestSuite>("VulkanVTPrimitivesTests");
    suite->AddTestCase(TestCase("ReadbackRing", TestVT_ReadbackRing));
    suite->AddTestCase(TestCase("RegionUploadTexelFetch", TestVT_RegionUploadTexelFetch));
    suite->AddTestCase(TestCase("ASTCCapability", TestVT_ASTCCapability));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanVTPrimitivesTests();
    const auto stats=TestRunner::RunAllSuites();
    return stats.totalTests==3 && stats.passedTests==3 && stats.failedTests==0 && stats.skippedTests==0 ? 0 : 1;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanVTPrimitives] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
