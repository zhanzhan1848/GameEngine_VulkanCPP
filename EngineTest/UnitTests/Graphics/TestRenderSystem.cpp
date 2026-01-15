#include "TestFramework.h"
#include "Graphics/RHI/Systems/RenderSystem.h" // Updated include path
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RenderProxy.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/RHI/Core/RHIGeometry.h"
#include "Graphics/RHI/Core/RHISwapChain.h"
#include "Graphics/RenderMesh.h"
#include "Common/Id.h"

using namespace primal::graphics;
using namespace primal::graphics::rhi;

class MockRHISwapChain : public RHISwapChain {
public:
    MockRHISwapChain(RHIDeviceBase& device, const SwapChainDesc& desc)
        : RHISwapChain(device, desc) {}

    bool Initialize() override { return true; }
    void Destroy() override {}
    bool AcquireNextImage(uint32_t* imageIndex, SyncHandle, SyncHandle) override {
        *imageIndex = 0;
        return true;
    }
    void Present(SyncHandle) override { presentCalled = true; }
    uint32_t GetCurrentBackBufferIndex() const override { return 0; }
    ResourceHandle GetBackBuffer(uint32_t) const override { return (ResourceHandle)999; }
    
    // Implement RHIResource pure virtuals
    void* mapImpl(uint64_t, uint64_t) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, uint64_t, uint64_t) override { return true; }
    
    bool presentCalled = false;
};

using namespace primal::graphics::rhi;

// Mock CommandBuffer
class MockCommandBuffer : public RHICommandBuffer {
public:
    MockCommandBuffer(RHIDeviceBase& device) 
        : RHICommandBuffer(device, CommandQueueType::Graphics) {
        // Assign a mock handle
        handle_ = (CommandBufferHandle)12345; 
    }
    
    bool Initialize() override { return true; }
    void Destroy() override {} // No-op for mock
    
    bool resetCalled = false;
    bool beginCalled = false;
    bool endCalled = false;
    bool submitCalled = false;

    // Implement pure virtuals
    bool resetImpl() override { resetCalled = true; return true; }
    bool beginImpl() override { beginCalled = true; return true; }
    bool endImpl() override { endCalled = true; return true; }
    bool submitImpl(uint32_t) override { submitCalled = true; return true; }
    bool waitForCompletionImpl() override { return true; }
    
    void BeginRenderPass(const RenderPassDesc&) override { beginRenderPassCount++; }
    void BeginRenderPass(RenderPassHandle) override { beginRenderPassCount++; }
    void EndRenderPass() override { endRenderPassCount++; }
    void SetViewport(const ViewportDesc&) override {}
    void SetScissor(const Rect&) override {}
    void BindGraphicsPipeline(PipelineHandle) override {}
    void BindVertexBuffers(uint32_t, uint32_t, const ResourceHandle*, const uint64_t*) override {}
    void BindIndexBuffer(ResourceHandle, DataFormat, uint64_t) override {}
    void BindDescriptorSets(PipelineBindPoint, PipelineLayoutHandle, uint32_t, uint32_t, const DescriptorSetHandle*, uint32_t, const uint32_t*) override {}
    void Draw(uint32_t vertexCount, uint32_t startVertex, uint32_t instanceCount, uint32_t startInstance) override {
        drawCalled = true;
        drawCallCount++;
        lastDrawVertexCount = vertexCount;
    }
    void DrawIndexed(uint32_t indexCount, uint32_t startIndex, uint32_t baseVertex, uint32_t instanceCount, uint32_t startInstance) override {
        drawIndexedCalled = true;
        lastDrawIndexCount = indexCount;
    }
    void DrawIndirect(ResourceHandle, uint64_t, uint32_t) override {}
    void BindComputePipeline(PipelineHandle) override {}

    // Tracking flags
    bool drawCalled = false;
    uint32_t drawCallCount = 0;
    uint32_t lastDrawVertexCount = 0;
    bool drawIndexedCalled = false;
    uint32_t lastDrawIndexCount = 0;

    uint32_t beginRenderPassCount = 0;
    uint32_t endRenderPassCount = 0;

    void Dispatch(uint32_t, uint32_t, uint32_t) override {}
    void DispatchIndirect(ResourceHandle, uint64_t) override {}
    void CopyBuffer(ResourceHandle, ResourceHandle, uint64_t, uint64_t, uint64_t) override {}
    void CopyBufferToTexture(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void CopyTextureToBuffer(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void BlitTexture(ResourceHandle, ResourceHandle, const TextureBlitRegion*, uint32_t, FilterMode) override {}
    void GenerateMipmaps(ResourceHandle) override {}
    void InsertBarrier(const ResourceBarrier*, uint32_t) override {}
};

// Mock RHIDevice implementation
class MockRHIDevice : public RHIDevice<MockRHIDevice> {
public:
    MockCommandBuffer* mockCmdBuffer{nullptr};

    MockRHIDevice() : RHIDevice(DeviceDesc()) {
        isValid_ = true; // Force valid for mock
    }

    virtual ~MockRHIDevice() {
        if (mockCmdBuffer) {
            // Unregister before delete to avoid stale pointer in global map
            // Assuming UnregisterCommandBuffer exists or we just rely on RHICommandBuffer destructor
            // RHICommandBuffer destructor doesn't unregister automatically?
            // Let's manually unregister if possible, or just rely on RHICommand system cleanup if any
            // RHICommand.h: UnregisterCommandBuffer(CommandBufferHandle handle);
            if (mockCmdBuffer->GetHandle() != handles::INVALID_COMMAND_BUFFER) {
                UnregisterCommandBuffer(mockCmdBuffer->GetHandle());
            }
            delete mockCmdBuffer;
        }
    }
    
    // Implement required methods for RHIDevice<Derived>
    bool initializeImpl() { return true; }
    void shutdownImpl() {}
    void waitIdleImpl() const {}
    void beginFrameImpl() {}
    void endFrameImpl() {}
    void presentImpl() {}
    void queryDeviceInfo(DeviceInfo&) {}
    
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
    SamplerHandle createSamplerImpl(const SamplerDesc&) { return handles::INVALID_SAMPLER; }
    void destroySamplerImpl(SamplerHandle) {}
    DescriptorSetLayoutHandle createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc&) { return handles::INVALID_DESCRIPTOR_SET_LAYOUT; }
    void destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle) {}
    PipelineLayoutHandle createPipelineLayoutImpl(const PipelineLayoutDesc&) { return handles::INVALID_PIPELINE_LAYOUT; }
    void destroyPipelineLayoutImpl(PipelineLayoutHandle) {}
    DescriptorSetHandle createDescriptorSetImpl(const DescriptorSetDesc&) { return handles::INVALID_DESCRIPTOR_SET; }
    void destroyDescriptorSetImpl(DescriptorSetHandle) {}
    void updateDescriptorSetsImpl(uint32_t, const WriteDescriptorSet*) {}
    // Mock Buffer Storage
    std::unordered_map<uint64_t, std::vector<uint8_t>> bufferStorage;
    uint64_t nextBufferHandle = 1000;

    ResourceHandle createBufferImpl(const BufferDesc& desc) { 
        ResourceHandle handle = (ResourceHandle)++nextBufferHandle;
        bufferStorage[(uint64_t)handle].resize(desc.size);
        return handle; 
    }
    
    void destroyBufferImpl(ResourceHandle handle) {
        bufferStorage.erase((uint64_t)handle);
    }
    
    void* mapBufferImpl(ResourceHandle handle, u64 offset, u64 size) { 
        if (bufferStorage.find((uint64_t)handle) != bufferStorage.end()) {
            return bufferStorage[(uint64_t)handle].data() + offset;
        }
        return nullptr; 
    }
    
    void unmapBufferImpl(ResourceHandle) {}

    ResourceHandle createTextureImpl(const TextureDesc&) { return (ResourceHandle)2002; }
    ShaderHandle createShaderImpl(const void*, size_t, ShaderStage, const char*) { return handles::INVALID_SHADER; }
    PipelineHandle createGraphicsPipelineImpl(const GraphicsPipelineDesc&) { return (PipelineHandle)666; }
    PipelineHandle createComputePipelineImpl(const ComputePipelineDesc&) { return handles::INVALID_PIPELINE; }
    RenderPassHandle createRenderPassImpl(const RenderPassDesc&) { return handles::INVALID_RESOURCE; }
    void destroyRenderPassImpl(RenderPassHandle) {}
    
    MockRHISwapChain* mockSwapChain = nullptr;

    RHISwapChain* createSwapChainImpl(const SwapChainDesc& desc) {
        mockSwapChain = new MockRHISwapChain(*this, desc);
        return mockSwapChain;
    }
    
    void destroySwapChainImpl(RHISwapChain* swapChain) {
        if (swapChain == mockSwapChain) {
            delete mockSwapChain;
            mockSwapChain = nullptr;
        } else {
            delete swapChain;
        }
    }

    CommandBufferHandle createCommandBufferImpl(CommandQueueType) {
        if (!mockCmdBuffer) {
            mockCmdBuffer = new MockCommandBuffer(*this);
            RegisterCommandBuffer(mockCmdBuffer);
        }
        return mockCmdBuffer->GetHandle();
    }
    
    // void destroyBufferImpl(ResourceHandle) {} // Removed as it is now implemented above
    void destroyTextureImpl(ResourceHandle) {}
    void destroyShaderImpl(ShaderHandle) {}
    void destroyPipelineImpl(PipelineHandle) {}
    
    // void* mapBufferImpl(ResourceHandle, u64, u64) { return nullptr; } // Removed as it is now implemented above
    // void unmapBufferImpl(ResourceHandle) {} // Removed as it is now implemented above

    // Base class overrides if any needed
};

using TestResult = Engine::Test::TestResult;

TestResult TestRenderSystemInit() {
    RenderSystem system;
    MockRHIDevice mockDevice;
    
    RenderSystemInitInfo initInfo;
    initInfo.device = &mockDevice;
    initInfo.window = (primal::platform::window_handle)1; // Mock window
    initInfo.width = 800;
    initInfo.height = 600;

    TEST_ASSERT(system.Initialize(initInfo), "RenderSystem initialize failed");
    
    system.Shutdown();
    
    return TestResult::Passed;
}

TestResult TestRenderSystemRender() {
    RenderSystem system;
    MockRHIDevice mockDevice;
    
    RenderSystemInitInfo initInfo;
    initInfo.device = &mockDevice;
    initInfo.window = (primal::platform::window_handle)1;
    initInfo.width = 800;
    initInfo.height = 600;

    if (!system.Initialize(initInfo)) {
        return TestResult::Failed;
    }
    
    RenderScene scene;
    RenderView view;
    
    // Create a mesh
    RenderMesh mesh;
    float vertices[] = { 0.0f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f, -0.5f, -0.5f, 0.0f };
    primal::id::id_type meshEntityId = (primal::id::id_type)100;
    if (!mesh.Create(&mockDevice, meshEntityId, vertices, 3, sizeof(float) * 3)) {
        return TestResult::Failed;
    }

    // Setup Material
    Material material;
    rhi::BlendState blendState{};
    blendState.enableBlend = false;
    material.SetBlendState(blendState);
    
    MaterialInstance materialInstance(&material);
    materialInstance.Initialize(&mockDevice);
    
    // Register Material
    primal::id::id_type materialId = (primal::id::id_type)300;
    system.RegisterMaterialInstance(materialId, &materialInstance);
    
    // Add proxy pointing to this mesh
    RenderProxy proxy = RenderProxy::Create((primal::id::id_type)200, meshEntityId, materialId);
    scene.AddProxy(proxy);
    
    // Render
    // This should not crash and should internally cull and iterate
    system.Render(scene, view, 0);
    
    // Verify command buffer calls
    if (mockDevice.mockCmdBuffer) {
        TEST_ASSERT(mockDevice.mockCmdBuffer->resetCalled, "CommandBuffer Reset should be called");
        TEST_ASSERT(mockDevice.mockCmdBuffer->beginCalled, "CommandBuffer Begin should be called");
        TEST_ASSERT(mockDevice.mockCmdBuffer->endCalled, "CommandBuffer End should be called");
        // Submit is called on the command buffer object in RenderSystem::Render
        TEST_ASSERT(mockDevice.mockCmdBuffer->submitCalled, "CommandBuffer Submit should be called");
        
        // Check if Draw was called
        TEST_ASSERT(mockDevice.mockCmdBuffer->drawCalled, "Draw should be called for visible proxy");
        
        // Verify Z-Prepass integration
        // Expect 2 RenderPasses: 1 for DepthPrePass, 1 for MainPass
        TEST_ASSERT(mockDevice.mockCmdBuffer->beginRenderPassCount == 2, "Should have 2 RenderPasses (DepthPrePass + Main)");
        // Expect 2 Draw calls: 1 for DepthPrePass, 1 for OpaquePass
        TEST_ASSERT(mockDevice.mockCmdBuffer->drawCallCount == 2, "Should have 2 Draw calls (1 DepthPrePass + 1 Main)");
    } else {
        mesh.Destroy(&mockDevice);
        return TestResult::Failed;
    }
    
    // Verify SwapChain calls
    if (mockDevice.mockSwapChain) {
        TEST_ASSERT(mockDevice.mockSwapChain->presentCalled, "SwapChain Present should be called");
    } else {
        mesh.Destroy(&mockDevice);
        return TestResult::Failed;
    }
    
    mesh.Destroy(&mockDevice);
    system.Shutdown();
    
    return TestResult::Passed;
}

int main() {
    Engine::Test::TestSuite suite("RenderSystem Tests");
    
    suite.AddTestCase(Engine::Test::TestCase("TestRenderSystemInit", TestRenderSystemInit));
    suite.AddTestCase(Engine::Test::TestCase("TestRenderSystemRender", TestRenderSystemRender));
    
    suite.RunAllTests();
    
    return 0;
}
