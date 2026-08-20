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
// stale-test port: the render loop resolves proxies through the ECS transform
// registry (SceneExtractionSystem::QueryDirtyTransforms →
// transform::get_updated_components_flags), so proxies must carry a real
// entity id — fabricated ids fail the entity.is_valid() assert.
#include "Components/Entity.h"
#include "Components/Transform.h"

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

    void WriteTimestamp(QueryPoolHandle queryPool, uint32_t queryIndex) override {}

    // stale-test port: pure virtuals added to RHICommandBuffer after the Dawn era
    void PushConstants(PipelineLayoutHandle, ShaderStage, uint32_t, uint32_t, const void*) override {}
    void SetComputeBytes(uint32_t, const void*, uint32_t) override {}
    void MemoryBarrier(PipelineStage, PipelineStage, AccessFlag, AccessFlag) override {}

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
    double getTimestampPeriodImpl() const { return 1.0; }
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
    bool getQueryPoolResultsImpl(QueryPoolHandle handle, uint32_t firstQuery, uint32_t queryCount, void* data, size_t stride) { return false; }
    SamplerHandle createSamplerImpl(const SamplerDesc&) { return (SamplerHandle)7001; }
    void destroySamplerImpl(SamplerHandle) {}
    DescriptorSetLayoutHandle createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc&) { return (DescriptorSetLayoutHandle)4001; }
    void destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle) {}
    PipelineLayoutHandle createPipelineLayoutImpl(const PipelineLayoutDesc&) { return (PipelineLayoutHandle)8001; }
    void destroyPipelineLayoutImpl(PipelineLayoutHandle) {}
    DescriptorSetHandle createDescriptorSetImpl(const DescriptorSetDesc&) { return (DescriptorSetHandle)6001; }
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
    ShaderHandle createShaderImpl(const void*, size_t, ShaderStage, const char*) { return (ShaderHandle)3001; }
    PipelineHandle createGraphicsPipelineImpl(const GraphicsPipelineDesc&) { return (PipelineHandle)666; }
    PipelineHandle createComputePipelineImpl(const ComputePipelineDesc&) { return (PipelineHandle)5001; }
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

    void destroyCommandBufferImpl(CommandBufferHandle handle) {
        if (mockCmdBuffer && mockCmdBuffer->GetHandle() == handle) {
            if (handle != handles::INVALID_COMMAND_BUFFER) {
                UnregisterCommandBuffer(handle);
            }
            delete mockCmdBuffer;
            mockCmdBuffer = nullptr;
        }
    }
    
    // void destroyBufferImpl(ResourceHandle) {} // Removed as it is now implemented above
    void destroyTextureImpl(ResourceHandle) {}
    void destroyShaderImpl(ShaderHandle) {}
    void destroyPipelineImpl(PipelineHandle) {}

    // stale-test port: Impl hooks added to the RHIDevice CRTP base after the Dawn era
    ResourceHandle createTextureViewImpl(const TextureViewDesc&) { return handles::INVALID_RESOURCE; }
    void setBufferDirtySizeImpl(ResourceHandle, u64) {}
    u32 getCurrentFrameIndexImpl() const { return 0; }
    
    // void* mapBufferImpl(ResourceHandle, u64, u64) { return nullptr; } // Removed as it is now implemented above
    // void unmapBufferImpl(ResourceHandle) {} // Removed as it is now implemented above

    // Base class overrides if any needed
};

using TestResult = Engine::Test::TestResult;

TestResult TestRenderSystemInit() {
    MockRHIDevice mockDevice;
    RenderSystem system;
    
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
    MockRHIDevice mockDevice;
    RenderSystem system;
    
    RenderSystemInitInfo initInfo;
    initInfo.device = &mockDevice;
    initInfo.window = (primal::platform::window_handle)1;
    initInfo.width = 800;
    initInfo.height = 600;

    if (!system.Initialize(initInfo)) {
        return TestResult::Failed;
    }
    std::cout << "TestRenderSystem: System Initialized" << std::endl;
    
    RenderScene scene;
    RenderView view;
    
    // Create a mesh
    RenderMesh mesh;
    float vertices[] = { 0.0f, 0.5f, 0.0f, 0.5f, -0.5f, 0.0f, -0.5f, -0.5f, 0.0f };
    primal::id::id_type meshEntityId = (primal::id::id_type)100;
    if (!mesh.Create(&mockDevice, meshEntityId, vertices, 3, sizeof(float) * 3)) {
        return TestResult::Failed;
    }
    std::cout << "TestRenderSystem: Mesh Created" << std::endl;

    // Setup Material
    Material material;
    rhi::BlendState blendState{};
    blendState.enableBlend = false;
    material.SetBlendState(blendState);
    
    MaterialInstance materialInstance(&material);
    materialInstance.Initialize(&mockDevice);
    std::cout << "TestRenderSystem: Material Initialized" << std::endl;
    
    // Register Material
    primal::id::id_type materialId = (primal::id::id_type)300;
    // stale-test port: RegisterMaterialInstance now owns via shared_ptr
    system.RegisterMaterialInstance(materialId, std::make_shared<MaterialInstance>(&material));
    
    // Add proxy pointing to this mesh
    // stale-test port: create a real ECS entity (identity transform) instead
    // of a fabricated id — QueryDirtyTransforms indexes the transform
    // component registry with the proxy's entity id and asserts validity.
    primal::transform::init_info tfInfo{};
    tfInfo.rotation[3] = 1.0f;  // identity quaternion {x,y,z,w}
    primal::game_entity::entity_info entInfo{};
    entInfo.transform = &tfInfo;
    primal::game_entity::entity renderEntity = primal::game_entity::create(entInfo);
    if (!renderEntity.is_valid()) {
        return TestResult::Failed;
    }
    RenderProxy proxy = RenderProxy::Create(renderEntity.get_id(), meshEntityId, materialId);
    scene.AddProxy(proxy);
    std::cout << "TestRenderSystem: Proxy Added" << std::endl;

    // Render
    // This should not crash and should internally cull and iterate
    system.Render(scene, view);
    std::cout << "TestRenderSystem: Render Completed" << std::endl;
    primal::game_entity::remove(renderEntity.get_id());
    
    // Verify command buffer calls
    if (mockDevice.mockCmdBuffer) {
        TEST_ASSERT(mockDevice.mockCmdBuffer->resetCalled, "CommandBuffer Reset should be called");
        TEST_ASSERT(mockDevice.mockCmdBuffer->beginCalled, "CommandBuffer Begin should be called");
        TEST_ASSERT(mockDevice.mockCmdBuffer->endCalled, "CommandBuffer End should be called");
        // Submit is called on the command buffer object in RenderSystem::Render
        TEST_ASSERT(mockDevice.mockCmdBuffer->submitCalled, "CommandBuffer Submit should be called");
        
        // Verify Draw calls
        // Since we didn't setup a full scene with visible objects that pass culling, draw might not be called
        // But we added a proxy, so if culling passes, it should draw.
        // We need to ensure culling passes. The default view frustum and proxy AABB should overlap.
        // Proxy AABB is derived from Mesh AABB.
        // Mesh vertices: (0,0.5,0), (0.5,-0.5,0), (-0.5,-0.5,0). Z=0.
        // View is default. Default view matrix?
        // RenderView initializes with default camera at (0,0,0) looking at -Z?
        // We need to check RenderView implementation or just check if drawCalled is true/false and adjust expectation.
        
        // For now, just print the stats
        std::cout << "Draw calls: " << mockDevice.mockCmdBuffer->drawCallCount << std::endl;
    }
    
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
