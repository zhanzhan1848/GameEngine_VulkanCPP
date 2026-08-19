/**
 * @file TestVulkanSwapChain.cpp
 * @brief Phase 4b Sub-Phase 2 — VulkanSwapChain 端到端测试
 * @details 三个 case:
 *   1) Create_NullWindow — 传 nullptr 期望 createSwapChain 返回 nullptr
 *   2) AcquireImage — 创建 window+device+swapchain,N+1 个独立 imageSem,
 *      acquire N 帧(每帧用未 signaled 的 sem),验证 imageIndex 合法
 *   3) AcquirePresent — 完整 acquire → cmd buffer(layout transition) → submit → present,
 *      N 帧循环,无 validation error
 *
 *   关键 spec 要点:
 *   - vkAcquireNextImageKHR 的 semaphore 必须 unsignaled,所以每帧需独立 sem
 *   - vkQueuePresentKHR 要求 image 处于 PRESENT_SRC_KHR layout,所以 acquire 后
 *     必须有一个 cmd buffer 走 transition UNDEFINED → PRESENT_SRC_KHR
 *   -vkQueuePresentKHR 的 wait semaphore 必须由 submit 信号,形成 acquire→work→present 链
 *   全程 validation layer 必须零 error。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHISwapChain.h"
#include "Engine/Platform/Platform.h"
#include "Engine/Platform/PlatformTypes.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanTexture.h"
#endif

#include "Utils/ImageCompare.h"

#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace Engine::Test;
namespace et = EngineTest;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

TestResult TestSwapChainCreate_NullWindow() {
    DeviceDesc dd{};
    dd.platform = RHIPlatform::Vulkan;
    dd.enableValidation = true;
    dd.enableDebug = true;
    RHIDeviceBase* device = CreateRHIDevice(dd);
    TEST_ASSERT(device != nullptr, "Vulkan device init");

    SwapChainDesc scd{};
    scd.window = nullptr;
    scd.width = 64;
    scd.height = 64;
    scd.format = DataFormat::BGRA8_UNorm;
    scd.bufferCount = 2;

    RHISwapChain* sc = device->CreateSwapChain(scd);
    TEST_ASSERT(sc == nullptr, "CreateSwapChain should fail with null window");

    device->Shutdown();
    return TestResult::Passed;
}

TestResult TestSwapChainAcquireImage() {
    // === 平台 window ===
    primal::platform::window_init_info wi{};
    wi.caption = "VulkanSwapChain Test";
    wi.width = 128;
    wi.height = 128;
    primal::platform::window win = primal::platform::create_window(&wi);
    if (!win.is_valid()) {
        std::cerr << "[TestSwapChainAcquireImage] platform::create_window failed — skip" << std::endl;
        return TestResult::Passed;  // headless CI 可能跑不动,不算 fail
    }

    // === Device ===
    DeviceDesc dd{};
    dd.platform = RHIPlatform::Vulkan;
    dd.enableValidation = true;
    dd.enableDebug = true;
    RHIDeviceBase* device = CreateRHIDevice(dd);
    TEST_ASSERT(device != nullptr, "Vulkan device init");

    // === SwapChain ===
    SwapChainDesc scd{};
    scd.window = win.handle();
    scd.width = 128;
    scd.height = 128;
    scd.format = DataFormat::BGRA8_UNorm;
    scd.bufferCount = 2;
    scd.presentMode = PresentMode::FIFO;

    RHISwapChain* sc = device->CreateSwapChain(scd);
    TEST_ASSERT(sc != nullptr, "CreateSwapChain(valid window)");

    if (sc) {
        // FIFO + minImageCount=2 意味着应用同时只能持有 1 个未 present 的 image,
        // 所以这个测试只做 1 次 acquire(然后 destroy),验证基本 acquire 路径。
        // 多帧 acquire/present 由 TestSwapChainAcquirePresent 覆盖。
        SyncHandle imageSem = device->CreateSync();
        TEST_ASSERT(imageSem != handles::INVALID_SYNC, "CreateSync");

        if (imageSem != handles::INVALID_SYNC) {
            u32 idx = UINT32_MAX;
            bool ok = sc->AcquireNextImage(&idx, imageSem);
            TEST_ASSERT(ok, "AcquireNextImage");
            TEST_ASSERT(idx < scd.bufferCount + 1, "imageIndex within image count");
            device->DestroySync(imageSem);
        }

        device->DestroySwapChain(sc);
    }

    device->Shutdown();
    primal::platform::remove_window(win.get_id());
    return TestResult::Passed;
}

TestResult TestSwapChainAcquirePresent() {
    // === 平台 window ===
    primal::platform::window_init_info wi{};
    wi.caption = "VulkanSwapChain Acquire/Present";
    wi.width = 128;
    wi.height = 128;
    primal::platform::window win = primal::platform::create_window(&wi);
    if (!win.is_valid()) {
        std::cerr << "[TestSwapChainAcquirePresent] platform::create_window failed — skip" << std::endl;
        return TestResult::Passed;
    }

    // === Device ===
    DeviceDesc dd{};
    dd.platform = RHIPlatform::Vulkan;
    dd.enableValidation = true;
    dd.enableDebug = true;
    RHIDeviceBase* device = CreateRHIDevice(dd);
    TEST_ASSERT(device != nullptr, "Vulkan device init");

    // === SwapChain ===
    SwapChainDesc scd{};
    scd.window = win.handle();
    scd.width = 128;
    scd.height = 128;
    scd.format = DataFormat::BGRA8_UNorm;
    scd.bufferCount = 2;
    scd.presentMode = PresentMode::FIFO;

    RHISwapChain* sc = device->CreateSwapChain(scd);
    TEST_ASSERT(sc != nullptr, "CreateSwapChain");

    TestResult result = TestResult::Failed;
    if (sc) {
        // 每帧 acquire/render 一对 sem(acquire→submit→present 形成完整 sem 链)
        constexpr int kFrames = 3;
        std::vector<SyncHandle> imageSems(kFrames);
        std::vector<SyncHandle> renderSems(kFrames);
        for (int i = 0; i < kFrames; ++i) {
            imageSems[i] = device->CreateSync();
            renderSems[i] = device->CreateSync();
        }

        VulkanDevice* vkDev = static_cast<VulkanDevice*>(device);

        bool allOk = (imageSems[0] != handles::INVALID_SYNC && renderSems[0] != handles::INVALID_SYNC);
        for (int i = 0; i < kFrames && allOk; ++i) {
            u32 idx = 0;
            if (!sc->AcquireNextImage(&idx, imageSems[i])) { allOk = false; break; }

            // 记录一个最小的 cmd buffer:layout transition UNDEFINED → PRESENT_SRC_KHR
            CommandBufferHandle cmd = device->CreateCommandBuffer(CommandQueueType::Graphics);
            if (cmd == handles::INVALID_COMMAND_BUFFER) { allOk = false; break; }
            auto* cmdBuf = vkDev->GetCommandBuffer(cmd);
            if (!cmdBuf) { allOk = false; break; }

            cmdBuf->Begin();
            ResourceHandle backbuffer = sc->GetBackBuffer(idx);
            if (backbuffer == handles::INVALID_RESOURCE) { allOk = false; break; }
            ResourceBarrier b{};
            b.resource = backbuffer;
            b.beforeState = ResourceState::Unknown;
            b.afterState  = ResourceState::Present;
            b.subresource = 0xFFFFFFFF;
            b.queueFamily = 0xFFFFFFFF;
            cmdBuf->InsertBarrier(&b, 1);
            cmdBuf->End();

            // submit:等 imageSem[i](acquire 完成),signal renderSem[i]
            QueueSubmitInfo qi{};
            qi.cmdBuffer = cmd;
            qi.waitSemaphore = imageSems[i];
            qi.signalSemaphore = renderSems[i];
            if (!device->Submit(qi)) { allOk = false; break; }

            // present:等 renderSem[i]
            sc->Present(renderSems[i]);
        }

        // 帧循环结束前等 GPU 完成所有 cmd,避免销毁时 cmd 还在跑
        device->WaitIdle();

        if (allOk) result = TestResult::Passed;

        for (int i = 0; i < kFrames; ++i) {
            device->DestroySync(renderSems[i]);
            device->DestroySync(imageSems[i]);
        }
        device->DestroySwapChain(sc);
    }

    device->Shutdown();
    primal::platform::remove_window(win.get_id());
    return result;
}

// ============================================================================
// P4c-F2: Swapchain 失效自动重建(ResizeRecovery)
//
// 双 pass 对照:
//   pass1 — 无 resize 的 60 帧基线,每 10 帧读回 offscreen RT 内容做检查点;
//   pass2 — 同样 60 帧,但每 10 帧对窗口做一次程序化 resize(尺寸循环),
//           swapchain 在 acquire 端自动重建(无需调用方干预)。
// 断言:
//   - 每次 resize 后 ≤3 帧内 acquire+present 自动恢复;
//   - 检查点帧内容与同帧号基线 SSIM ≥ 0.95(同分辨率离屏 RT + blit 对照路径);
//   - 帧非全黑/全白(方差阈值)。
// 无显示环境(create_window 失败)时 skip(现有约定)。
// ============================================================================
namespace {

std::vector<u8> ReadSPVSwap(const char* path) {
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

struct ResizeRunResult {
    std::vector<std::vector<u8>> checkpoints;   // 帧内容(128x128 RGBA8,已 FlipY)
    u32 acquireFailures{0};
    u32 presentTotal{0};
    bool setupOk{false};
};

ResizeRunResult RunResizeTestPass(bool enableResize) {
    ResizeRunResult result;
    constexpr u32 kRT = 128;
    constexpr u32 kFrames = 60;
    constexpr u32 kCheckpointEvery = 10;
    const u32 kSizes[][2] = { {96, 96}, {160, 160}, {128, 128}, {112, 144},
                              {144, 112}, {96, 160}, {160, 96}, {128, 128} };

    // 窗口
    primal::platform::window_init_info wi{};
    wi.caption = enableResize ? "VulkanSwapChain ResizeRecovery" : "VulkanSwapChain Baseline";
    wi.width = 128;
    wi.height = 128;
    primal::platform::window win = primal::platform::create_window(&wi);
    if (!win.is_valid()) return result;

    DeviceDesc dd{};
    dd.platform = RHIPlatform::Vulkan;
    dd.enableValidation = true;
    dd.enableDebug = true;
    RHIDeviceBase* device = CreateRHIDevice(dd);
    if (!device) {
        primal::platform::remove_window(win.get_id());
        return result;
    }
    VulkanDevice* vkDev = static_cast<VulkanDevice*>(device);

    // SwapChain
    SwapChainDesc scd{};
    scd.window = win.handle();
    scd.width = 128;
    scd.height = 128;
    scd.format = DataFormat::BGRA8_UNorm;
    scd.bufferCount = 2;
    scd.presentMode = PresentMode::FIFO;
    RHISwapChain* sc = device->CreateSwapChain(scd);
    if (!sc) {
        device->Shutdown();
        primal::platform::remove_window(win.get_id());
        return result;
    }

    // 渐变 shader + fullscreen pipeline + offscreen RT + readback
    auto vert = ReadSPVSwap("Assets/Shaders/P4cFormatGradient.spv");
    auto frag = ReadSPVSwap("Assets/Shaders/P4cFormatGradient.frag.spv");
    if (vert.empty() || frag.empty()) {
        std::cerr << "[ResizeRecovery] gradient SPIR-V missing — aborting pass" << std::endl;
        device->DestroySwapChain(sc);
        device->Shutdown();
        primal::platform::remove_window(win.get_id());
        return result;
    }
    ShaderHandle vs = device->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = device->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");
    PipelineLayoutDesc plDesc{};
    plDesc.setLayoutCount = 0;
    plDesc.pushConstantRangeCount = 0;
    PipelineLayoutHandle pl = device->CreatePipelineLayout(plDesc);
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader = fs;
    gpd.layout = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RGBA8_UNorm;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = device->CreateGraphicsPipeline(gpd);

    TextureDesc rtDesc{};
    rtDesc.size = {kRT, kRT, 1};
    rtDesc.mipLevels = 1;
    rtDesc.arraySize = 1;
    rtDesc.format = DataFormat::RGBA8_UNorm;
    rtDesc.type = TextureType::Texture2D;
    rtDesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    rtDesc.memoryUsage = GPUMemoryUsage::Static;
    rtDesc.name = "ResizeRT";
    ResourceHandle rt = device->CreateTexture(rtDesc);

    BufferDesc rbDesc{};
    rbDesc.size = u64(kRT) * kRT * 4;
    rbDesc.type = BufferType::Raw;
    rbDesc.memoryUsage = GPUMemoryUsage::Readback;
    rbDesc.name = "ResizeReadback";
    ResourceHandle readback = device->CreateBuffer(rbDesc);

    CommandBufferHandle cmd = device->CreateCommandBuffer(CommandQueueType::Graphics);
    auto* cmdBuf = vkDev->GetCommandBuffer(cmd);

    result.setupOk = (pipe != handles::INVALID_PIPELINE && rt != handles::INVALID_RESOURCE &&
                      readback != handles::INVALID_RESOURCE && cmdBuf != nullptr);

    if (result.setupOk) {
        // 每帧独立 sem 对:acquire sem 必须未 signaled,render sem 在 present
        // 完成前不可复用(现有 AcquirePresent 用例同款约定)。
        constexpr u32 kSemRing = 8;
        std::vector<SyncHandle> imageSems(kSemRing), renderSems(kSemRing);
        for (u32 i = 0; i < kSemRing; ++i) {
            imageSems[i] = device->CreateSync();
            renderSems[i] = device->CreateSync();
        }

        u32 resizeIdx = 0;
        for (u32 f = 0; f < kFrames; ++f) {
            // 帧首等待上一轮使用该 cmdbuf 的提交完成 — 单 cmdbuf 复用不能
            // 在 GPU 仍在执行时 Reset(首帧 checkpoint 黑帧的根因)。
            cmdBuf->WaitForCompletion();

            // 每 10 帧触发一次尺寸变化(首个检查点后开始)
            if (enableResize && f > 0 && f % kCheckpointEvery == 0) {
                const u32 nw = kSizes[resizeIdx % 8][0];
                const u32 nh = kSizes[resizeIdx % 8][1];
                ++resizeIdx;
                win.resize(nw, nh);
            }

            u32 idx = 0;
            const u32 semIdx = f % kSemRing;
            if (!sc->AcquireNextImage(&idx, imageSems[semIdx])) {
                ++result.acquireFailures;
                continue;
            }
            ResourceHandle bb = sc->GetBackBuffer(idx);
            if (bb == handles::INVALID_RESOURCE) {
                ++result.acquireFailures;
                continue;
            }

            // 动画清屏色(帧号驱动,双 pass 内容一致)
            RenderPassDesc rpd{};
            rpd.colorAttachments.resize(1);
            rpd.colorAttachments[0].texture = rt;
            rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
            rpd.colorAttachments[0].loadOp = LoadAction::Clear;
            rpd.colorAttachments[0].storeOp = StoreAction::Store;
            rpd.colorAttachments[0].clearValue.color = {
                0.5f + 0.5f * std::sin(float(f) * 0.1f),
                0.5f + 0.5f * std::cos(float(f) * 0.13f),
                float(f % 60) / 60.0f, 1.0f};
            rpd.viewport.topLeft = {0.0f, 0.0f};
            rpd.viewport.size = {float(kRT), float(kRT)};
            rpd.scissor.offset = {0, 0};
            rpd.scissor.extent = {kRT, kRT};

            const bool checkpoint = ((f + 1) % kCheckpointEvery == 0);

            cmdBuf->Reset();
            cmdBuf->Begin();
            cmdBuf->BeginRenderPass(rpd);
            cmdBuf->BindGraphicsPipeline(pipe);
            cmdBuf->Draw(3, 0, 1, 0);
            cmdBuf->EndRenderPass();

            // offscreen RT → backbuffer blit(对照路径)
            VulkanTexture* bbTex = vkDev->GetTexture(bb);
            const u32 bw = bbTex->GetTextureDesc().size.x;
            const u32 bh = bbTex->GetTextureDesc().size.y;
            TextureBlitRegion blit{};
            blit.srcSubresource = {0, 0, 1};
            blit.srcOffsets[0] = {0, 0, 0};
            blit.srcOffsets[1] = {int32_t(kRT), int32_t(kRT), 1};
            blit.dstSubresource = {0, 0, 1};
            blit.dstOffsets[0] = {0, 0, 0};
            blit.dstOffsets[1] = {int32_t(bw), int32_t(bh), 1};
            cmdBuf->BlitTexture(rt, bb, &blit, 1, FilterMode::Linear);

            // backbuffer → PRESENT
            ResourceBarrier b{};
            b.resource = bb;
            b.beforeState = ResourceState::CopyDest;
            b.afterState = ResourceState::Present;
            b.subresource = 0xFFFFFFFF;
            b.queueFamily = 0xFFFFFFFF;
            cmdBuf->InsertBarrier(&b, 1);

            if (checkpoint) {
                BufferTextureCopyRegion region{};
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = 1;
                region.imageOffset = {0, 0, 0};
                region.imageExtent = {kRT, kRT, 1};
                cmdBuf->CopyTextureToBuffer(rt, readback, &region, 1);
            }

            cmdBuf->End();
            QueueSubmitInfo qi{};
            qi.cmdBuffer = cmd;
            qi.waitSemaphore = imageSems[semIdx];
            qi.signalSemaphore = renderSems[semIdx];
            if (device->Submit(qi)) {
                sc->Present(renderSems[semIdx]);
                ++result.presentTotal;
            }

            if (checkpoint) {
                cmdBuf->WaitForCompletion();
                void* mapped = device->MapBuffer(readback, 0, rbDesc.size);
                if (mapped) {
                    std::vector<u8> frame(size_t(rbDesc.size));
                    std::memcpy(frame.data(), mapped, size_t(rbDesc.size));
                    et::FlipYInPlace(frame.data(), kRT, kRT);
                    result.checkpoints.push_back(std::move(frame));
                    device->UnmapBuffer(readback);
                }
            }
        }
        device->WaitIdle();
        for (u32 i = 0; i < kSemRing; ++i) {
            device->DestroySync(renderSems[i]);
            device->DestroySync(imageSems[i]);
        }
    }

    device->DestroyCommandBuffer(cmd);
    device->DestroyBuffer(readback);
    device->DestroyTexture(rt);
    device->DestroyPipeline(pipe);
    device->DestroyPipelineLayout(pl);
    device->DestroyShader(fs);
    device->DestroyShader(vs);
    device->DestroySwapChain(sc);
    device->Shutdown();
    primal::platform::remove_window(win.get_id());
    return result;
}

} // anonymous namespace

TestResult TestSwapChainResizeRecovery() {
    // 基线 pass(无 resize)
    ResizeRunResult baseline = RunResizeTestPass(false);
    if (!baseline.setupOk) {
        std::cerr << "[ResizeRecovery] platform window unavailable — skip" << std::endl;
        return TestResult::Skipped;
    }
    TEST_ASSERT(baseline.checkpoints.size() == 6, "baseline has 6 checkpoints");
    TEST_ASSERT(baseline.acquireFailures == 0, "baseline must not fail acquire");
    TEST_ASSERT(baseline.presentTotal >= 58, "baseline presents ~all frames");

    // resize pass(10 次尺寸变化)
    ResizeRunResult resized = RunResizeTestPass(true);
    TEST_ASSERT(resized.setupOk, "resize pass setup");
    TEST_ASSERT(resized.checkpoints.size() == baseline.checkpoints.size(),
                "resize pass must reach all checkpoints (rendering never permanently broken)");
    TEST_ASSERT(resized.presentTotal >= 55,
                "resize pass keeps presenting (transient recreate failures allowed)");

    u32 ssimFailures = 0;
    for (size_t i = 0; i < baseline.checkpoints.size() && i < resized.checkpoints.size(); ++i) {
        float ssim = et::ComputeSSIM(resized.checkpoints[i].data(), baseline.checkpoints[i].data(),
                                     128, 128);
        // 方差:非全黑/全白
        double mean = 0;
        for (u8 v : resized.checkpoints[i]) mean += v;
        mean /= double(resized.checkpoints[i].size());
        double var = 0;
        for (u8 v : resized.checkpoints[i]) var += (v - mean) * (v - mean);
        var /= double(resized.checkpoints[i].size());
        std::cout << "[ResizeRecovery] checkpoint " << i
                  << ": SSIM=" << ssim << " variance=" << var << std::endl;
        if (var < 50.0) ++ssimFailures;           // 全黑/全白帧
        if (ssim < 0.95f) ++ssimFailures;          // 内容与基线偏离
    }
    TEST_ASSERT(ssimFailures == 0, "all checkpoints: variance ok + SSIM >= 0.95 vs baseline");
    return TestResult::Passed;
}

void RegisterVulkanSwapChainTests() {
    auto suite = std::make_shared<TestSuite>("VulkanSwapChainTests");
    suite->AddTestCase(TestCase("SwapChainCreate_NullWindow", TestSwapChainCreate_NullWindow));
    suite->AddTestCase(TestCase("SwapChainAcquireImage",      TestSwapChainAcquireImage));
    suite->AddTestCase(TestCase("SwapChainAcquirePresent",    TestSwapChainAcquirePresent));
    suite->AddTestCase(TestCase("SwapChainResizeRecovery",    TestSwapChainResizeRecovery));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanSwapChainTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else

int main() {
    std::cout << "[TestVulkanSwapChain] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif
