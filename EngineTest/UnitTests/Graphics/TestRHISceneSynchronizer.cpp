
/**
 * @file TestRHISceneSynchronizer.cpp
 * @brief RHISceneSynchronizer 单元测试
 * @details 测试 GamePlay RenderScene 到 RHI ECS 的数据同步功能
 */

#include "CommonHeaders.h"
#include "TestFramework.h"
#include "Graphics/RHI/Systems/RHISceneSynchronizer.h"
#include "Graphics/RHI/Core/RHIEntityManager.h"
#include "Graphics/RHI/Systems/RenderSystem.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/MaterialInstance.h"
#include "Graphics/RHI/Components/RenderLayerComponent.h"
#include "Graphics/RHI/Components/MaterialComponent.h"
#include "Graphics/RHI/Components/GPUBufferComponent.h"
#include "Graphics/RHI/Components/RHITransformComponent.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace Engine::Test;

// --- Mock Classes (Copied/Adapted from TestRenderMesh.cpp) ---

class MockResource : public RHIResource {
public:
    MockResource(RHIDeviceBase& device, const ResourceDesc& desc)
        : RHIResource(device, desc) {}
    
    bool Initialize() override { return true; }
    void SetPublicHandle(ResourceHandle h) { SetHandle(h); }
    
protected:
    void* mapImpl(uint64_t, uint64_t) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void*, uint64_t, uint64_t) override { return true; }
};

class MockCommandBuffer : public RHICommandBuffer {
public:
    MockCommandBuffer(RHIDeviceBase& device) : RHICommandBuffer(device, CommandQueueType::Graphics) {}
    bool Initialize() override { return true; }
protected:
    void destroyImpl() override {}
    bool resetImpl() override { return true; }
    bool beginImpl() override { return true; }
    bool endImpl() override { return true; }
    bool submitImpl(uint32_t) override { return true; }
    bool waitForCompletionImpl() override { return true; }
    
    // Minimal implementation of pure virtuals
    void BeginRenderPass(const RenderPassDesc&) override {}
    void BeginRenderPass(RenderPassHandle) override {}
    void EndRenderPass() override {}
    void SetViewport(const ViewportDesc&) override {}
    void SetScissor(const Rect&) override {}
    void BindGraphicsPipeline(PipelineHandle) override {}
    void BindComputePipeline(PipelineHandle) override {}
    void BindVertexBuffers(uint32_t, uint32_t, const ResourceHandle*, const uint64_t*) override {}
    void BindIndexBuffer(ResourceHandle, DataFormat, uint64_t) override {}
    void BindDescriptorSets(PipelineBindPoint, PipelineLayoutHandle, uint32_t, uint32_t, const DescriptorSetHandle*, uint32_t, const uint32_t*) override {}
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void DrawIndexed(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void DrawIndirect(ResourceHandle, uint64_t, uint32_t) override {}
    void Dispatch(uint32_t, uint32_t, uint32_t) override {}
    void DispatchIndirect(ResourceHandle, uint64_t) override {}
    void CopyBuffer(ResourceHandle, ResourceHandle, uint64_t, uint64_t, uint64_t) override {}
    void CopyBufferToTexture(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void CopyTextureToBuffer(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void BlitTexture(ResourceHandle, ResourceHandle, const TextureBlitRegion*, uint32_t, FilterMode) override {}
    void GenerateMipmaps(ResourceHandle) override {}
    void InsertBarrier(const ResourceBarrier*, uint32_t) override {}
    void WriteTimestamp(QueryPoolHandle, uint32_t) override {}
    // stale-test port: pure virtuals added to RHICommandBuffer after the Dawn era
    void PushConstants(PipelineLayoutHandle, ShaderStage, uint32_t, uint32_t, const void*) override {}
    void SetComputeBytes(uint32_t, const void*, uint32_t) override {}
    void MemoryBarrier(PipelineStage, PipelineStage, AccessFlag, AccessFlag) override {}
};

class MockDevice : public RHIDeviceBase {
public:
    MockDevice() { desc_.platform = RHIPlatform::Metal; }
    ~MockDevice() override {}

    // Implement RHIDeviceBase pure virtuals
    bool IsValid() const override { return true; }
    const DeviceInfo& GetDeviceInfo() const override { return info_; }
    const DeviceDesc& GetDesc() const override { return desc_; }
    double GetTimestampPeriod() const override { return 1.0; }
    void WaitIdle() const override {}
    void Shutdown() override {}
    bool Submit(const QueueSubmitInfo&) override { return true; }
    SyncHandle CreateSync() override { return handles::INVALID_SYNC; }
    bool WaitForSync(SyncHandle, uint32_t) override { return true; }
    void DestroySync(SyncHandle) override {}
    QueryPoolHandle CreateQueryPool(const QueryPoolDesc&) override { return handles::INVALID_QUERY_POOL; }
    void DestroyQueryPool(QueryPoolHandle) override {}
    bool GetQueryPoolResults(QueryPoolHandle, uint32_t, uint32_t, void*, size_t) override { return false; }
    SamplerHandle CreateSampler(const SamplerDesc&) override { return handles::INVALID_SAMPLER; }
    void DestroySampler(SamplerHandle) override {}
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc&) override { return handles::INVALID_DESCRIPTOR_SET_LAYOUT; }
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle) override {}
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc&) override { return handles::INVALID_PIPELINE_LAYOUT; }
    void DestroyPipelineLayout(PipelineLayoutHandle) override {}
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc&) override { return handles::INVALID_DESCRIPTOR_SET; }
    void DestroyDescriptorSet(DescriptorSetHandle) override {}
    void UpdateDescriptorSets(uint32_t, const WriteDescriptorSet*) override {}
    RHISwapChain* CreateSwapChain(const SwapChainDesc&) override { return nullptr; }
    void DestroySwapChain(RHISwapChain*) override {}
    RenderPassHandle CreateRenderPass(const RenderPassDesc&) override { return handles::INVALID_RENDER_PASS; }
    void DestroyRenderPass(RenderPassHandle) override {}
    
    ResourceHandle CreateBuffer(const BufferDesc& desc) override {
        auto* res = new MockResource(*this, ResourceDesc(ResourceType::Buffer, (ResourceUsage)desc.bindFlags, desc.usage, desc.size));
        ResourceHandle h = reinterpret_cast<ResourceHandle>(res);
        res->SetPublicHandle(h);
        ResourceManager::Instance().RegisterResource(res);
        return h;
    }
    
    ResourceHandle CreateTexture(const TextureDesc&) override { return handles::INVALID_RESOURCE; }
    ResourceHandle CreateTextureView(const TextureViewDesc&) override { return handles::INVALID_RESOURCE; }
    void DestroyBuffer(ResourceHandle) override {}
    void DestroyTexture(ResourceHandle) override {}
    ShaderHandle CreateShader(const void*, size_t, ShaderStage, const char*) override { return handles::INVALID_SHADER; }
    void DestroyShader(ShaderHandle) override {}
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override { return handles::INVALID_PIPELINE; }
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc&) override { return handles::INVALID_PIPELINE; }
    void DestroyPipeline(PipelineHandle) override {}
    void* MapBuffer(ResourceHandle, u64, u64) override { return nullptr; }
    void UnmapBuffer(ResourceHandle) override {}
    RHIGarbageCollector& GetGarbageCollector() override { static RHIGarbageCollector gc; return gc; }
    // stale-test port: pure virtuals added to RHIDeviceBase after the Dawn era
    void SetBufferDirtySize(ResourceHandle, u64) override {}
    RHIPlatform GetPlatform() const override { return desc_.platform; }
    
    CommandBufferHandle CreateCommandBuffer(CommandQueueType) override { return reinterpret_cast<CommandBufferHandle>(new MockCommandBuffer(*this)); }
    void DestroyCommandBuffer(CommandBufferHandle cmd) override { delete reinterpret_cast<MockCommandBuffer*>(cmd); }

private:
    DeviceDesc desc_;
    DeviceInfo info_;
};

// --- Test Suite ---

class TestRHISceneSynchronizer : public Engine::Test::TestSuite {
public:
    TestRHISceneSynchronizer() : Engine::Test::TestSuite("RHISceneSynchronizer Tests") {
        AddTestCase({"Synchronize New Entity", [this]() { return TestSynchronizeNewEntity(); }});
        AddTestCase({"Synchronize Update Entity", [this]() { return TestSynchronizeUpdateEntity(); }});
        AddTestCase({"Synchronize Remove Entity", [this]() { return TestSynchronizeRemoveEntity(); }});
    }

private:
    MockDevice device;
    RHIEntityManager entityManager;
    RenderSystem renderSystem;
    RHISceneSynchronizer synchronizer;

    TestResult TestSynchronizeNewEntity() {
        entityManager.Clear();
        synchronizer.Initialize(&entityManager, &device, &renderSystem);
        
        // Setup Resources
        RenderMesh mesh;
        struct Vertex { float x, y, z; };
        std::vector<Vertex> vertices = {{0,0,0}, {1,1,1}, {2,2,2}};
        primal::id::id_type meshId = 2001;
        mesh.Create(&device, meshId, vertices.data(), vertices.size(), sizeof(Vertex));
        
        primal::id::id_type materialId = 3001;
        MaterialInstance materialInstance(nullptr); // Using default constructor for test
        // stale-test port: RegisterMaterialInstance now owns via shared_ptr
        renderSystem.RegisterMaterialInstance(materialId, std::make_shared<MaterialInstance>(nullptr));
        
        // Create Scene and Proxy
        RenderScene scene;
        primal::id::id_type entityId = 1001;
        RenderProxy proxy = RenderProxy::Create(entityId, meshId, materialId);
        
        math::m4x4 transform = math::MatrixIdentity();
        transform.columns[0][3] = 10.0f; // Translate X
        proxy.UpdateTransform(transform);
        
        scene.AddProxy(proxy);
        
        // Execute Synchronization
        synchronizer.Synchronize(scene);
        
        // Verify
        // 1. Entity Count
        // EntityManager uses free list, first entity is index 0 (if cleared properly) or 1
        // RHIEntityID is uint32_t.
        // We iterate or check if count > 0.
        // RHIEntityManager doesn't expose "GetEntityCount" easily, but we can check if components exist.
        
        // We need to find the RHIEntityID that corresponds to gameEntityId.
        // But synchronizer hides the map.
        // However, we can iterate all valid entities in EntityManager.
        
        std::vector<RHIEntityID> entities;
        // Hack: Try first few IDs
        for(uint32_t i=0; i<100; ++i) {
            if (entityManager.IsAlive(i)) {
                entities.push_back(i);
            }
        }
        
        TEST_ASSERT_EQ(entities.size(), 1, "Should have 1 RHI entity");
        RHIEntityID rhiEntity = entities[0];
        
        // 2. Check Components
        auto* layer = entityManager.GetComponent<RenderLayerComponent>(rhiEntity);
        TEST_ASSERT(layer != nullptr, "Should have RenderLayerComponent");
        
        auto* matComp = entityManager.GetComponent<MaterialComponent>(rhiEntity);
        TEST_ASSERT(matComp != nullptr, "Should have MaterialComponent");
        // We can't easily check shared_ptr equality because synchronizer creates a new shared_ptr from raw pointer
        // But we can check if it's not null.
        TEST_ASSERT(matComp->materialInstance != nullptr, "MaterialInstance should be set");
        
        auto* bufferComp = entityManager.GetComponent<GPUBufferComponent>(rhiEntity);
        TEST_ASSERT(bufferComp != nullptr, "Should have GPUBufferComponent");
        TEST_ASSERT(bufferComp->IsValid(), "GPUBufferComponent should be valid");
        TEST_ASSERT_EQ(bufferComp->vertexCount, 3, "Vertex count should match");
        
        auto* transformComp = entityManager.GetComponent<RHITransformComponent>(rhiEntity);
        TEST_ASSERT(transformComp != nullptr, "Should have RHITransformComponent");
        // Check translation X
        TEST_ASSERT_FLOAT_EQ(transformComp->worldMatrix.columns[0][3], 10.0f, 0.001f, "Transform should match");
        
        // Cleanup
        mesh.Destroy(&device);
        synchronizer.Clear();
        entityManager.Clear();
        
        return TestResult::Passed;
    }
    
    TestResult TestSynchronizeUpdateEntity() {
        entityManager.Clear();
        synchronizer.Initialize(&entityManager, &device, &renderSystem);
        
        // Setup
        RenderScene scene;
        primal::id::id_type entityId = 1002;
        RenderProxy proxy = RenderProxy::Create(entityId, primal::id::invalid_id, primal::id::invalid_id);
        scene.AddProxy(proxy);
        
        // First Sync
        synchronizer.Synchronize(scene);
        
        // Get Entity
        RHIEntityID rhiEntity = 0;
        for(uint32_t i=0; i<100; ++i) { if(entityManager.IsAlive(i)) { rhiEntity = i; break; } }
        
        auto* transformComp = entityManager.GetComponent<RHITransformComponent>(rhiEntity);
        TEST_ASSERT(transformComp != nullptr, "Should have transform");
        
        // Update Proxy
        math::m4x4 newTransform = math::MatrixIdentity();
        newTransform.columns[1][3] = 5.0f; // Translate Y
        proxy.UpdateTransform(newTransform);
        scene.UpdateProxy(entityId, proxy);
        
        // Second Sync
        synchronizer.Synchronize(scene);
        
        // Check Update
        TEST_ASSERT_FLOAT_EQ(transformComp->worldMatrix.columns[1][3], 5.0f, 0.001f, "Transform should update");
        
        synchronizer.Clear();
        entityManager.Clear();
        return TestResult::Passed;
    }
    
    TestResult TestSynchronizeRemoveEntity() {
        entityManager.Clear();
        synchronizer.Initialize(&entityManager, &device, &renderSystem);
        
        RenderScene scene;
        primal::id::id_type entityId = 1003;
        RenderProxy proxy = RenderProxy::Create(entityId, primal::id::invalid_id, primal::id::invalid_id);
        scene.AddProxy(proxy);
        
        synchronizer.Synchronize(scene);
        
        // Verify exists
        bool exists = false;
        for(uint32_t i=0; i<100; ++i) { if(entityManager.IsAlive(i)) { exists = true; break; } }
        TEST_ASSERT(exists, "Entity should exist");
        
        // Remove from scene
        scene.RemoveProxy(entityId);
        
        synchronizer.Synchronize(scene);
        
        // Verify removed
        exists = false;
        for(uint32_t i=0; i<100; ++i) { if(entityManager.IsAlive(i)) { exists = true; break; } }
        TEST_ASSERT(!exists, "Entity should be removed");
        
        synchronizer.Clear();
        entityManager.Clear();
        return TestResult::Passed;
    }
};

int main() {
    auto suite = std::make_shared<TestRHISceneSynchronizer>();
    Engine::Test::TestRunner::RegisterTestSuite(suite);
    Engine::Test::TestRunner::RunAllSuites();
    return 0;
}
