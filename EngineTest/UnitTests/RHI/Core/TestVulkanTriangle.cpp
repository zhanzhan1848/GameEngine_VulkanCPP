/**
 * @file TestVulkanTriangle.cpp
 * @brief Phase 4 三角形渲染集成测试
 * @details 渲染一个三角形到 offscreen texture,验证完整渲染路径:
 *          ShaderModule → Pipeline → RenderPass → BeginRenderPass → Draw → EndRenderPass
 *          → CopyTextureToBuffer → readback 验证非空。
 *          不依赖 SwapChain(避免 surface 创建),作为 Phase 4a 验收。
 *          Phase 4b 加 SwapChain 后做 on-screen 测试。
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDeviceFactory.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RHI/Core/RHIRenderPass.h"
#include "Graphics/RHI/Core/RHIDescriptorSet.h"
#include "Graphics/RHI/Core/RHIPipelineLayout.h"

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanDevice.h"
#include "Graphics/RHI/Platforms/Vulkan/VulkanCommandBuffer.h"
#endif

#include <iostream>
#include <fstream>
#include <vector>
#include <cstring>
#include <cmath>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

#if defined(ENABLE_VULKAN) && ENABLE_VULKAN

namespace {

/// 读 SPIR-V 字节码
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
    ~DeviceFixture() {
        if (base) base->Shutdown();
    }
};

} // anonymous namespace

/// 渲染三角形到 256x256 RGBA8 offscreen,readback,验证中心像素非空
TestResult TestOffscreenTriangle() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    // === 1. SPIR-V shader ===
    auto vert = ReadSPV("Assets/Shaders/SPIRV/triangle.vert.spv");
    auto frag = ReadSPV("Assets/Shaders/SPIRV/triangle.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read SPIR-V files");

    ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel,  "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader vertex");
    TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader fragment");

    // === 2. Pipeline ===
    GraphicsPipelineDesc gpd{};
    gpd.vertexShader = vs;
    gpd.pixelShader  = fs;
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

    // === 3. Offscreen render target ===
    constexpr u32 kW = 256, kH = 256;
    TextureDesc tdesc{};
    tdesc.size = { kW, kH, 1 };
    tdesc.mipLevels = 1;
    tdesc.arraySize = 1;
    tdesc.format = DataFormat::RG8B8A8_UNorm;
    tdesc.type = TextureType::Texture2D;
    tdesc.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
    tdesc.memoryUsage = GPUMemoryUsage::Static;
    tdesc.name = "OffscreenRT";
    ResourceHandle rt = fx.base->CreateTexture(tdesc);
    TEST_ASSERT(rt != handles::INVALID_RESOURCE, "CreateTexture RT");

    // === 4. Readback buffer ===
    BufferDesc bdesc{};
    bdesc.size = u64(kW) * u64(kH) * 4;
    bdesc.type = BufferType::Raw;
    bdesc.memoryUsage = GPUMemoryUsage::Readback;
    bdesc.name = "ReadbackBuf";
    ResourceHandle readback = fx.base->CreateBuffer(bdesc);
    TEST_ASSERT(readback != handles::INVALID_RESOURCE, "CreateBuffer readback");

    // === 5. Render pass desc (offscreen) ===
    RenderPassDesc rpd{};
    rpd.colorAttachments.resize(1);
    rpd.colorAttachments[0].texture = rt;
    rpd.colorAttachments[0].format = DataFormat::RG8B8A8_UNorm;
    rpd.colorAttachments[0].loadOp = LoadAction::Clear;
    rpd.colorAttachments[0].storeOp = StoreAction::Store;
    rpd.colorAttachments[0].clearValue.color = primal::math::v4{0.0f, 0.0f, 0.0f, 1.0f};
    rpd.viewport.topLeft = {0.0f, 0.0f};
    rpd.viewport.size = {float(kW), float(kH)};
    rpd.viewport.minDepth = 0.0f;
    rpd.viewport.maxDepth = 1.0f;
    rpd.scissor.offset = {0, 0};
    rpd.scissor.extent = {kW, kH};

    // === 6. Render ===
    CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
    TEST_ASSERT(cmd != handles::INVALID_COMMAND_BUFFER, "CreateCommandBuffer");
    VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
    TEST_ASSERT(vcmd, "GetCommandBuffer");

    TEST_ASSERT(vcmd->Reset(), "Reset");
    TEST_ASSERT(vcmd->Begin(), "Begin");
    vcmd->BeginRenderPass(rpd);
    vcmd->BindGraphicsPipeline(pipe);
    vcmd->Draw(3, 0, 1, 0);
    vcmd->EndRenderPass();

    // Copy rt → readback
    BufferTextureCopyRegion region{};
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {kW, kH, 1};
    vcmd->CopyTextureToBuffer(rt, readback, &region, 1);

    TEST_ASSERT(vcmd->End(), "End");
    TEST_ASSERT(vcmd->Submit(0), "Submit");
    TEST_ASSERT(vcmd->WaitForCompletion(), "WaitForCompletion");

    // === 7. Readback & check center pixel !== clear color ===
    void* mapped = fx.base->MapBuffer(readback, 0, bdesc.size);
    TEST_ASSERT(mapped, "MapBuffer readback");
    {
        const u8* pixels = reinterpret_cast<const u8*>(mapped);
        // center pixel
        u32 cx = kW / 2, cy = kH / 2;
        // Triangle 中心应位于上半部分(顶点在 (0,-0.5) → top),中心 (0,0) 也可能在三角形内
        // 顶点坐标:(0,-0.5),(0.5,0.5),(-0.5,0.5) — 中心 (0,0) 落在三角形内
        u32 idx = (cy * kW + cx) * 4;
        u8 r = pixels[idx + 0], g = pixels[idx + 1], b = pixels[idx + 2], a = pixels[idx + 3];
        // 至少有一个通道 != 0(混合 3 个 vertex color)
        bool pixelNonEmpty = (r + g + b) > 0;
        TEST_ASSERT(pixelNonEmpty, "Triangle center pixel rendered");
        std::cout << "[TestVulkanTriangle] center pixel RGBA = ("
                  << int(r) << "," << int(g) << "," << int(b) << "," << int(a) << ")" << std::endl;
    }
    fx.base->UnmapBuffer(readback);

    // === Cleanup ===
    fx.base->DestroyCommandBuffer(cmd);
    fx.base->DestroyBuffer(readback);
    fx.base->DestroyTexture(rt);
    fx.base->DestroyPipeline(pipe);
    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    return TestResult::Passed;
}

/// 仅创建 + 销毁 pipeline / shader,验证生命周期
TestResult TestPipelineCreateDestroy() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "Vulkan device init");

    auto vert = ReadSPV("Assets/Shaders/SPIRV/triangle.vert.spv");
    auto frag = ReadSPV("Assets/Shaders/SPIRV/triangle.frag.spv");
    TEST_ASSERT(!vert.empty() && !frag.empty(), "Read SPIR-V files");

    for (int i = 0; i < 3; ++i) {
        ShaderHandle vs = fx.base->CreateShader(vert.data(), vert.size(), ShaderStage::Vertex, "main");
        ShaderHandle fs = fx.base->CreateShader(frag.data(), frag.size(), ShaderStage::Pixel, "main");
        TEST_ASSERT(vs != handles::INVALID_SHADER, "CreateShader vertex");
        TEST_ASSERT(fs != handles::INVALID_SHADER, "CreateShader fragment");

        GraphicsPipelineDesc gpd{};
        gpd.vertexShader = vs;
        gpd.pixelShader  = fs;
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

        fx.base->DestroyPipeline(pipe);
        fx.base->DestroyShader(fs);
        fx.base->DestroyShader(vs);
    }
    return TestResult::Passed;
}

void RegisterVulkanTriangleTests() {
    auto suite = std::make_shared<TestSuite>("VulkanTriangleTests");
    suite->AddTestCase(TestCase("OffscreenTriangle",       TestOffscreenTriangle));
    suite->AddTestCase(TestCase("PipelineCreateDestroy",   TestPipelineCreateDestroy));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanTriangleTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else // ENABLE_VULKAN undefined

int main() {
    std::cout << "[TestVulkanTriangle] ENABLE_VULKAN not defined — test is a no-op build check." << std::endl;
    return 0;
}

#endif // ENABLE_VULKAN
