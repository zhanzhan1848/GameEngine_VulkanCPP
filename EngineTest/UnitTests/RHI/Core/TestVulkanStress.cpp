/**
 * @file TestVulkanStress.cpp
 * @brief Phase 6 压力测试 — rapid create/destroy 循环验证 GC + free_list 正确性
 * @details:
 *   1) Buffer churn: 1000 个 1KB buffer 顺序创建+销毁
 *   2) Buffer churn (interleaved): 1000 个 buffer,每 10 个销毁一次前 5 个
 *   3) Texture churn: 100 个 64x64 RG8B8A8 纹理顺序创建+销毁
 *   4) Shader churn: 50 次 shader 创建+销毁
 *   5) Pipeline churn: 50 次 pipeline 创建+销毁
 *   6) Mixed: 创建大量资源 → 提交空 command buffer → 销毁所有(GC deferred destroys 必须等待)
 *      验证下一帧资源再分配不会撞到上一帧未释放的内存
 *   全程 validation layer 必须 0 error。
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

#include <iostream>
#include <fstream>
#include <vector>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

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

} // anonymous namespace

/// 1000 个 1KB buffer,创建 + 立即销毁(free_list 复用)
TestResult TestBufferChurnSequential() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "device init");
    constexpr int N = 1000;
    for (int i = 0; i < N; ++i) {
        BufferDesc d{};
        d.size = 1024;
        d.type = BufferType::Raw;
        d.memoryUsage = GPUMemoryUsage::Dynamic;
        ResourceHandle h = fx.base->CreateBuffer(d);
        TEST_ASSERT(h != handles::INVALID_RESOURCE, "CreateBuffer in churn");
        fx.base->DestroyBuffer(h);
    }
    fx.base->WaitIdle();
    return TestResult::Passed;
}

/// 1000 个 buffer,interleaved:每 10 个销毁前 5 个 → free_list 有空洞被复用
TestResult TestBufferChurnInterleaved() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "device init");
    constexpr int N = 1000;
    std::vector<ResourceHandle> live;
    live.reserve(64);
    for (int i = 0; i < N; ++i) {
        BufferDesc d{};
        d.size = 512;
        d.type = BufferType::Raw;
        d.memoryUsage = GPUMemoryUsage::Dynamic;
        ResourceHandle h = fx.base->CreateBuffer(d);
        TEST_ASSERT(h != handles::INVALID_RESOURCE, "CreateBuffer interleaved");
        live.push_back(h);
        if (live.size() >= 10) {
            for (int j = 0; j < 5; ++j) {
                fx.base->DestroyBuffer(live[j]);
            }
            live.erase(live.begin(), live.begin() + 5);
        }
    }
    for (ResourceHandle h : live) fx.base->DestroyBuffer(h);
    fx.base->WaitIdle();
    return TestResult::Passed;
}

/// 100 个 64x64 纹理顺序创建+销毁
TestResult TestTextureChurn() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "device init");
    constexpr int N = 100;
    for (int i = 0; i < N; ++i) {
        TextureDesc d{};
        d.size = {64, 64, 1};
        d.mipLevels = 1;
        d.arraySize = 1;
        d.format = DataFormat::RG8B8A8_UNorm;
        d.type = TextureType::Texture2D;
        d.usage = TextureUsage::RenderTarget | TextureUsage::CopySource;
        d.memoryUsage = GPUMemoryUsage::Static;
        ResourceHandle h = fx.base->CreateTexture(d);
        TEST_ASSERT(h != handles::INVALID_RESOURCE, "CreateTexture in churn");
        fx.base->DestroyTexture(h);
    }
    fx.base->WaitIdle();
    return TestResult::Passed;
}

/// 50 次 shader 创建+销毁,需 SPIR-V 字节码
TestResult TestShaderChurn() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "device init");
    // 内嵌 SPIR-V:使用 triangle.vert.spv 文件
    std::ifstream f("Assets/Shaders/SPIRV/triangle.vert.spv", std::ios::binary | std::ios::ate);
    TEST_ASSERT(f, "open triangle.vert.spv");
    if (!f) return TestResult::Failed;
    std::streamsize sz = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<u8> data(sz);
    f.read(reinterpret_cast<char*>(data.data()), sz);
    TEST_ASSERT(!data.empty(), "SPIR-V bytes loaded");

    constexpr int N = 50;
    for (int i = 0; i < N; ++i) {
        ShaderHandle h = fx.base->CreateShader(data.data(), data.size(),
                                               ShaderStage::Vertex, "main");
        TEST_ASSERT(h != handles::INVALID_SHADER, "CreateShader in churn");
        fx.base->DestroyShader(h);
    }
    fx.base->WaitIdle();
    return TestResult::Passed;
}

/// 50 次 graphics pipeline 创建+销毁(轻量 desc,triangle shader)
TestResult TestPipelineChurn() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "device init");

    std::ifstream vf("Assets/Shaders/SPIRV/triangle.vert.spv", std::ios::binary | std::ios::ate);
    std::ifstream ff("Assets/Shaders/SPIRV/triangle.frag.spv", std::ios::binary | std::ios::ate);
    TEST_ASSERT(vf && ff, "open shaders");
    if (!vf || !ff) return TestResult::Failed;
    std::vector<u8> vdata(vf.tellg()); vf.seekg(0); vf.read(reinterpret_cast<char*>(vdata.data()), vdata.size());
    std::vector<u8> fdata(ff.tellg()); ff.seekg(0); ff.read(reinterpret_cast<char*>(fdata.data()), fdata.size());

    // 创建一次 shader(50 pipeline 共享)
    ShaderHandle vs = fx.base->CreateShader(vdata.data(), vdata.size(), ShaderStage::Vertex, "main");
    ShaderHandle fs = fx.base->CreateShader(fdata.data(), fdata.size(), ShaderStage::Pixel, "main");
    TEST_ASSERT(vs != handles::INVALID_SHADER && fs != handles::INVALID_SHADER, "shaders");

    constexpr int N = 50;
    for (int i = 0; i < N; ++i) {
        GraphicsPipelineDesc gpd{};
        gpd.vertexShader = vs;
        gpd.pixelShader  = fs;
        gpd.topology = PrimitiveTopology::TriangleList;
        gpd.fillMode = FillMode::Solid;
        gpd.cullMode = CullMode::None;
        gpd.renderTargetCount = 1;
        gpd.renderTargetFormats[0] = DataFormat::RG8B8A8_UNorm;
        gpd.depthStencilFormat = DataFormat::Unknown;
        PipelineHandle p = fx.base->CreateGraphicsPipeline(gpd);
        TEST_ASSERT(p != handles::INVALID_PIPELINE, "CreatePipeline in churn");
        fx.base->DestroyPipeline(p);
    }

    fx.base->DestroyShader(fs);
    fx.base->DestroyShader(vs);
    fx.base->WaitIdle();
    return TestResult::Passed;
}

/// GC 压力:创建 200 个 buffer → 提交空 cmd → 立即销毁 buffer(GC 延迟释放)
/// 下一帧再创建 200 个,验证 free_list 不撞未释放的内存
TestResult TestGCDeferredDestroy() {
    DeviceFixture fx;
    TEST_ASSERT(fx.Init(), "device init");

    constexpr int N = 200;
    constexpr int kFrames = 5;
    for (int f = 0; f < kFrames; ++f) {
        std::vector<ResourceHandle> handles_buf;
        handles_buf.reserve(N);
        for (int i = 0; i < N; ++i) {
            BufferDesc d{};
            d.size = 256;
            d.type = BufferType::Raw;
            d.memoryUsage = GPUMemoryUsage::Dynamic;
            ResourceHandle h = fx.base->CreateBuffer(d);
            TEST_ASSERT(h != handles::INVALID_RESOURCE, "CreateBuffer in frame loop");
            handles_buf.push_back(h);
        }
        // 提交一个空 cmd buffer 触发 frame boundary
        CommandBufferHandle cmd = fx.base->CreateCommandBuffer(CommandQueueType::Graphics);
        VulkanCommandBuffer* vcmd = fx.vk->GetCommandBuffer(cmd);
        vcmd->Reset(); vcmd->Begin(); vcmd->End(); vcmd->Submit(0); vcmd->WaitForCompletion();
        fx.base->DestroyCommandBuffer(cmd);

        // 销毁所有(GC 应该 defer)
        for (ResourceHandle h : handles_buf) fx.base->DestroyBuffer(h);
        // 不调 WaitIdle — 让下一帧的分配自己碰 deferred destroy 的资源,验证 free_list race
    }
    fx.base->WaitIdle();
    return TestResult::Passed;
}

void RegisterVulkanStressTests() {
    auto suite = std::make_shared<TestSuite>("VulkanStressTests");
    suite->AddTestCase(TestCase("BufferChurnSequential", TestBufferChurnSequential));
    suite->AddTestCase(TestCase("BufferChurnInterleaved", TestBufferChurnInterleaved));
    suite->AddTestCase(TestCase("TextureChurn",          TestTextureChurn));
    suite->AddTestCase(TestCase("ShaderChurn",           TestShaderChurn));
    suite->AddTestCase(TestCase("PipelineChurn",         TestPipelineChurn));
    suite->AddTestCase(TestCase("GCDeferredDestroy",     TestGCDeferredDestroy));
    TestRunner::RegisterTestSuite(suite);
}

int main() {
    RegisterVulkanStressTests();
    TestRunner::RunAllSuites();
    return 0;
}

#else

int main() {
    std::cout << "[TestVulkanStress] ENABLE_VULKAN not defined — no-op." << std::endl;
    return 0;
}

#endif
