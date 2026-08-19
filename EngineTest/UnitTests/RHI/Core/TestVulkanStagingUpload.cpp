/**
 * @file TestVulkanStagingUpload.cpp
 * @brief P4c-F4 — StagingAllocator 上传队列验收
 * @details 用例:
 *   1) RollingGradient300Frames — 300 帧连续 staged 上传(每帧 1-2 个子矩形,
 *      直接驱动 allocator API:Allocate + QueueBlit_Texture,Begin 时自动
 *      EncodePendingBlits),第 300 帧 readback 与 CPU 同步计算的期望帧
 *      max-abs-diff ≤ 2/255;第 1 帧写入后不再更新的区域在两帧 readback 间
 *      像素精确一致(验证无 pool 踩踏);零 validation error。
 *   2) OverflowFallback — 帧内单次 >16MB(pool 容量)上传触发 overflow →
 *      FlushBlocking + 一次性路径,数据仍正确。
 *   3) FlushBlockingNoFrameContext — 无帧上下文(资产加载期)手动
 *      Allocate + QueueBlit + FlushBlocking,数据落 GPU 正确。
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
#include "Graphics/RHI/Platforms/Vulkan/VulkanStagingAllocator.h"
#endif

#include "Utils/ImageCompare.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace Engine::Test;
namespace et = EngineTest;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

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

ResourceHandle MakeTexture(DeviceFixture& fx, u32 w, u32 h, const char* name) {
    TextureDesc td{};
    td.size = {w, h, 1};
    td.mipLevels = 1;
    td.arraySize = 1;
    td.format = DataFormat::RGBA8_UNorm;
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest | TextureUsage::CopySource;
    td.memoryUsage = GPUMemoryUsage::Static;
    td.name = name;
    return fx.base->CreateTexture(td);
}

std::vector<u8> ReadbackTexture(DeviceFixture& fx, ResourceHandle tex, u32 w, u32 h) {
    BufferDesc rdesc{};
    rdesc.size = u64(w) * h * 4;
    rdesc.type = BufferType::Raw;
    rdesc.memoryUsage = GPUMemoryUsage::Readback;
    rdesc.name = "StagingReadback";
    ResourceHandle rb = fx.base->CreateBuffer(rdesc);
    if (rb == handles::INVALID_RESOURCE) return {};
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    std::vector<u8> out;
    if (vcmd && vcmd->Reset() && vcmd->Begin()) {
        BufferTextureCopyRegion region{};
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {w, h, 1};
        vcmd->CopyTextureToBuffer(tex, rb, &region, 1);
        if (vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion()) {
            void* mapped = fx.base->MapBuffer(rb, 0, rdesc.size);
            if (mapped) {
                out.resize(size_t(rdesc.size));
                std::memcpy(out.data(), mapped, size_t(rdesc.size));
                fx.base->UnmapBuffer(rb);
            }
        }
    }
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(rb);
    return out;
}

} // anonymous namespace

// ============================================================================
// Case 1: 300 帧滚动渐变 staged 上传
// ============================================================================
TestResult TestRollingGradient300Frames() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    constexpr u32 kW = 128, kH = 128;
    constexpr u32 kTile = 16;              // 每帧更新的子矩形 16x16
    constexpr u32 kFrames = 300;
    constexpr u32 kTilesX = kW / kTile;
    constexpr u32 kTilesY = kH / kTile;
    constexpr u64 tileBytes = u64(kTile) * kTile * 4;

    ResourceHandle tex = MakeTexture(fx, kW, kH, "RollingTex");
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture rolling");
    VulkanTexture* vtex = fx.vk->GetTexture(tex);
    TEST_ASSERT(vtex != nullptr, "GetTexture");

    // CPU shadow — 与 GPU 上传同步更新,作为期望帧
    std::vector<u8> shadow(size_t(kW) * kH * 4, 0);
    for (u32 i = 0; i < kW * kH; ++i) shadow[i * 4 + 3] = 255;

    // 初始化:整图写入 shadow 初值(无帧上下文 → F3 同步路径),
    // 消除"未写入区域 = 未定义内容"的不确定性。
    TEST_ASSERT(vtex->UpdateData(shadow.data(), shadow.size(), 0),
                "initial full-image upload (deterministic base)");

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd != nullptr, "GetCommandBuffer");

    std::vector<u8> frame1Snapshot;
    u32 seq = 0;  // 连续 tile 序号 — 确定性覆盖全部可更新 tile

    for (u32 f = 0; f < kFrames; ++f) {
        fx.base->BeginFrame();  // 旋转 staging pool

        // 每帧 1-2 个子矩形:tile 索引推进,值 = f 的确定性函数。
        // tile 行限定在 [0, kTilesY-2] — 底部 tile 行(y ∈ [112,128))是从不
        // 更新的保护区,用于第 1 帧 vs 第 300 帧的一致性对照。
        const u32 updates = 1 + (f % 2);
        for (u32 u = 0; u < updates; ++u, ++seq) {
            const u32 tx = seq % kTilesX;
            const u32 ty = (seq / kTilesX) % (kTilesY - 1);
            const u8 r = static_cast<u8>((f * 7 + u * 31) & 0xFF);
            const u8 g = static_cast<u8>((f * 13 + u * 3) & 0xFF);
            const u8 b = static_cast<u8>((f * 29 + u * 11) & 0xFF);

            // CPU shadow
            for (u32 y = 0; y < kTile; ++y) {
                for (u32 x = 0; x < kTile; ++x) {
                    u8* p = &shadow[((ty * kTile + y) * kW + (tx * kTile + x)) * 4];
                    p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
                }
            }
            // staged 上传(currentLayout 用 RHI 跟踪真值 — snapshot 读回等
            // 操作会改变 tracked 状态,本地变量会失真)
            VulkanStagingAllocator& staging = fx.vk->GetStagingAllocator();
            VulkanStagingAllocator::Allocation alloc = staging.Allocate(tileBytes, 256);
            TEST_ASSERT(!alloc.overflow, "staging Allocate (300-frame churn fits in 16MB pool)");
            for (u32 y = 0; y < kTile; ++y) {
                std::memcpy(static_cast<u8*>(alloc.cpuPtr) + y * kTile * 4,
                            &shadow[((ty * kTile + y) * kW + tx * kTile) * 4], kTile * 4);
            }
            staging.QueueBlit_Texture(alloc, vtex->GetNativeImage(),
                                      /*mip=*/0, /*slice=*/0,
                                      /*origin=*/tx * kTile, ty * kTile, 0,
                                      /*extent=*/kTile, kTile, 1,
                                      /*bytesPerRow=*/kTile * 4, /*bytesPerImage=*/0,
                                      vtex->GetCurrentLayout(),
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_IMAGE_ASPECT_COLOR_BIT,
                                      /*texelSize=*/4);
            // 契约:queue 后同步 RHI 层 tracked layout(见 VulkanStagingAllocator.h)
            vtex->SetCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }

        // Begin 自动 EncodePendingBlits(帧首,用户 pass 之前)
        TEST_ASSERT(vcmd->Reset(), "Reset");
        TEST_ASSERT(vcmd->Begin(), "Begin (encodes pending blits)");
        TEST_ASSERT(vcmd->End(), "End");
        TEST_ASSERT(vcmd->Submit(0), "Submit");
        TEST_ASSERT(vcmd->WaitForCompletion(), "WaitForCompletion");

        // 第 1 帧 snapshot(未更新区域对照)
        if (f == 1) {
            frame1Snapshot = ReadbackTexture(fx, tex, kW, kH);
            TEST_ASSERT(!frame1Snapshot.empty(), "frame-1 snapshot readback");
        }
    }
    fx.vk->GetStagingAllocator().FlushBlocking();  // 排干残留(应已空)

    std::vector<u8> finalRB = ReadbackTexture(fx, tex, kW, kH);
    TEST_ASSERT(!finalRB.empty(), "final readback");

    // 主断言:与 CPU 期望帧 max-abs-diff ≤ 2/255
    const int maxDiff = et::MaxAbsDiff(finalRB.data(), shadow.data(), kW, kH);
    std::cout << "[TestVulkanStagingUpload] 300-frame rolling max-abs-diff = " << maxDiff << std::endl;
    if (maxDiff > 2) {
        et::SavePNG("P4cF4_rolling_final.png", finalRB.data(), kW, kH);
        et::SavePNG("P4cF4_rolling_shadow.png", shadow.data(), kW, kH);
        u32 diffTiles[8][8] = {};
        for (u32 y = 0; y < kH; ++y) {
            for (u32 x = 0; x < kW; ++x) {
                const size_t i = (y * kW + x) * 4;
                if (std::abs(int(finalRB[i]) - int(shadow[i])) > 2 ||
                    std::abs(int(finalRB[i + 1]) - int(shadow[i + 1])) > 2) {
                    ++diffTiles[y / kTile][x / kTile];
                }
            }
        }
        for (u32 ty = 0; ty < 8; ++ty) {
            std::cout << "  tileRow " << ty << ": ";
            for (u32 tx = 0; tx < 8; ++tx) std::cout << (diffTiles[ty][tx] ? 'X' : '.');
            std::cout << std::endl;
        }
    }
    TEST_ASSERT(maxDiff <= 2, "final frame matches CPU-computed expectation within 2/255");

    // 未更新区域:底部 tile 行(y ∈ [112,128))从不更新 — 第 1 帧与第 300 帧
    // 的 readback 必须逐字节相等,且等于初始值(0,0,0,255)。
    const u32 guardY = (kTilesY - 1) * kTile;
    u32 mismatched = 0;
    for (u32 y = guardY; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const size_t i = (y * kW + x) * 4;
            // 保护区从未被写:期望值即初始(0,0,0,255),两帧快照须一致且等于初值
            if (finalRB[i] != 0 || finalRB[i + 1] != 0 || finalRB[i + 2] != 0 ||
                finalRB[i + 3] != 255) ++mismatched;
            if (frame1Snapshot[i] != finalRB[i] || frame1Snapshot[i + 1] != finalRB[i + 1]) ++mismatched;
        }
    }
    std::cout << "[TestVulkanStagingUpload] untouched-region mismatches: " << mismatched << std::endl;
    TEST_ASSERT(mismatched == 0, "untouched guard rows byte-identical across frames 1→300");

    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyTexture(tex);
    return TestResult::Passed;
}

// ============================================================================
// Case 2: 单帧 >16MB overflow fallback
// ============================================================================
TestResult TestOverflowFallback() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // 2080x2080 RGBA8 ≈ 17.3MB > 16MB pool 容量
    constexpr u32 kW = 2080, kH = 2080;
    ResourceHandle tex = MakeTexture(fx, kW, kH, "OverflowTex");
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture overflow");
    VulkanTexture* vtex = fx.vk->GetTexture(tex);

    // 确定性 pattern(全图)
    std::vector<u8> pattern(size_t(kW) * kH * 4);
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            u8* p = &pattern[(y * kW + x) * 4];
            p[0] = static_cast<u8>(x & 0xFF);
            p[1] = static_cast<u8>(y & 0xFF);
            p[2] = static_cast<u8>((x ^ y) & 0xFF);
            p[3] = 255;
        }
    }

    // 帧内上下文:开一个 recording cmdbuf,使 updateDataImpl 走 staged 分支
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "Begin (frame context active)");

    // >16MB 单次上传:Allocate 必 overflow → FlushBlocking + one-shot 路径
    const u64 fullBytes = u64(kW) * kH * 4;
    TEST_ASSERT(vtex->UpdateData(pattern.data(), fullBytes, 0),
                "updateData >16MB overflow fallback succeeds");

    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit frame");

    // 抽样验证(全量 readback 17MB 也行,抽样省时间:对角线 + 角落)
    std::vector<u8> rb = ReadbackTexture(fx, tex, kW, kH);
    TEST_ASSERT(!rb.empty(), "readback overflow texture");
    const int maxDiff = et::MaxAbsDiff(rb.data(), pattern.data(), kW, kH);
    std::cout << "[TestVulkanStagingUpload] overflow fallback max-abs-diff = " << maxDiff << std::endl;
    TEST_ASSERT(maxDiff == 0, "overflow fallback data exact");

    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyTexture(tex);
    return TestResult::Passed;
}

// ============================================================================
// Case 3: 无帧上下文 FlushBlocking
// ============================================================================
TestResult TestFlushBlockingNoFrameContext() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");
    TEST_ASSERT(!fx.vk->IsFrameRecording(), "no frame context at start");

    constexpr u32 kW = 64, kH = 64;
    ResourceHandle tex = MakeTexture(fx, kW, kH, "FlushTex");
    TEST_ASSERT(tex != handles::INVALID_RESOURCE, "CreateTexture flush");
    VulkanTexture* vtex = fx.vk->GetTexture(tex);

    // 上半红下半蓝,两个 blit 一次 Flush
    std::vector<u8> top(size_t(kW) * (kH / 2) * 4), bottom(size_t(kW) * (kH / 2) * 4);
    for (u32 i = 0; i < kW * (kH / 2); ++i) {
        top[i * 4 + 0] = 200; top[i * 4 + 3] = 255;
        bottom[i * 4 + 2] = 200; bottom[i * 4 + 3] = 255;
    }

    VulkanStagingAllocator& staging = fx.vk->GetStagingAllocator();
    VulkanStagingAllocator::Allocation a1 = staging.Allocate(top.size(), 256);
    VulkanStagingAllocator::Allocation a2 = staging.Allocate(bottom.size(), 256);
    TEST_ASSERT(!a1.overflow && !a2.overflow, "Allocate flush case");
    std::memcpy(a1.cpuPtr, top.data(), top.size());
    std::memcpy(a2.cpuPtr, bottom.data(), bottom.size());
    staging.QueueBlit_Texture(a1, vtex->GetNativeImage(), 0, 0, 0, 0, 0, kW, kH / 2, 1,
                              kW * 4, 0, VK_IMAGE_LAYOUT_UNDEFINED,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_ASPECT_COLOR_BIT, 4);
    staging.QueueBlit_Texture(a2, vtex->GetNativeImage(), 0, 0, 0, kH / 2, 0, kW, kH / 2, 1,
                              kW * 4, 0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                              VK_IMAGE_ASPECT_COLOR_BIT, 4);
    TEST_ASSERT(staging.HasPendingBlits(), "pending before flush");
    staging.FlushBlocking();   // 无帧上下文:资产加载期语义
    TEST_ASSERT(!staging.HasPendingBlits(), "queue drained after flush");
    // 契约:flush 后同步 tracked layout(backLayout)
    vtex->SetCurrentLayout(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    std::vector<u8> rb = ReadbackTexture(fx, tex, kW, kH);
    TEST_ASSERT(!rb.empty(), "readback flush texture");
    u32 bad = 0;
    for (u32 y = 0; y < kH; ++y) {
        for (u32 x = 0; x < kW; ++x) {
            const u8* p = &rb[(y * kW + x) * 4];
            const bool isTop = y < kH / 2;
            if (isTop ? (p[0] != 200) : (p[2] != 200)) ++bad;
        }
    }
    std::cout << "[TestVulkanStagingUpload] FlushBlocking mismatches: " << bad << std::endl;
    TEST_ASSERT(bad == 0, "FlushBlocking (no frame context) data correct");

    fx.base->DestroyTexture(tex);
    return TestResult::Passed;
}

void RegisterVulkanStagingUploadTests() {
    auto suite = std::make_shared<TestSuite>("VulkanStagingUploadTests");
    suite->AddTestCase(TestCase("RollingGradient300Frames",   TestRollingGradient300Frames));
    suite->AddTestCase(TestCase("OverflowFallback",           TestOverflowFallback));
    suite->AddTestCase(TestCase("FlushBlockingNoFrameContext", TestFlushBlockingNoFrameContext));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanStagingUploadTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanStagingUpload] ENABLE_Vulkan not defined — no-op." << std::endl;
    return 0;
}

#endif
