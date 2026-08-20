#include "TestFramework.h"
#include "Graphics/RHI/Systems/RenderSystem.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIEntityManager.h"
#include "Graphics/RHI/Components/RenderLayerComponent.h"
#include "Graphics/RHI/Components/MaterialComponent.h"
#include "Graphics/RHI/Components/GPUBufferComponent.h"
#include "Graphics/RenderMesh.h"
#include "Common/Id.h"
#include <vector>
#include <cstdlib>
#include <unordered_map>
#include <cstring>

using namespace primal::graphics;
using namespace primal::graphics::rhi;

// Mock CommandBuffer
class MockCommandBufferECS : public RHICommandBuffer {
public:
    MockCommandBufferECS(RHIDeviceBase& device) 
        : RHICommandBuffer(device, CommandQueueType::Graphics) {
        handle_ = (CommandBufferHandle)12345; 
    }
    
    bool Initialize() override { return true; }
    void Destroy() override {}
    
    bool resetImpl() override { return true; }
    bool beginImpl() override { return true; }
    bool endImpl() override { return true; }
    bool submitImpl(uint32_t) override { return true; }
    bool waitForCompletionImpl() override { return true; }
    
    void BeginRenderPass(const RenderPassDesc&) override {}
    void BeginRenderPass(RenderPassHandle) override {}
    void EndRenderPass() override {}
    void SetViewport(const ViewportDesc&) override {}
    void SetScissor(const Rect&) override {}
    void BindGraphicsPipeline(PipelineHandle) override { bindPipelineCalled = true; }
    void BindVertexBuffers(uint32_t, uint32_t, const ResourceHandle*, const uint64_t*) override {}
    void BindIndexBuffer(ResourceHandle, DataFormat, uint64_t) override {}
    void BindDescriptorSets(PipelineBindPoint, PipelineLayoutHandle, uint32_t, uint32_t, const DescriptorSetHandle*, uint32_t, const uint32_t*) override { bindSetsCalled = true; }
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {
        drawCalled = true;
        drawCallCount++;
    }
    void DrawIndexed(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {
        drawIndexedCalled = true;
        drawCallCount++;
    }
    void DrawIndirect(ResourceHandle, uint64_t, uint32_t) override {}
    void BindComputePipeline(PipelineHandle) override {}
    void WriteTimestamp(QueryPoolHandle, uint32_t) override {}
    // stale-test port: pure virtuals added to RHICommandBuffer after the Dawn era
    void PushConstants(PipelineLayoutHandle, ShaderStage, uint32_t, uint32_t, const void*) override {}
    void SetComputeBytes(uint32_t, const void*, uint32_t) override {}
    void MemoryBarrier(PipelineStage, PipelineStage, AccessFlag, AccessFlag) override {}
    void Dispatch(uint32_t, uint32_t, uint32_t) override {}
    void DispatchIndirect(ResourceHandle, uint64_t) override {}
    void CopyBuffer(ResourceHandle, ResourceHandle, uint64_t, uint64_t, uint64_t) override {}
    void CopyBufferToTexture(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void CopyTextureToBuffer(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void BlitTexture(ResourceHandle, ResourceHandle, const TextureBlitRegion*, uint32_t, FilterMode) override {}
    void GenerateMipmaps(ResourceHandle) override {}
    void InsertBarrier(const ResourceBarrier*, uint32_t) override {}

    bool drawCalled = false;
    bool drawIndexedCalled = false;
    uint32_t drawCallCount = 0;
    bool bindPipelineCalled = false;
    bool bindSetsCalled = false;
};

// Mock RHISwapChain
class MockRHISwapChainECS : public RHISwapChain {
public:
    MockRHISwapChainECS(RHIDeviceBase& device, const SwapChainDesc& desc) 
        : RHISwapChain(device, desc) {}
    
    bool Initialize() override { return true; }
    void Destroy() override {}
    
    void Resize(uint32_t width, uint32_t height) override {
        swapChainDesc_.width = width;
        swapChainDesc_.height = height;
    }
    
    bool AcquireNextImage(uint32_t* imageIndex, SyncHandle semaphore, SyncHandle fence) override {
        *imageIndex = 0;
        return true;
    }
    
    void Present(SyncHandle semaphore) override {}
    
    uint32_t GetCurrentBackBufferIndex() const override { return 0; }
    
    ResourceHandle GetBackBuffer(uint32_t index) const override { return (ResourceHandle)3003; }

    // RHIResource implementation
    void* mapImpl(uint64_t, uint64_t) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, uint64_t, uint64_t) override { return true; }
};

// Mock RHIDevice implementation
class MockRHIDeviceECS : public RHIDevice<MockRHIDeviceECS> {
public:
    MockCommandBufferECS* mockCmdBuffer{nullptr};
    MockRHISwapChainECS* mockSwapChain{nullptr};

    MockRHIDeviceECS() : RHIDevice(DeviceDesc()) {
        isValid_ = true;
    }

    virtual ~MockRHIDeviceECS() {
        if (mockCmdBuffer) {
            if (mockCmdBuffer->GetHandle() != handles::INVALID_COMMAND_BUFFER) {
                UnregisterCommandBuffer(mockCmdBuffer->GetHandle());
            }
            delete mockCmdBuffer;
        }
        if (mockSwapChain) {
            delete mockSwapChain;
        }
    }
    
    bool initializeImpl() { return true; }
    void shutdownImpl() {}
    double getTimestampPeriodImpl() const { return 1.0; }
    void waitIdleImpl() const {}
    void beginFrameImpl() {}
    void endFrameImpl() {}
    void presentImpl() {}
    void queryDeviceInfo(DeviceInfo&) {}
    // stale-test port: Impl hooks added to the RHIDevice CRTP base after the Dawn era
    void setBufferDirtySizeImpl(ResourceHandle, u64) {}
    u32 getCurrentFrameIndexImpl() const { return 0; }
    
    bool submitImpl(const QueueSubmitInfo& info) {
        if (mockCmdBuffer && mockCmdBuffer->GetHandle() == info.cmdBuffer) {
            return mockCmdBuffer->Submit();
        }
        return false;
    }
    SyncHandle createSyncImpl() { return (SyncHandle)54321; }
    bool waitForSyncImpl(SyncHandle, u32) { return true; }
    void destroySyncImpl(SyncHandle) {}
    
    QueryPoolHandle createQueryPoolImpl(const QueryPoolDesc&) { return handles::INVALID_QUERY_POOL; }
    void destroyQueryPoolImpl(QueryPoolHandle) {}
    bool getQueryPoolResultsImpl(QueryPoolHandle, uint32_t, uint32_t, void*, size_t) { return false; }
    SamplerHandle createSamplerImpl(const SamplerDesc&) { return (SamplerHandle)7001; }
    void destroySamplerImpl(SamplerHandle) {}
    DescriptorSetLayoutHandle createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc&) { return (DescriptorSetLayoutHandle)4001; }
    void destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle) {}
    PipelineLayoutHandle createPipelineLayoutImpl(const PipelineLayoutDesc&) { return (PipelineLayoutHandle)8001; }
    void destroyPipelineLayoutImpl(PipelineLayoutHandle) {}
    DescriptorSetHandle createDescriptorSetImpl(const DescriptorSetDesc&) { return (DescriptorSetHandle)6001; }
    void destroyDescriptorSetImpl(DescriptorSetHandle) {}
    void updateDescriptorSetsImpl(uint32_t, const WriteDescriptorSet*) {}
    
    RenderPassHandle createRenderPassImpl(const RenderPassDesc&) { return (RenderPassHandle)9001; } // Moved/Renamed to match RHIDevice template call
    void destroyRenderPassImpl(RenderPassHandle) {}

    // Mock Buffer Storage
    ResourceHandle createBufferImpl(const BufferDesc&) { return (ResourceHandle)1001; }
    void destroyBufferImpl(ResourceHandle) {}
    // stale-test port: ParticlePass::initialize maps its per-frame uniform
    // buffer and fails on nullptr — back the mock with static scratch memory.
    void* mapBufferImpl(ResourceHandle, u64, u64) {
        static u8* scratch = (u8*)calloc(1, 1 << 20);
        return scratch;
    }
    void unmapBufferImpl(ResourceHandle) {}

    ResourceHandle createTextureImpl(const TextureDesc&) { return (ResourceHandle)2002; }
    ResourceHandle createTextureViewImpl(const TextureViewDesc&) { return (ResourceHandle)2003; } // Added
    void destroyTextureImpl(ResourceHandle) {}
    void destroyTextureViewImpl(ResourceHandle) {} // Added
    ShaderHandle createShaderImpl(const void*, size_t, ShaderStage, const char*) { return (ShaderHandle)3001; }
    void destroyShaderImpl(ShaderHandle) {}
    PipelineHandle createGraphicsPipelineImpl(const GraphicsPipelineDesc&) { return (PipelineHandle)666; }
    void destroyPipelineImpl(PipelineHandle) {}
    PipelineHandle createComputePipelineImpl(const ComputePipelineDesc&) { return (PipelineHandle)5001; }
    
    RHISwapChain* createSwapChainImpl(const SwapChainDesc& desc) {
        if (!mockSwapChain) {
            mockSwapChain = new MockRHISwapChainECS(*this, desc);
        }
        return mockSwapChain;
    }
    void destroySwapChainImpl(RHISwapChain* sc) {
        if (sc == mockSwapChain) {
            // Managed by device destructor or explicit cleanup
        }
    }
    
    CommandBufferHandle createCommandBufferImpl(CommandQueueType) {
        if (!mockCmdBuffer) {
            mockCmdBuffer = new MockCommandBufferECS(*this);
            RegisterCommandBuffer(mockCmdBuffer);
        }
        return mockCmdBuffer->GetHandle();
    }

    void destroyCommandBufferImpl(CommandBufferHandle handle) { // Added
        if (mockCmdBuffer && mockCmdBuffer->GetHandle() == handle) {
             // Already handled in destructor or we can do it here.
        }
    }
};

class TestRenderSystemECS : public Engine::Test::TestSuite {
public:
    TestRenderSystemECS() : Engine::Test::TestSuite("TestRenderSystemECS") {
        AddTestCase({"RenderSystemECS_Test", [this]() { return RunTests(); }});
    }

    Engine::Test::TestResult RunTests() {
        // Setup Device
        MockRHIDeviceECS device;
        device.Initialize();
        
        // Setup EntityManager
        RHIEntityManager entityManager;

        // Setup RenderSystem
        RenderSystem renderSystem;
        RenderSystemInitInfo initInfo;
        initInfo.device = &device;
        initInfo.entityManager = &entityManager;
        // Window is not needed for this test
        renderSystem.Initialize(initInfo);
        
        // Create Mock CommandBuffer
        device.createCommandBufferImpl(CommandQueueType::Graphics);
        MockCommandBufferECS* cmdBuffer = device.mockCmdBuffer;

        // --- Test 1: Empty EntityManager ---
        renderSystem.Render(cmdBuffer);
        TEST_ASSERT(cmdBuffer->drawCallCount == 0, "Empty manager should result in 0 draw calls");

        // --- Test 2: Single Entity ---
        RHIEntityID entity = entityManager.CreateEntity();
        
        // Add RenderLayer
        RenderLayerComponent layer;
        layer.layerMask = 1;
        layer.priority = 0;
        entityManager.AddComponent<RenderLayerComponent>(entity) = layer;
        
        // Add Material
        Material material;
        MaterialComponent matComp;
        matComp.materialInstance = std::make_shared<MaterialInstance>(&material);
        matComp.materialInstance->Initialize(&device);
        entityManager.AddComponent<MaterialComponent>(entity) = matComp;
        
        // Add Buffer
        GPUBufferComponent buffer;
        buffer.vertexBuffer = (ResourceHandle)1001;
        buffer.vertexCount = 3;
        entityManager.AddComponent<GPUBufferComponent>(entity) = buffer;
        
        // Set Render Pass
        renderSystem.SetCurrentRenderPass((RenderPassHandle)9001);
        
        // Render
        renderSystem.Render(cmdBuffer);
        
        TEST_ASSERT(cmdBuffer->drawCallCount == 1, "Should have 1 draw call");
        TEST_ASSERT(cmdBuffer->drawCalled == true, "Draw should be called");
        TEST_ASSERT(cmdBuffer->bindPipelineCalled == true, "Pipeline should be bound");

        // --- Test 3: Multiple Entities with Sorting ---
        // Add another entity with higher priority
        RHIEntityID entity2 = entityManager.CreateEntity();
        
        RenderLayerComponent layer2;
        layer2.layerMask = 1;
        layer2.priority = 10; 
        
        entityManager.AddComponent<RenderLayerComponent>(entity2) = layer2;
        entityManager.AddComponent<MaterialComponent>(entity2) = matComp; // Share material
        entityManager.AddComponent<GPUBufferComponent>(entity2) = buffer;
        
        cmdBuffer->drawCallCount = 0;
        renderSystem.Render(cmdBuffer);
        TEST_ASSERT(cmdBuffer->drawCallCount == 2, "Should have 2 draw calls");
        
        renderSystem.Shutdown();
        return Engine::Test::TestResult::Passed;
    }
};

int main() {
    TestRenderSystemECS test;
    test.RunAllTests();
    return 0;
}
