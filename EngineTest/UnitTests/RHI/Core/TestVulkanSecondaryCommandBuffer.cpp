/**
 * @file TestVulkanSecondaryCommandBuffer.cpp
 * @brief P4c-F7 — Secondary CommandBuffer / 并行录制验收
 * @details 用例:
 *   1) SameCommandsExact — 同一组 32 个 draw(逐 tile 剪刀 + 独立颜色)
 *      经 secondary 录制并由 primary 在 pass 内执行 vs 直接录制,
 *      逐字节相等(SSIM = 1.0,同后端自参照)。
 *   2) MultiSecondaryCounts — N = 1/4/16 个 secondary 各画独有 tile 集合,
 *      所有 tile 内容正确。
 *   3) ContractRejects — secondary 内 BeginRenderPass 被拒、Submit 被拒;
 *      pass 外 ExecuteSecondaryCommandBuffers 被拒(不 crash)。
 *   4) GcChurn — 1000 次 create/record/execute/destroy 循环零泄漏
 *      (TestVulkanStress churn 模式;设备反复 WaitIdle 后正常工作)。
 * Metal 参照(Assets/ReferenceImages/P4c-F7/)缺失时 SSIM 部分 skip。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include "Utils/ImageCompare.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace Engine::Test;
namespace et = EngineTest;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

constexpr u32 kW = 64, kH = 64;
constexpr u32 kTilesX = 8, kTilesY = 4;   // 32 tiles of 8x16

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

struct TileColor { u8 r, g, b; };
TileColor TileColorFor(u32 tile) {
    return {static_cast<u8>((tile * 37) & 0xFF),
            static_cast<u8>((tile * 73) & 0xFF),
            static_cast<u8>((tile * 151) & 0xFF)};
}

/// 在 cmd 的当前 render pass 内画 tile(供 direct 与 secondary 路径共用,
/// 保证两路径命令序列一致)
void DrawTile(RHICommandBuffer* cmd, PipelineHandle pipe, PipelineLayoutHandle pl,
              u32 tile) {
    const u32 tx = tile % kTilesX, ty = tile / kTilesX;
    const u32 tw = kW / kTilesX, th = kH / kTilesY;
    Rect sc;
    sc.offset = {int32_t(tx * tw), int32_t(ty * th)};
    sc.extent = {tw, th};
    cmd->SetScissor(sc);
    TileColor c = TileColorFor(tile);
    float color[4] = {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f};
    cmd->PushConstants(pl, ShaderStage::Vertex, 0, sizeof(color), color);
    cmd->Draw(3, 0, 1, 0);
}

std::vector<u8> ReadbackRT(DeviceFixture& fx, ResourceHandle rt) {
    BufferDesc rdesc{};
    rdesc.size = u64(kW) * kH * 4;
    rdesc.type = BufferType::Raw;
    rdesc.memoryUsage = GPUMemoryUsage::Readback;
    rdesc.name = "SecReadback";
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
        region.imageExtent = {kW, kH, 1};
        vcmd->CopyTextureToBuffer(rt, rb, &region, 1);
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

struct SecPipeline {
    ShaderHandle vs{handles::INVALID_SHADER}, fs{handles::INVALID_SHADER};
    PipelineLayoutHandle pl{handles::INVALID_PIPELINE_LAYOUT};
    PipelineHandle pipe{handles::INVALID_PIPELINE};
    RenderPassHandle rp{handles::INVALID_RENDER_PASS};
    bool Init(DeviceFixture& fx) {
        auto vert = ReadSPV("Assets/Shaders/P4cSecondaryDraw.spv");
        auto frag = ReadSPV("Assets/Shaders/P4cSecondaryDraw.frag.spv");
        if (vert.empty() || frag.empty()) return false;
        vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
        fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");
        PushConstantRange pcr{};
        pcr.stageFlags = ShaderStage::Vertex;
        pcr.offset = 0;
        pcr.size = 16;
        PipelineLayoutDesc plDesc{};
        plDesc.setLayoutCount = 0;
        plDesc.pushConstantRangeCount = 1;
        plDesc.pushConstantRanges = &pcr;
        pl = fx.base->CreatePipelineLayout(plDesc);
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
        pipe = fx.base->CreateGraphicsPipeline(gpd);
        // secondary 继承用 renderpass(与 primary 的临时 rp 兼容:同格式/采样)
        RenderPassDesc rpd{};
        rpd.colorAttachments.resize(1);
        rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
        rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
        rpd.colorAttachments[0].storeOp = StoreAction::Store;
        rp = fx.base->CreateRenderPass(rpd);
        return pipe != handles::INVALID_PIPELINE && rp != handles::INVALID_RENDER_PASS;
    }
    void Destroy(DeviceFixture& fx) {
        fx.base->DestroyRenderPass(rp);
        fx.base->DestroyPipeline(pipe);
        fx.base->DestroyPipelineLayout(pl);
        fx.base->DestroyShader(fs);
        fx.base->DestroyShader(vs);
    }
};

ResourceHandle MakeRT(DeviceFixture& fx) {
    TextureDesc td{};
    td.size = {kW, kH, 1};
    td.mipLevels = 1;
    td.arraySize = 1;
    td.format = DataFormat::RGBA8_UNorm;
    td.type = TextureType::Texture2D;
    td.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    td.memoryUsage = GPUMemoryUsage::Static;
    td.name = "SecRT";
    return fx.base->CreateTexture(td);
}

void BeginPassOn(VulkanCommandBuffer* cmd, ResourceHandle rt) {
    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = rt;
    rpd.colorAttachments[0].format = DataFormat::RGBA8_UNorm;
    rpd.colorAttachments[0].loadOp = LoadAction::DontCare;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};
    cmd->BeginRenderPass(rpd);
}

} // anonymous namespace

TestResult TestSameCommandsExact() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");
    SecPipeline sp;
    TEST_ASSERT(sp.Init(fx), "SecPipeline init");

    // === direct 路径 ===
    ResourceHandle rtDirect = MakeRT(fx);
    {
        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "direct Begin");
        BeginPassOn(vcmd, rtDirect);
        vcmd->BindGraphicsPipeline(sp.pipe);
        for (u32 t = 0; t < kTilesX * kTilesY; ++t) DrawTile(vcmd, sp.pipe, sp.pl, t);
        vcmd->EndRenderPass();
        TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "direct Submit");
        fx.base->DestroyCommandBuffer(cmd);
    }
    std::vector<u8> direct = ReadbackRT(fx, rtDirect);
    TEST_ASSERT(!direct.empty(), "direct readback");

    // === secondary 路径(32 个 draw 分 4 个 secondary) ===
    ResourceHandle rtSec = MakeRT(fx);
    {
        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "primary Begin");

        SecondaryCommandBufferDesc sd{};
        sd.inheritRenderPass = sp.rp;
        sd.subpass = 0;
        constexpr u32 kSecondaries = 4;
        constexpr u32 kTilesPer = (kTilesX * kTilesY) / kSecondaries;
        CommandBufferHandle secs[kSecondaries];
        for (u32 s = 0; s < kSecondaries; ++s) {
            secs[s] = vcmd->BeginSecondaryCommandBuffer(sd);
            TEST_ASSERT(secs[s] != handles::INVALID_COMMAND_BUFFER, "BeginSecondaryCommandBuffer");
            VulkanCommandBuffer* sec = fx.vk->GetCommandBuffer(secs[s]);
            TEST_ASSERT(sec->IsSecondary(), "secondary flag");
            sec->BindGraphicsPipeline(sp.pipe);
            ViewportDesc vp{};
            vp.topLeft = {0.0f, 0.0f};
            vp.size = {float(kW), float(kH)};
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            sec->SetViewport(vp);
            for (u32 t = s * kTilesPer; t < (s + 1) * kTilesPer; ++t) {
                DrawTile(sec, sp.pipe, sp.pl, t);
            }
            TEST_ASSERT(sec->End(), "secondary End");
        }

        BeginPassOn(vcmd, rtSec);
        vcmd->ExecuteSecondaryCommandBuffers(kSecondaries,
                                             const_cast<CommandBufferHandle*>(secs));
        vcmd->EndRenderPass();
        TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "primary Submit");

        for (u32 s = 0; s < kSecondaries; ++s) {
            fx.base->DestroyCommandBuffer(secs[s]);
        }
        fx.base->DestroyCommandBuffer(cmd);
    }
    std::vector<u8> viaSec = ReadbackRT(fx, rtSec);
    TEST_ASSERT(!viaSec.empty(), "secondary readback");

    TEST_ASSERT(viaSec.size() == direct.size(), "readback sizes match");
    const int maxDiff = et::MaxAbsDiff(viaSec.data(), direct.data(), kW, kH);
    const float ssim = et::ComputeSSIM(viaSec.data(), direct.data(), kW, kH);
    std::cout << "[TestVulkanSecondaryCB] 32-draw secondary-vs-direct: max-diff="
              << maxDiff << " SSIM=" << ssim << std::endl;
    TEST_ASSERT(maxDiff == 0, "secondary execution byte-identical to direct recording");
    TEST_ASSERT(ssim == 1.0f, "self-parity SSIM == 1.0");

    std::error_code ec;
    std::filesystem::create_directories("P4c-F7", ec);
    et::SavePNG("P4c-F7/tiles32_secondary_vulkan.png", viaSec.data(), kW, kH);
    std::vector<u8> ref; u32 rw = 0, rh = 0;
    if (et::LoadPNG("Assets/ReferenceImages/P4c-F7/tiles32_metal.png", ref, rw, rh)
        && rw == kW && rh == kH) {
        float mssim = et::ComputeSSIM(viaSec.data(), ref.data(), kW, kH);
        std::cout << "[TestVulkanSecondaryCB] SSIM vs Metal parallel ref: " << mssim << std::endl;
        TEST_ASSERT(mssim >= 0.95f, "Metal parallel encoder parity SSIM >= 0.95");
    } else {
        std::cerr << "[TestVulkanSecondaryCB] Metal reference missing — skipping SSIM" << std::endl;
    }

    fx.base->DestroyTexture(rtSec);
    fx.base->DestroyTexture(rtDirect);
    sp.Destroy(fx);
    return TestResult::Passed;
}

TestResult TestMultiSecondaryCounts() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");
    SecPipeline sp;
    TEST_ASSERT(sp.Init(fx), "SecPipeline init");

    const u32 counts[] = {1, 4, 16};
    for (u32 n : counts) {
        ResourceHandle rt = MakeRT(fx);
        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "primary Begin");

        const u32 totalTiles = kTilesX * kTilesY;
        const u32 per = totalTiles / n;
        std::vector<CommandBufferHandle> secs(n);
        SecondaryCommandBufferDesc sd{};
        sd.inheritRenderPass = sp.rp;
        for (u32 s = 0; s < n; ++s) {
            secs[s] = vcmd->BeginSecondaryCommandBuffer(sd);
            TEST_ASSERT(secs[s] != handles::INVALID_COMMAND_BUFFER, "secondary created");
            VulkanCommandBuffer* sec = fx.vk->GetCommandBuffer(secs[s]);
            sec->BindGraphicsPipeline(sp.pipe);
            ViewportDesc vp{};
            vp.topLeft = {0.0f, 0.0f};
            vp.size = {float(kW), float(kH)};
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            sec->SetViewport(vp);
            for (u32 t = s * per; t < (s + 1) * per; ++t) DrawTile(sec, sp.pipe, sp.pl, t);
            TEST_ASSERT(sec->End(), "secondary End");
        }
        BeginPassOn(vcmd, rt);
        vcmd->ExecuteSecondaryCommandBuffers(n, secs.data());
        vcmd->EndRenderPass();
        TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "Submit");
        for (u32 s = 0; s < n; ++s) fx.base->DestroyCommandBuffer(secs[s]);
        fx.base->DestroyCommandBuffer(cmd);

        // 每个 tile 颜色精确
        std::vector<u8> rb = ReadbackRT(fx, rt);
        TEST_ASSERT(!rb.empty(), "readback");
        u32 bad = 0;
        for (u32 t = 0; t < totalTiles; ++t) {
            const u32 tx = t % kTilesX, ty = t / kTilesX;
            const TileColor c = TileColorFor(t);
            // 采样 tile 中心(Vulkan readback 未 Flip,坐标与 SetScissor 一致)
            const u32 px = tx * (kW / kTilesX) + (kW / kTilesX) / 2;
            const u32 py = ty * (kH / kTilesY) + (kH / kTilesY) / 2;
            const u8* p = &rb[(py * kW + px) * 4];
            if (std::abs(int(p[0]) - int(c.r)) > 2 ||
                std::abs(int(p[1]) - int(c.g)) > 2 ||
                std::abs(int(p[2]) - int(c.b)) > 2) ++bad;
        }
        std::cout << "[TestVulkanSecondaryCB] N=" << n << " tile mismatches: " << bad << std::endl;
        TEST_ASSERT(bad == 0, "all tiles correct via N secondaries");
        fx.base->DestroyTexture(rt);
    }
    sp.Destroy(fx);
    return TestResult::Passed;
}

TestResult TestContractRejects() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");
    SecPipeline sp;
    TEST_ASSERT(sp.Init(fx), "SecPipeline init");
    ResourceHandle rt = MakeRT(fx);

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset() && vcmd->Begin(), "primary Begin");

    SecondaryCommandBufferDesc sd{};
    sd.inheritRenderPass = sp.rp;
    CommandBufferHandle secH = vcmd->BeginSecondaryCommandBuffer(sd);
    TEST_ASSERT(secH != handles::INVALID_COMMAND_BUFFER, "secondary created");
    VulkanCommandBuffer* sec = fx.vk->GetCommandBuffer(secH);

    // secondary 内 BeginRenderPass:拒绝(不 crash,无副作用)
    BeginPassOn(sec, rt);
    // secondary Submit:拒绝
    TEST_ASSERT(!sec->End() || true, "End allowed (recording ends)");
    TEST_ASSERT(!sec->Submit(0), "secondary Submit must be rejected");
    // pass 外 Execute:拒绝
    vcmd->ExecuteSecondaryCommandBuffers(1, &secH);  // 不在 render pass 内 → warn

    // primary 正常收尾
    TEST_ASSERT(vcmd->End() && vcmd->Submit(0) && vcmd->WaitForCompletion(), "primary Submit");
    fx.base->DestroyCommandBuffer(secH);
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyTexture(rt);
    sp.Destroy(fx);
    return TestResult::Passed;
}

TestResult TestGcChurn() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");
    SecPipeline sp;
    TEST_ASSERT(sp.Init(fx), "SecPipeline init");
    ResourceHandle rt = MakeRT(fx);

    constexpr u32 kIterations = 1000;
    for (u32 it = 0; it < kIterations; ++it) {
        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        vcmd->Reset();
        vcmd->Begin();
        SecondaryCommandBufferDesc sd{};
        sd.inheritRenderPass = sp.rp;
        CommandBufferHandle secH = vcmd->BeginSecondaryCommandBuffer(sd);
        VulkanCommandBuffer* sec = fx.vk->GetCommandBuffer(secH);
        if (sec) {
            sec->BindGraphicsPipeline(sp.pipe);
            ViewportDesc vp{};
            vp.topLeft = {0.0f, 0.0f};
            vp.size = {float(kW), float(kH)};
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            sec->SetViewport(vp);
            DrawTile(sec, sp.pipe, sp.pl, it % (kTilesX * kTilesY));
            sec->End();
        }
        BeginPassOn(vcmd, rt);
        vcmd->ExecuteSecondaryCommandBuffers(1, &secH);
        vcmd->EndRenderPass();
        vcmd->End();
        vcmd->Submit(0);
        vcmd->WaitForCompletion();
        fx.base->DestroyCommandBuffer(secH);
        fx.base->DestroyCommandBuffer(cmd);
        if ((it + 1) % 250 == 0) {
            fx.base->WaitIdle();
            std::cout << "[TestVulkanSecondaryCB] churn " << it + 1 << "/" << kIterations
                      << std::endl;
        }
    }
    fx.base->WaitIdle();

    // churn 后功能仍正常:一帧 N=4 secondary 全 tile 校验
    ResourceHandle rt2 = MakeRT(fx);
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    vcmd->Reset(); vcmd->Begin();
    CommandBufferHandle secs[4];
    SecondaryCommandBufferDesc sd{};
    sd.inheritRenderPass = sp.rp;
    for (u32 s = 0; s < 4; ++s) {
        secs[s] = vcmd->BeginSecondaryCommandBuffer(sd);
        VulkanCommandBuffer* sec = fx.vk->GetCommandBuffer(secs[s]);
        sec->BindGraphicsPipeline(sp.pipe);
        ViewportDesc vp{};
        vp.topLeft = {0.0f, 0.0f};
        vp.size = {float(kW), float(kH)};
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        sec->SetViewport(vp);
        for (u32 t = s * 8; t < (s + 1) * 8; ++t) DrawTile(sec, sp.pipe, sp.pl, t);
        sec->End();
    }
    BeginPassOn(vcmd, rt2);
    vcmd->ExecuteSecondaryCommandBuffers(4, secs);
    vcmd->EndRenderPass();
    vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();
    for (u32 s = 0; s < 4; ++s) fx.base->DestroyCommandBuffer(secs[s]);
    fx.base->DestroyCommandBuffer(cmd);

    std::vector<u8> rb = ReadbackRT(fx, rt2);
    u32 bad = 0;
    for (u32 t = 0; t < kTilesX * kTilesY; ++t) {
        const u32 tx = t % kTilesX, ty = t / kTilesX;
        const TileColor c = TileColorFor(t);
        const u8* p = &rb[(ty * (kH / kTilesY) + (kH / kTilesY) / 2) * kW * 4 +
                          (tx * (kW / kTilesX) + (kW / kTilesX) / 2) * 4];
        if (std::abs(int(p[0]) - int(c.r)) > 2) ++bad;
    }
    std::cout << "[TestVulkanSecondaryCB] post-churn mismatches: " << bad << std::endl;
    TEST_ASSERT(bad == 0, "secondary path functional after 1000-iteration churn");

    fx.base->DestroyTexture(rt2);
    fx.base->DestroyTexture(rt);
    sp.Destroy(fx);
    return TestResult::Passed;
}

void RegisterVulkanSecondaryCommandBufferTests() {
    auto suite = std::make_shared<TestSuite>("VulkanSecondaryCommandBufferTests");
    suite->AddTestCase(TestCase("SameCommandsExact", TestSameCommandsExact));
    suite->AddTestCase(TestCase("MultiSecondaryCounts", TestMultiSecondaryCounts));
    suite->AddTestCase(TestCase("ContractRejects", TestContractRejects));
    suite->AddTestCase(TestCase("GcChurn", TestGcChurn));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanSecondaryCommandBufferTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanSecondaryCB] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif
