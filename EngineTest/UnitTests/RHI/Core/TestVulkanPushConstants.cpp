/**
 * @file TestVulkanPushConstants.cpp
 * @brief Phase 5 PushConstants + Shader HotReload 集成测试
 * @details 验证:
 *          1) PushConstants 在 graphics pipeline 中正确传值 (vec4 color)
 *          2) ReloadShader 触发依赖 pipeline Recreate,新 SPIR-V 立即生效
 *          整条 pipeline: Shader → PipelineLayout(push_constant) → Pipeline →
 *                        BeginRenderPass → BindPipeline → PushConstants → Draw →
 *                        CopyTextureToBuffer → readback 校验像素
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIRenderPass.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

std::vector<u8> ReadSPV(const char* path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        std::cerr << "ReadSPV: cannot open " << path << std::endl;
        return {};
    }
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> data(sz);
    if (sz > 0) f.read(reinterpret_cast<char*>(data.data()), sz);
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

struct Frame {
    ResourceHandle rt{handles::INVALID_RESOURCE};
    ResourceHandle readback{handles::INVALID_RESOURCE};
    constexpr static u32 kW = 64, kH = 64;
    void Init(RHIDeviceBase* d) {
        TextureDesc tdesc{};
        tdesc.size = { kW, kH, 1 };
        tdesc.mipLevels = 1;
        tdesc.arraySize = 1;
        tdesc.format = DataFormat::RG8B8A8_UNorm;
        tdesc.type = TextureType::Texture2D;
        tdesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
        tdesc.memoryUsage = GPUMemoryUsage::Static;
        tdesc.name = "RT";
        rt = d->CreateTexture(tdesc);

        BufferDesc bdesc{};
        bdesc.size = u64(kW) * u64(kH) * 4;
        bdesc.type = BufferType::Raw;
        bdesc.memoryUsage = GPUMemoryUsage::Readback;
        bdesc.name = "Readback";
        readback = d->CreateBuffer(bdesc);
    }
    void Destroy(RHIDeviceBase* d) {
        if (readback != handles::INVALID_RESOURCE) d->DestroyBuffer(readback);
        if (rt != handles::INVALID_RESOURCE) d->DestroyTexture(rt);
    }
};

/// 读中心像素
u32 ReadCenterPixel(void* mapped) {
    const u8* p = reinterpret_cast<const u8*>(mapped);
    u32 cx = Frame::kW / 2, cy = Frame::kH / 2;
    u32 idx = (cy * Frame::kW + cx) * 4;
    return (u32(p[idx + 3]) << 24) | (u32(p[idx + 2]) << 16) | (u32(p[idx + 1]) << 8) | u32(p[idx]);
}

} // anonymous namespace

/// PushConstants: 传入 vec4(1,0,0,1) (红), 验证中心像素 ≈ (255,0,0,255)
TestResult TestPushConstantsRed() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto vert = ReadSPV("Assets/Shaders/SPIRV/pc_triangle.vert.spv");
    auto frag = ReadSPV("Assets/Shaders/SPIRV/pc_triangle.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read SPIR-V");

    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader vs");
    TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader fs");

    // PipelineLayout with push constant range (vec4 color, vert stage, 16B)
    PushConstantRange range{};
    range.stageFlags = ShaderStage::Vertex;
    range.offset = 0;
    range.size = 16;
    PipelineLayoutDesc pld{};
    pld.setLayoutCount = 0;
    pld.pushConstantRangeCount = 1;
    pld.pushConstantRanges = &range;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(pld);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader  = fs;
    gpd.layout       = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RG8B8A8_UNorm;
    gpd.depthStencilFormat = DataFormat::Unknown;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = fx.base->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline");

    Frame frame;
    frame.Init(fx.base);

    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = frame.rt;
    rpd.colorAttachments[0].format = DataFormat::RG8B8A8_UNorm;
    rpd.colorAttachments[0].loadOp = LoadAction::Clear;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.colorAttachments[0].clearValue.color = primal::math::v4{0.0f, 0.0f, 0.0f, 1.0f};
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(Frame::kW), float(Frame::kH)};
    rpd.viewport.minDepth = 0.0f;
    rpd.viewport.maxDepth = 1.0f;
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {Frame::kW, Frame::kH};

    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd->Reset(), "Reset");
    TEST_ASSERT(vcmd->Begin(), "Begin");
    vcmd->BeginRenderPass(rpd);
    vcmd->BindGraphicsPipeline(pipe);
    primal::math::v4 color{1.0f, 0.0f, 0.0f, 1.0f};  // red
    vcmd->PushConstants(pl, ShaderStage::Vertex, 0, 16, &color);
    vcmd->Draw(3, 0, 1, 0);
    vcmd->EndRenderPass();
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {Frame::kW, Frame::kH, 1};
    vcmd->CopyTextureToBuffer(frame.rt, frame.readback, &region, 1);
    TEST_ASSERT(vcmd->End(), "End");
    TEST_ASSERT(vcmd->Submit(0), "Submit");
    TEST_ASSERT(vcmd->WaitForCompletion(), "Wait");

    void* mapped = fx.base->MapBuffer(frame.readback, 0, u64(Frame::kW) * Frame::kH * 4);
    TEST_ASSERT(mapped, "MapBuffer");
    u32 pixel = ReadCenterPixel(mapped);
    fx.base->UnmapBuffer(frame.readback);
    std::cout << "[PushConstantsRed] center pixel RGBA8 = 0x" << std::hex << pixel << std::dec << std::endl;

    // Allow gamma/precision slop: red channel ≥ 200, others ≤ 50
    u8 r = pixel & 0xFF, g = (pixel >> 8) & 0xFF, b = (pixel >> 16) & 0xFF, a = (pixel >> 24) & 0xFF;
    TEST_ASSERT(r >= 200, "red channel dominant");
    TEST_ASSERT(g <= 50,  "green suppressed");
    TEST_ASSERT(b <= 50,  "blue suppressed");
    TEST_ASSERT(a >= 200, "alpha high");

    fx.base->DestroyCommandBuffer(cmd);
    frame.Destroy(fx.base);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

/// Hot reload: render with pc_triangle.vert (color = input), reload pc_triangle_v2.vert (swap r/g),
/// render again, verify pixel r/g swapped
TestResult TestShaderHotReload() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto v1 = ReadSPV("Assets/Shaders/SPIRV/pc_triangle.vert.spv");
    auto v2 = ReadSPV("Assets/Shaders/SPIRV/pc_triangle_v2.vert.spv");
    auto frag = ReadSPV("Assets/Shaders/SPIRV/pc_triangle.frag.spv");
    TEST_ASSERT(!v1.empty() && !v2.empty() && !frag.empty(), "Read SPIR-V");

    ShaderHandle vs = fx.base->CreateShader(v1.data(), v1.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader v1");
    TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader fs");

    PushConstantRange range{};
    range.stageFlags = ShaderStage::Vertex;
    range.offset = 0;
    range.size = 16;
    PipelineLayoutDesc pld{};
    pld.pushConstantRangeCount = 1;
    pld.pushConstantRanges = &range;
    PipelineLayoutHandle pl = fx.base->CreatePipelineLayout(pld);
    TEST_ASSERT(pl != handles::INVALID_PIPELINE_LAYOUT, "CreatePipelineLayout");

    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader  = fs;
    gpd.layout       = pl;
    gpd.topology = PrimitiveTopology::TriangleList;
    gpd.fillMode = FillMode::Solid;
    gpd.cullMode = CullMode::None;
    gpd.renderTargetCount = 1;
    gpd.renderTargetFormats[0] = DataFormat::RG8B8A8_UNorm;
    gpd.depthStencilFormat = DataFormat::Unknown;
    gpd.enableDepthTest = false;
    gpd.enableDepthWrite = false;
    PipelineHandle pipe = fx.base->CreateGraphicsPipeline(gpd);
    TEST_ASSERT(pipe != handles::INVALID_PIPELINE, "CreateGraphicsPipeline");

    Frame frame;
    frame.Init(fx.base);
    constexpr u64 kBufSize = u64(Frame::kW) * Frame::kH * 4;

    auto Render = [&](primal::math::v4 const& c) -> u32 {
        RenderPassDesc rpd{};
        rpd.colorAttachments.resize(1);
        rpd.colorAttachments[0].texture = frame.rt;
        rpd.colorAttachments[0].format = DataFormat::RG8B8A8_UNorm;
        rpd.colorAttachments[0].loadOp = LoadAction::Clear;
        rpd.colorAttachments[0].storeOp = StoreAction::Store;
        rpd.colorAttachments[0].clearValue.color = primal::math::v4{0,0,0,1};
        rpd.viewport.topLeft = {0,0};
        rpd.viewport.size = {float(Frame::kW), float(Frame::kH)};
        rpd.viewport.minDepth = 0;
        rpd.viewport.maxDepth = 1;
        rpd.scissor.offset = {0,0};
        rpd.scissor.extent = {Frame::kW, Frame::kH};

        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        vcmd->Reset(); vcmd->Begin();
        vcmd->BeginRenderPass(rpd);
        vcmd->BindGraphicsPipeline(pipe);
        vcmd->PushConstants(pl, ShaderStage::Vertex, 0, 16, &c);
        vcmd->Draw(3, 0, 1, 0);
        vcmd->EndRenderPass();
        BufferTextureCopyRegion region{};
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {Frame::kW, Frame::kH, 1};
        vcmd->CopyTextureToBuffer(frame.rt, frame.readback, &region, 1);
        vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();
        void* mapped = fx.base->MapBuffer(frame.readback, 0, kBufSize);
        u32 p = ReadCenterPixel(mapped);
        fx.base->UnmapBuffer(frame.readback);
        fx.base->DestroyCommandBuffer(cmd);
        return p;
    };

    // Input color = vec4(1, 0.2, 0.2, 1) — v1 outputs (R=high, G=low, B=low)
    u32 before = Render(primal::math::v4{1.0f, 0.2f, 0.2f, 1.0f});
    u8 rB = before & 0xFF, gB = (before >> 8) & 0xFF;
    std::cout << "[HotReload before] RGBA = (r=" << int(rB) << ",g=" << int(gB) << ")" << std::endl;
    TEST_ASSERT(rB > 200 && gB < 100, "v1: red dominant, green suppressed");

    // Reload vertex shader → v2 swaps R and G channels
    bool ok = fx.base->ReloadShader(vs, v2.data(), v2.size());
    TEST_ASSERT(ok, "ReloadShader returns true");
    fx.base->WaitIdle();

    u32 after = Render(primal::math::v4{1.0f, 0.2f, 0.2f, 1.0f});
    u8 rA = after & 0xFF, gA = (after >> 8) & 0xFF;
    std::cout << "[HotReload after]  RGBA = (r=" << int(rA) << ",g=" << int(gA) << ")" << std::endl;
    // After v2: R should be low (was green=0.2*255=51), G should be high (was red=1.0*255=255)
    TEST_ASSERT(rA < 100 && gA > 200, "v2: channels swapped (red low, green high)");

    frame.Destroy(fx.base);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyPipelineLayout(pl);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

void RegisterVulkanPushConstantTests() {
    auto suite = std::make_shared<TestSuite>("VulkanPushConstantTests");
    suite->AddTestCase(TestCase("PushConstantsRed",    TestPushConstantsRed));
    suite->AddTestCase(TestCase("ShaderHotReload",     TestShaderHotReload));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanPushConstantTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else

int main() {
    std::cout << "[TestVulkanPushConstants] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif
