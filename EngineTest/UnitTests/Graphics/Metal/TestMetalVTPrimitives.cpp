/**
 * @file TestMetalVTPrimitives.cpp
 * @brief VT M0 — Virtual Texture RHI 原语验证(Metal)。
 * @details 与 TestVulkanVTPrimitives.cpp 对等的 Metal 侧用例:
 *
 *   1. ReadbackRing roundtrip(M0.5):CS 写模式 → CopyBuffer → Submit(signalFence)
 *      → WaitForSync → Map 校验;多帧轮转验证 ring 复用。
 *   2. R16_UINT 整数纹理 texelFetch(M0.6):MSL texture2d<uint>::read 路径。
 *   3. UpdateTextureRegion(M0.7):两个子区域上传(Private 存储走 staging 队列,
 *      Begin 时编码 — 见 MetalCommandBuffer::beginImpl 的对齐修改)+
 *      texelFetch 回读校验区域值与 no-clobber。
 *   4. ASTC 能力查询(M0.8):Apple GPU 预期 supportsASTC=true(硬断言,
 *      本测试仅在 __APPLE__ 下编译)。
 */

#ifdef __APPLE__

#include "../../TestFramework.h"

#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIReadbackRing.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalDevice.h"
#include "Engine/Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"

#include <chrono>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

namespace {

constexpr u32 kRingElems = 4096;
constexpr u32 kTexW = 256, kTexH = 256;

// ============================================================================
// 嵌入式 MSL — 与 GLSL VTTest_*.comp 语义一致
// binding: 0 = UBO params, 1 = storage out, 2 = 采样纹理(仅 fetch kernel)
// ============================================================================
const char* kWriteCS = R"(
#include <metal_stdlib>
using namespace metal;

kernel void vtWriteCS(constant uint4& params [[buffer(0)]],   // x=tag, y=count
                      device uint* outBuf [[buffer(1)]],
                      uint gid [[thread_position_in_grid]])
{
    if (gid < params.y) {
        outBuf[gid] = params.x * 0x01010101u + gid;
    }
}
)";

const char* kFetchCS = R"(
#include <metal_stdlib>
using namespace metal;

kernel void vtFetchCS(constant uint4& params [[buffer(0)]],   // x=width, y=height
                      device uint* outBuf [[buffer(1)]],
                      texture2d<uint, access::sample> srcTex [[texture(2)]],
                      uint2 xy [[thread_position_in_grid]])
{
    if (xy.x >= params.x || xy.y >= params.y) return;
    outBuf[xy.y * params.x + xy.x] = srcTex.read(ushort2(xy), 0).r;
}
)";

struct MetalFixture {
    DeviceDesc desc{};
    MetalDevice* device{nullptr};
    bool Init() {
        desc.platform = RHIPlatform::Metal;
        desc.enableDebug = true;
        device = new MetalDevice(desc);
        if (!device->Initialize() || !device->IsValid()) {
            delete device; device = nullptr;
            return false;
        }
        return true;
    }
    ~MetalFixture() { if (device) { device->Shutdown(); delete device; } }
};

} // anonymous namespace

// =====================================================================================
// M0.5: readback ring roundtrip
// =====================================================================================
TestResult TestVT_ReadbackRing_Metal() {
    MetalFixture fx;
    TEST_ASSERT(fx.Init(), "Metal device init");

    const size_t srcLen = strlen(kWriteCS);
    ShaderHandle cs = fx.device->CreateShader(kWriteCS, srcLen, ShaderStage::Compute, "vtWriteCS");
    TEST_ASSERT(cs != handles::INVALID_SHADER, "CreateShader vtWriteCS");

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
    DescriptorSetLayoutHandle layout = fx.device->CreateDescriptorSetLayout(layoutDesc);
    TEST_ASSERT(layout != handles::INVALID_DESCRIPTOR_SET_LAYOUT, "CreateDescriptorSetLayout");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.device->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    ComputePipelineDesc cpd{};
    cpd.computeShader = cs;
    cpd.layout = pl;
    cpd.threadGroupSize = {64, 1, 1};
    PipelineHandle pipe = fx.device->CreateComputePipeline(cpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateComputePipeline");

    const u64 bufBytes = u64(kRingElems) * sizeof(u32);
    BufferDesc gpuDesc{};
    gpuDesc.size = bufBytes;
    gpuDesc.type = BufferType::Raw;
    gpuDesc.memoryUsage = GPUMemoryUsage::Static;
    gpuDesc.name = "VTTest_GpuOut";
    ResourceHandle gpuOut = fx.device->CreateBuffer(gpuDesc);
    TEST_ASSERT(gpuOut != handles::INVALID_RESOURCE, "CreateBuffer gpuOut");

    BufferDesc uboDesc{};
    uboDesc.size = 16;
    uboDesc.type = BufferType::Constant;
    uboDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    uboDesc.name = "VTTest_WriteParams";
    ResourceHandle ubo = fx.device->CreateBuffer(uboDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer params");

    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = fx.device->CreateDescriptorSet(dsDesc);
    TEST_ASSERT(ds != handles::INVALID_DESCRIPTOR_SET, "CreateDescriptorSet");
    DescriptorBufferInfo uboInfo{ubo, 0, 16};
    DescriptorBufferInfo outInfo{gpuOut, 0, bufBytes};
    WriteDescriptorSet writes[2]{};
    writes[0].dstSet = ds; writes[0].dstBinding = 0; writes[0].dstArrayElement = 0;
    writes[0].descriptorCount = 1; writes[0].descriptorType = DescriptorType::UniformBuffer;
    writes[0].bufferInfo = &uboInfo;
    writes[1].dstSet = ds; writes[1].dstBinding = 1; writes[1].dstArrayElement = 0;
    writes[1].descriptorCount = 1; writes[1].descriptorType = DescriptorType::StorageBuffer;
    writes[1].bufferInfo = &outInfo;
    fx.device->UpdateDescriptorSets(2, writes);

    rhi_readback_ring ring;
    TEST_ASSERT(ring.initialize(fx.device, bufBytes, rhi_readback_ring::kDefaultRingSize),
                "readback ring initialize");

    const u32 kFrames = 5;
    for (u32 frame = 0; frame < kFrames; ++frame) {
        const u32 tag = 0xA0 + frame;
        u32 params[4] = {tag, kRingElems, 0, 0};
        TEST_ASSERT(fx.device->UpdateBufferData(ubo, params, sizeof(params), 0),
                    "UpdateBufferData params");

        CommandBufferHandle cmd = fx.device->CreateCommandBuffer(CommandQueueType::Graphics);
        MetalCommandBuffer* mcmd = fx.device->GetCommandBuffer(cmd);
        TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "Begin cmd");

        mcmd->BindComputePipeline(pipe);
        mcmd->BindDescriptorSets(PipelineBindPoint::Compute, pl, 0, 1, &ds, 0, nullptr);
        mcmd->Dispatch((kRingElems + 63) / 64, 1, 1);

        const u32 slot = ring.slot_for_frame(frame);
        mcmd->CopyBuffer(gpuOut, ring.buffer(slot), 0, 0, bufBytes);

        TEST_ASSERT(mcmd->End(), "End cmd");

        QueueSubmitInfo si{};
        si.cmdBuffer = cmd;
        ring.attach_fence(si, slot);
        auto t0 = std::chrono::steady_clock::now();
        TEST_ASSERT(fx.device->Submit(si), "Submit(frame)");
        TEST_ASSERT(ring.wait(fx.device, slot, 5000), "ring.wait (fence 5s)");
        auto t1 = std::chrono::steady_clock::now();
        double waitMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

        const u32* mapped = static_cast<const u32*>(ring.map(fx.device, slot));
        TEST_ASSERT(mapped != nullptr, "ring.map");
        u32 mismatches = 0;
        for (u32 i = 0; i < kRingElems; ++i) {
            u32 expect = tag * 0x01010101u + i;
            if (mapped[i] != expect) {
                if (mismatches < 4) {
                    std::cerr << "[VTReadback/Metal] frame " << frame << " elem " << i
                              << ": got " << mapped[i] << " expect " << expect << std::endl;
                }
                ++mismatches;
            }
        }
        ring.unmap(fx.device, slot);
        fx.device->DestroyCommandBuffer(cmd);
        std::cout << "[VTReadback/Metal] frame " << frame << " (slot " << slot
                  << ") mismatches=" << mismatches << " fence-wait=" << waitMs << "ms" << std::endl;
        TEST_ASSERT(mismatches == 0, "ring data matches tag pattern");
    }

    ring.shutdown(fx.device);
    fx.device->DestroyDescriptorSet(ds);
    fx.device->DestroyDescriptorSetLayout(layout);
    fx.device->DestroyPipelineLayout(pl);
    fx.device->DestroyPipeline(pipe);
    fx.device->DestroyBuffer(gpuOut);
    fx.device->DestroyBuffer(ubo);
    fx.device->DestroyShader(cs);
    return TestResult::Passed;
}

// =====================================================================================
// M0.6 + M0.7: R16_UINT texelFetch + 子区域上传
// =====================================================================================
TestResult TestVT_RegionUploadTexelFetch_Metal() {
    MetalFixture fx;
    TEST_ASSERT(fx.Init(), "Metal device init");

    const size_t srcLen = strlen(kFetchCS);
    ShaderHandle cs = fx.device->CreateShader(kFetchCS, srcLen, ShaderStage::Compute, "vtFetchCS");
    TEST_ASSERT(cs != handles::INVALID_SHADER, "CreateShader vtFetchCS");

    TextureDesc texDesc{};
    texDesc.size = {kTexW, kTexH, 1};
    texDesc.mipLevels = 1;
    texDesc.arraySize = 1;
    texDesc.format = DataFormat::R16_UInt;
    texDesc.type = TextureType::Texture2D;
    texDesc.usage = TextureUsage::ShaderResource;
    texDesc.memoryUsage = GPUMemoryUsage::Static;  // Private 存储 → staging 队列路径
    texDesc.name = "VTTest_PageTable";
    ResourceHandle tex = fx.device->CreateTexture(texDesc);
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture R16_UINT");

    // 整图清零 → 两个不相交子区域(与 Vulkan 测试同款数据)
    {
        std::vector<u16> zeros(size_t(kTexW) * kTexH, 0);
        TextureRegionUpdate full{};
        full.width = kTexW; full.height = kTexH;
        TEST_ASSERT(fx.device->UpdateTextureRegion(tex, zeros.data(), zeros.size() * sizeof(u16), full),
                    "full clear via UpdateTextureRegion");
    }
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
    TEST_ASSERT(fx.device->UpdateTextureRegion(tex, regionA.data(), regionA.size() * sizeof(u16), ra),
                "UpdateTextureRegion A");

    TextureRegionUpdate rb{};
    rb.offsetX = 128; rb.offsetY = 128; rb.width = 128; rb.height = 128;
    TEST_ASSERT(fx.device->UpdateTextureRegion(tex, regionB.data(), regionB.size() * sizeof(u16), rb),
                "UpdateTextureRegion B");

    {
        TextureRegionUpdate bad = rb;
        bad.offsetX = 200; bad.width = 128;
        TEST_ASSERT(!fx.device->UpdateTextureRegion(tex, regionB.data(), regionB.size() * sizeof(u16), bad),
                    "region out-of-bounds rejected");
    }

    const u64 outBytes = u64(kTexW) * kTexH * sizeof(u32);
    BufferDesc outDesc{};
    outDesc.size = outBytes;
    outDesc.type = BufferType::Raw;
    outDesc.memoryUsage = GPUMemoryUsage::Static;
    outDesc.name = "VTTest_FetchOut";
    ResourceHandle out = fx.device->CreateBuffer(outDesc);
    TEST_ASSERT(out != handles::INVALID_RESOURCE, "CreateBuffer fetchOut");

    BufferDesc uboDesc{};
    uboDesc.size = 16;
    uboDesc.type = BufferType::Constant;
    uboDesc.memoryUsage = GPUMemoryUsage::Dynamic;
    uboDesc.name = "VTTest_FetchParams";
    ResourceHandle ubo = fx.device->CreateBuffer(uboDesc);
    TEST_ASSERT(ubo != handles::INVALID_RESOURCE, "CreateBuffer fetchParams");
    u32 fp[4] = {kTexW, kTexH, 0, 0};
    TEST_ASSERT(fx.device->UpdateBufferData(ubo, fp, sizeof(fp), 0), "UpdateBufferData fetchParams");

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
    DescriptorSetLayoutHandle layout = fx.device->CreateDescriptorSetLayout(layoutDesc);
    TEST_ASSERT(layout != handles::INVALID_DESCRIPTOR_SET_LAYOUT, "CreateDescriptorSetLayout");

    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 1;
    plDesc.setLayouts = &layout;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = fx.device->CreatePipelineLayout(plDesc);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    ComputePipelineDesc cpd{};
    cpd.computeShader = cs;
    cpd.layout = pl;
    cpd.threadGroupSize = {8, 8, 1};
    PipelineHandle pipe = fx.device->CreateComputePipeline(cpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateComputePipeline vtFetchCS");

    DescriptorSetDesc dsDesc{}; dsDesc.layout = layout;
    DescriptorSetHandle ds = fx.device->CreateDescriptorSet(dsDesc);
    TEST_ASSERT(ds != handles::INVALID_DESCRIPTOR_SET, "CreateDescriptorSet");
    DescriptorBufferInfo uboInfo{ubo, 0, 16};
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
    fx.device->UpdateDescriptorSets(3, writes);

    rhi_readback_ring ring;
    TEST_ASSERT(ring.initialize(fx.device, outBytes, rhi_readback_ring::kDefaultRingSize),
                "readback ring initialize");

    CommandBufferHandle cmd = fx.device->CreateCommandBuffer(CommandQueueType::Graphics);
    MetalCommandBuffer* mcmd = fx.device->GetCommandBuffer(cmd);
    TEST_ASSERT(mcmd->Reset() && mcmd->Begin(), "Begin cmd");  // Begin 先编码 pending 上传 blits

    mcmd->BindComputePipeline(pipe);
    mcmd->BindDescriptorSets(PipelineBindPoint::Compute, pl, 0, 1, &ds, 0, nullptr);
    mcmd->Dispatch(kTexW / 8, kTexH / 8, 1);

    mcmd->CopyBuffer(out, ring.buffer(0), 0, 0, outBytes);
    TEST_ASSERT(mcmd->End(), "End cmd");

    QueueSubmitInfo si{};
    si.cmdBuffer = cmd;
    ring.attach_fence(si, 0);
    TEST_ASSERT(fx.device->Submit(si), "Submit");
    TEST_ASSERT(ring.wait(fx.device, 0, 5000), "ring.wait");

    const u32* mapped = static_cast<const u32*>(ring.map(fx.device, 0));
    TEST_ASSERT(mapped != nullptr, "ring.map");

    u32 errA = 0, errB = 0, errOutside = 0;
    auto inRect = [](u32 x, u32 y, u32 rx, u32 ry, u32 rw, u32 rh) {
        return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
    };
    for (u32 y = 0; y < kTexH; ++y) {
        for (u32 x = 0; x < kTexW; ++x) {
            const u32 got = mapped[size_t(y) * kTexW + x];
            if (inRect(x, y, 0, 0, 128, 128)) {
                if (got != u32(u16(0x1000 | ((x * 8 + y) & 0xFFF)))) ++errA;
            } else if (inRect(x, y, 128, 128, 128, 128)) {
                if (got != u32(u16(0x2000 | ((x * 8 + y) & 0xFFF)))) ++errB;
            } else {
                if (got != 0) ++errOutside;
            }
        }
    }
    ring.unmap(fx.device, 0);
    fx.device->DestroyCommandBuffer(cmd);
    ring.shutdown(fx.device);

    std::cout << "[VTRegion/Metal] errA=" << errA << " errB=" << errB
              << " errOutside=" << errOutside << std::endl;
    TEST_ASSERT(errA == 0, "region A values correct");
    TEST_ASSERT(errB == 0, "region B values correct");
    TEST_ASSERT(errOutside == 0, "no-clobber outside regions");

    fx.device->DestroyDescriptorSet(ds);
    fx.device->DestroyDescriptorSetLayout(layout);
    fx.device->DestroyPipelineLayout(pl);
    fx.device->DestroyPipeline(pipe);
    fx.device->DestroyBuffer(out);
    fx.device->DestroyBuffer(ubo);
    fx.device->DestroyTexture(tex);
    fx.device->DestroyShader(cs);
    return TestResult::Passed;
}

// =====================================================================================
// M0.8: ASTC 能力查询(Apple GPU 硬断言 true)
// =====================================================================================
TestResult TestVT_ASTCCapability_Metal() {
    MetalFixture fx;
    TEST_ASSERT(fx.Init(), "Metal device init");
    RHIDeviceBase* base = fx.device;  // GetDeviceInfo 经基类接口访问
    const DeviceInfo& info = base->GetDeviceInfo();
    std::cout << "[VTASTC/Metal] device=" << info.deviceName << " supportsASTC="
              << (info.supportsASTC ? "true" : "false") << std::endl;
    TEST_ASSERT(info.supportsASTC, "Apple GPU supports ASTC");
    return TestResult::Passed;
}

void RegisterMetalVTPrimitivesTests() {
    auto suite = std::make_shared<TestSuite>("MetalVTPrimitivesTests");
    suite->AddTestCase(TestCase("ReadbackRing", TestVT_ReadbackRing_Metal));
    suite->AddTestCase(TestCase("RegionUploadTexelFetch", TestVT_RegionUploadTexelFetch_Metal));
    suite->AddTestCase(TestCase("ASTCCapability", TestVT_ASTCCapability_Metal));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterMetalVTPrimitivesTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // !__APPLE__

int main() {
    std::cout << "[TestMetalVTPrimitives] not Apple platform — no-op." << std::endl;
    return 0;
}

#endif // __APPLE__
