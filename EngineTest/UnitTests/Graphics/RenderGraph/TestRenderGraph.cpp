#include "../../TestFramework.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include <iostream>

using namespace primal::graphics::rendergraph;
using namespace primal::graphics::rhi;

// Mock RHI Device
class MockRHICommandBuffer : public RHICommandBuffer {
public:
    MockRHICommandBuffer(RHIDeviceBase& device) : RHICommandBuffer(device, CommandQueueType::Graphics) {}

    virtual void BeginRenderPass(const RenderPassDesc& desc) override {}
    virtual void BeginRenderPass(RenderPassHandle renderPass) override {}
    virtual void EndRenderPass() override {}
    
    virtual void SetViewport(const ViewportDesc& viewport) override {}
    virtual void SetScissor(const Rect& scissor) override {}
    
    virtual void BindGraphicsPipeline(PipelineHandle pipeline) override {}
    virtual void BindVertexBuffers(uint32_t firstSlot, uint32_t slotCount, const ResourceHandle* buffers, const uint64_t* offsets) override {}
    virtual void BindIndexBuffer(ResourceHandle buffer, DataFormat format, uint64_t offset = 0) override {}
    virtual void BindDescriptorSets(PipelineBindPoint bindPoint, PipelineLayoutHandle pipelineLayout, uint32_t firstSet, uint32_t setCount, const DescriptorSetHandle* descriptorSets, uint32_t dynamicOffsetCount, const uint32_t* dynamicOffsets) override {}
    
    virtual void Draw(uint32_t vertexCount, uint32_t startVertex = 0, uint32_t instanceCount = 1, uint32_t startInstance = 0) override {}
    virtual void DrawIndexed(uint32_t indexCount, uint32_t startIndex = 0, uint32_t baseVertex = 0, uint32_t instanceCount = 1, uint32_t startInstance = 0) override {}
    virtual void DrawIndirect(ResourceHandle buffer, uint64_t offset = 0, uint32_t drawCount = 1) override {}
    
    virtual void BindComputePipeline(PipelineHandle pipeline) override {}
    virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override {}
    virtual void DispatchIndirect(ResourceHandle buffer, uint64_t offset = 0) override {}
    
    virtual void CopyBuffer(ResourceHandle src, ResourceHandle dst, uint64_t srcOffset = 0, uint64_t dstOffset = 0, uint64_t size = 0) override {}
    virtual void CopyBufferToTexture(ResourceHandle srcBuffer, ResourceHandle dstTexture, const BufferTextureCopyRegion* regions, uint32_t regionCount) override {}
    virtual void CopyTextureToBuffer(ResourceHandle srcTexture, ResourceHandle dstBuffer, const BufferTextureCopyRegion* regions, uint32_t regionCount) override {}
    virtual void BlitTexture(ResourceHandle src, ResourceHandle dst, const TextureBlitRegion* regions, uint32_t regionCount, FilterMode filter) override {}
    virtual void GenerateMipmaps(ResourceHandle texture) override {}
    
    virtual void InsertBarrier(const ResourceBarrier* barriers, uint32_t barrierCount) override {}

    virtual bool Initialize() override { return true; }

protected:
    virtual bool resetImpl() override { return true; }
    virtual bool beginImpl() override { return true; }
    virtual bool endImpl() override { return true; }
    virtual bool submitImpl(uint32_t waitFlags) override { return true; }
    virtual bool waitForCompletionImpl() override { return true; }
};

class MockRHIDevice : public RHIDeviceBase {
public:
    // 最小化实现，仅用于测试 RenderGraph
    virtual bool IsValid() const override { return true; }
    virtual const DeviceInfo& GetDeviceInfo() const override { static DeviceInfo info; return info; }
    virtual const DeviceDesc& GetDesc() const override { static DeviceDesc desc; return desc; }
    virtual void WaitIdle() const override {}
    virtual void Shutdown() override {}
    virtual bool Submit(const QueueSubmitInfo& info) override { return true; }
    virtual SyncHandle CreateSync() override { return handles::INVALID_SYNC; }
    virtual bool WaitForSync(SyncHandle handle, u32 timeoutMs) override { return true; }
    virtual void DestroySync(SyncHandle handle) override {}
    virtual QueryPoolHandle CreateQueryPool(const QueryPoolDesc& desc) override { return handles::INVALID_QUERY_POOL; }
    virtual void DestroyQueryPool(QueryPoolHandle handle) override {}
    virtual SamplerHandle CreateSampler(const SamplerDesc& desc) override { return handles::INVALID_SAMPLER; }
    virtual void DestroySampler(SamplerHandle handle) override {}
    virtual DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override { return handles::INVALID_DESCRIPTOR_SET_LAYOUT; }
    virtual void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle handle) override {}
    virtual PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) override { return handles::INVALID_PIPELINE_LAYOUT; }
    virtual void DestroyPipelineLayout(PipelineLayoutHandle handle) override {}
    virtual DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& desc) override { return handles::INVALID_DESCRIPTOR_SET; }
    virtual void DestroyDescriptorSet(DescriptorSetHandle handle) override {}
    virtual void UpdateDescriptorSets(uint32_t writeCount, const WriteDescriptorSet* writes) override {}
    virtual RHISwapChain* CreateSwapChain(const SwapChainDesc& desc) override { return nullptr; }
    virtual void DestroySwapChain(RHISwapChain* swapChain) override {}

    virtual RenderPassHandle CreateRenderPass(const RenderPassDesc& desc) override { return handles::INVALID_RENDER_PASS; }
    virtual void DestroyRenderPass(RenderPassHandle handle) override {}
    
    virtual ResourceHandle CreateBuffer(const BufferDesc& desc) override { 
        return {static_cast<uint32_t>(bufferCounter_++)}; 
    }
    
    virtual ResourceHandle CreateTexture(const TextureDesc& desc) override { 
        return {static_cast<uint32_t>(textureCounter_++)}; 
    }
    
    virtual ShaderHandle CreateShader(const void* data, size_t size, ShaderStage stage, const char* entryPoint = "main") override { return handles::INVALID_SHADER; }
    virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override { return handles::INVALID_PIPELINE; }
    virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override { return handles::INVALID_PIPELINE; }
    virtual CommandBufferHandle CreateCommandBuffer(CommandQueueType type) override { return handles::INVALID_COMMAND_BUFFER; }
    virtual void DestroyCommandBuffer(CommandBufferHandle handle) override {}
    virtual void DestroyBuffer(ResourceHandle handle) override {}
    virtual void DestroyTexture(ResourceHandle handle) override {}
    virtual void DestroyShader(ShaderHandle) override {}
    virtual void DestroyPipeline(PipelineHandle) override {}
    virtual void* MapBuffer(ResourceHandle, u64 = 0, u64 = 0) override { return nullptr; }
    virtual void UnmapBuffer(ResourceHandle) override {}
    virtual RHIGarbageCollector& GetGarbageCollector() override { static RHIGarbageCollector gc; return gc; }

private:
    uint64_t bufferCounter_ = 1;
    uint64_t textureCounter_ = 1;
};

// 测试用例 1: 基础图构建与资源创建
Engine::Test::TestResult TestBasicGraphBuild() {
    MockRHIDevice device;
    RenderGraph graph(device);
    MockRHICommandBuffer cmdBuffer(device);

    struct PassData {
        RGResourceHandle outputTex;
    };

    bool executed = false;

    graph.AddPass<PassData>("TestPass", RGPassType::Graphics,
        [&](PassData& data, RenderGraphBuilder& builder) {
            TextureDesc desc;
            desc.size = {1024, 1024, 1};
            data.outputTex = builder.CreateTexture("OutputTex", desc);
            builder.SideEffect(); // Ensure pass is not culled
        },
        [&](const PassData& data, RenderGraphContext&) {
            executed = true;
            // 验证资源是否已分配 (Handle index > 0)
            if (data.outputTex.index == 0) {
                std::cerr << "Error: Output texture handle is invalid." << std::endl;
            }
        }
    );

    graph.Compile();
    graph.Execute(&cmdBuffer);

    if (!executed) {
        std::cerr << "Error: Pass was not executed." << std::endl;
        return Engine::Test::TestResult::Failed;
    }

    return Engine::Test::TestResult::Passed;
}

// 测试用例 2: 资源依赖
Engine::Test::TestResult TestResourceDependency() {
    MockRHIDevice device;
    RenderGraph graph(device);
    MockRHICommandBuffer cmdBuffer(device);


    struct Pass1Data {
        RGResourceHandle tex;
    };

    struct Pass2Data {
        RGResourceHandle inputTex;
    };

    RGResourceHandle sharedTex;

    graph.AddPass<Pass1Data>("Pass1", RGPassType::Graphics,
        [&](Pass1Data& data, RenderGraphBuilder& builder) {
            TextureDesc desc;
            data.tex = builder.CreateTexture("SharedTex", desc);
            sharedTex = data.tex;
        },
        [](const Pass1Data&, RenderGraphContext&) {}
    );

    graph.AddPass<Pass2Data>("Pass2", RGPassType::Graphics,
        [&](Pass2Data& data, RenderGraphBuilder& builder) {
            data.inputTex = builder.Read(sharedTex);
        },
        [](const Pass2Data&, RenderGraphContext&) {}
    );

    graph.Compile();
    
    // 验证资源生命周期 (需访问内部状态，或通过行为验证)
    // 这里简单验证是否能正常运行
    graph.Execute(&cmdBuffer);

    return Engine::Test::TestResult::Passed;
}

// 测试用例 3: Pass 剔除
Engine::Test::TestResult TestPassCulling() {
    MockRHIDevice device;
    RenderGraph graph(device);
    MockRHICommandBuffer cmdBuffer(device);

    struct Data {
        RGResourceHandle res;
    };

    bool passA_Executed = false;
    bool passB_Executed = false;
    bool passC_Executed = false; // Should be culled
    bool passD_Executed = false;

    RGResourceHandle resX;
    RGResourceHandle resY;
    RGResourceHandle resZ;

    // Pass A: Produces X
    graph.AddPass<Data>("PassA", RGPassType::Graphics,
        [&](Data& data, RenderGraphBuilder& builder) {
            TextureDesc desc;
            desc.size = {1024, 1024, 1};
            data.res = builder.CreateTexture("ResX", desc);
            resX = data.res;
        },
        [&](const Data&, RenderGraphContext&) { passA_Executed = true; }
    );

    // Pass B: Reads X, Produces Y
    graph.AddPass<Data>("PassB", RGPassType::Graphics,
        [&](Data& data, RenderGraphBuilder& builder) {
            builder.Read(resX);
            TextureDesc desc;
            desc.size = {1024, 1024, 1};
            data.res = builder.CreateTexture("ResY", desc);
            resY = data.res;
        },
        [&](const Data&, RenderGraphContext&) { passB_Executed = true; }
    );

    // Pass C: Reads X, Produces Z (Unused)
    graph.AddPass<Data>("PassC", RGPassType::Graphics,
        [&](Data& data, RenderGraphBuilder& builder) {
            builder.Read(resX);
            TextureDesc desc;
            desc.size = {1024, 1024, 1};
            data.res = builder.CreateTexture("ResZ", desc);
            resZ = data.res;
        },
        [&](const Data&, RenderGraphContext&) { passC_Executed = true; }
    );

    // Pass D: Reads Y, Output to BackBuffer
    RGResourceHandle backBuffer = graph.ImportResource("BackBuffer", 100); // Fake handle
    
    graph.AddPass<Data>("PassD", RGPassType::Graphics,
        [&](Data& /*data*/, RenderGraphBuilder& builder) {
             builder.Read(resY);
             builder.Write(backBuffer);
        },
        [&](const Data&, RenderGraphContext&) { passD_Executed = true; }
    );

    graph.Compile();
    graph.Execute(&cmdBuffer);

    if (!passA_Executed) std::cerr << "Pass A should be executed" << std::endl;
    if (!passB_Executed) std::cerr << "Pass B should be executed" << std::endl;
    if (passC_Executed) std::cerr << "Pass C should be culled" << std::endl;
    if (!passD_Executed) std::cerr << "Pass E should be executed" << std::endl;

    if (passA_Executed && passB_Executed && !passC_Executed && passD_Executed) {
        return Engine::Test::TestResult::Passed;
    }
    return Engine::Test::TestResult::Failed;
}


// 测试用例 4: 内存复用 (Aliasing)
Engine::Test::TestResult TestMemoryAliasing() {
    MockRHIDevice device;
    RenderGraph graph(device);
    MockRHICommandBuffer cmdBuffer(device);

    struct PassData {
        RGResourceHandle tex;
    };

    RGResourceHandle tex1;
    RGResourceHandle tex2;

    // Pass 1: Creates tex1
    graph.AddPass<PassData>("Pass1", RGPassType::Graphics,
        [&](PassData& data, RenderGraphBuilder& builder) {
            TextureDesc desc;
            desc.size = {1024, 1024, 1};
            data.tex = builder.CreateTexture("Tex1", desc);
            builder.Write(data.tex);
            tex1 = data.tex;
        },
        [&](const PassData&, RenderGraphContext&) {}
    );


    // Pass 2: Reads tex1 (End of tex1 lifetime)
    graph.AddPass<PassData>("Pass2", RGPassType::Graphics,
        [&](PassData& /*data*/, RenderGraphBuilder& builder) {
            builder.Read(tex1);
            builder.SideEffect(); // Ensure Pass 2 is not culled
        },
        [&](const PassData&, RenderGraphContext&) {}
    );

    // Pass 3: Creates tex2 (Start of tex2 lifetime)
    // Should reuse tex1's memory because tex1 is done after Pass 2
    graph.AddPass<PassData>("Pass3", RGPassType::Graphics,
        [&](PassData& data, RenderGraphBuilder& builder) {
            TextureDesc desc;
            desc.size = {1024, 1024, 1}; // Same desc
            data.tex = builder.CreateTexture("Tex2", desc);
            builder.Write(data.tex);
            builder.SideEffect(); // Force execution
            tex2 = data.tex;
        },
        [&](const PassData&, RenderGraphContext&) {}
    );

    graph.Compile();
    graph.Execute(&cmdBuffer);


    // Verify physical handles
    auto* r1 = graph.GetResource(tex1);
    auto* r2 = graph.GetResource(tex2);

    if (r1->GetPhysicalHandle() == 0 || r2->GetPhysicalHandle() == 0) {
        std::cerr << "Error: Resources not allocated." << std::endl;
        return Engine::Test::TestResult::Failed;
    }

    if (r1->GetPhysicalHandle() != r2->GetPhysicalHandle()) {
        std::cerr << "Error: Memory aliasing failed. Handles should be same." << std::endl;
        std::cerr << "Tex1: " << r1->GetPhysicalHandle() << ", Tex2: " << r2->GetPhysicalHandle() << std::endl;
        return Engine::Test::TestResult::Failed;
    }

    return Engine::Test::TestResult::Passed;
}

int main() {
    std::vector<Engine::Test::TestCase> tests = {
        {"BasicGraphBuild", TestBasicGraphBuild, "Tests basic render graph construction and execution"},
        {"ResourceDependency", TestResourceDependency, "Tests resource dependency between passes"},
        {"PassCulling", TestPassCulling, "Tests pass culling logic"},
        {"MemoryAliasing", TestMemoryAliasing, "Tests resource memory aliasing"}
    };

    Engine::Test::TestStats stats;
    std::cout << "Running RenderGraph Tests..." << std::endl;

    for (const auto& test : tests) {
        std::cout << "[RUNNING] " << test.name << "..." << std::endl;
        auto result = test.func();
        if (result == Engine::Test::TestResult::Passed) {
            std::cout << "[PASSED] " << test.name << std::endl;
            stats.passedTests++;
        } else {
            std::cout << "[FAILED] " << test.name << std::endl;
            stats.failedTests++;
        }
        stats.totalTests++;
    }

    std::cout << "Test Summary: " << stats.passedTests << " passed, " << stats.failedTests << " failed." << std::endl;
    return stats.failedTests == 0 ? 0 : 1;
}
