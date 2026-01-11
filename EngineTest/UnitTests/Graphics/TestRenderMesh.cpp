/**
 * @file TestRenderMesh.cpp
 * @brief RenderMesh 单元测试
 * @details 测试 RenderMesh 的创建、销毁和绘制功能，使用 TestFramework.h 框架
 */

#include "CommonHeaders.h"
#include "Graphics/RenderMesh.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include "TestFramework.h"
#include <vector>
#include <cstring>
#include <iostream>

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace Engine::Test;

// Mock Classes

class MockResource : public RHIResource {
public:
    MockResource(RHIDeviceBase& device, const ResourceDesc& desc)
        : RHIResource(device, desc) {}
    
    // Implement RHIResource pure virtuals
    bool Initialize() override { return true; }
    
    // Helper to access protected SetHandle
    void SetPublicHandle(ResourceHandle h) { SetHandle(h); }

protected:
    void* mapImpl(uint64_t /*offset*/, uint64_t /*size*/) override { return nullptr; }
    void unmapImpl() override {}
    bool updateDataImpl(const void* /*data*/, uint64_t /*size*/, uint64_t /*offset*/) override { return true; }
};

class MockCommandBuffer : public RHICommandBuffer {
public:
    std::vector<std::string> calls;

    MockCommandBuffer(RHIDeviceBase& device) : RHICommandBuffer(device, CommandQueueType::Graphics) {}

    // Lifecycle methods
    bool Initialize() override { return true; }
    
protected:
    void destroyImpl() override {}
    bool resetImpl() override { return true; }
    bool beginImpl() override { calls.push_back("Begin"); return true; }
    bool endImpl() override { calls.push_back("End"); return true; }
    bool submitImpl(uint32_t /*waitFlags*/) override { return true; }
    bool waitForCompletionImpl() override { return true; }

public:
    // Render Pass
    void BeginRenderPass(const RenderPassDesc& /*desc*/) override { calls.push_back("BeginRenderPass"); }
    void BeginRenderPass(RenderPassHandle /*renderPass*/) override { calls.push_back("BeginRenderPass"); }
    void EndRenderPass() override { calls.push_back("EndRenderPass"); }
    
    // Viewport & Scissor
    void SetViewport(const ViewportDesc& /*vp*/) override { calls.push_back("SetViewport"); }
    void SetScissor(const Rect& /*rect*/) override { calls.push_back("SetScissor"); }
    
    // Pipeline
    void BindGraphicsPipeline(PipelineHandle /*pipeline*/) override { calls.push_back("BindGraphicsPipeline"); }
    void BindComputePipeline(PipelineHandle /*pipeline*/) override { calls.push_back("BindComputePipeline"); }
    
    // Vertex & Index Buffers
    void BindVertexBuffers(uint32_t /*firstSlot*/, uint32_t /*slotCount*/, const ResourceHandle* /*buffers*/, const uint64_t* /*offsets*/) override { 
        calls.push_back("BindVertexBuffers"); 
    }
    void BindIndexBuffer(ResourceHandle /*buffer*/, DataFormat /*format*/, uint64_t /*offset*/) override { 
        calls.push_back("BindIndexBuffer"); 
    }
    
    // Descriptor Sets
    void BindDescriptorSets(PipelineBindPoint /*bindPoint*/, PipelineLayoutHandle /*pipelineLayout*/,
                           uint32_t /*firstSet*/, uint32_t /*setCount*/, const DescriptorSetHandle* /*descriptorSets*/,
                           uint32_t /*dynamicOffsetCount*/, const uint32_t* /*dynamicOffsets*/) override {
        calls.push_back("BindDescriptorSets");
    }
    
    // Draw
    void Draw(uint32_t /*vertexCount*/, uint32_t /*startVertex*/, uint32_t /*instanceCount*/, uint32_t /*startInstance*/) override { 
        calls.push_back("Draw"); 
    }
    void DrawIndexed(uint32_t /*indexCount*/, uint32_t /*startIndex*/, uint32_t /*baseVertex*/, uint32_t /*instanceCount*/, uint32_t /*startInstance*/) override { 
        calls.push_back("DrawIndexed"); 
    }
    void DrawIndirect(ResourceHandle /*buffer*/, uint64_t /*offset*/, uint32_t /*drawCount*/) override {
        calls.push_back("DrawIndirect");
    }
    
    // Compute
    void Dispatch(uint32_t /*x*/, uint32_t /*y*/, uint32_t /*z*/) override { calls.push_back("Dispatch"); }
    void DispatchIndirect(ResourceHandle /*buffer*/, uint64_t /*offset*/) override { calls.push_back("DispatchIndirect"); }
    
    // Copy & Barrier
    void CopyBuffer(ResourceHandle /*src*/, ResourceHandle /*dst*/, uint64_t /*srcOffset*/, uint64_t /*dstOffset*/, uint64_t /*size*/) override {
        calls.push_back("CopyBuffer");
    }
    void CopyBufferToTexture(ResourceHandle /*srcBuffer*/, ResourceHandle /*dstTexture*/, const BufferTextureCopyRegion* /*regions*/, uint32_t /*regionCount*/) override {
        calls.push_back("CopyBufferToTexture");
    }
    void CopyTextureToBuffer(ResourceHandle /*srcTexture*/, ResourceHandle /*dstBuffer*/, const BufferTextureCopyRegion* /*regions*/, uint32_t /*regionCount*/) override {
        calls.push_back("CopyTextureToBuffer");
    }
    void BlitTexture(ResourceHandle /*src*/, ResourceHandle /*dst*/, const TextureBlitRegion* /*regions*/, uint32_t /*regionCount*/, FilterMode /*filter*/) override {
        calls.push_back("BlitTexture");
    }
    void GenerateMipmaps(ResourceHandle /*texture*/) override {
        calls.push_back("GenerateMipmaps");
    }
    void InsertBarrier(const ResourceBarrier* /*barriers*/, uint32_t /*barrierCount*/) override {
        calls.push_back("InsertBarrier");
    }
};

class MockDevice : public RHIDeviceBase {
public:
    MockDevice() {
        desc_.platform = RHIPlatform::Metal;
    }
    virtual ~MockDevice() {}

    // Implement RHIDeviceBase pure virtuals
    bool IsValid() const override { return true; }
    const DeviceInfo& GetDeviceInfo() const override { return info_; }
    const DeviceDesc& GetDesc() const override { return desc_; }
    void WaitIdle() const override {}
    void Shutdown() override {}
    bool SubmitCommandBuffer(CommandBufferHandle /*handle*/) override { return true; }
    SyncHandle CreateSync() override { return handles::INVALID_SYNC; }
    bool WaitForSync(SyncHandle /*handle*/, uint32_t /*timeoutMs*/) override { return true; }
    void DestroySync(SyncHandle /*handle*/) override {}
    QueryPoolHandle CreateQueryPool(const QueryPoolDesc& /*desc*/) override { return handles::INVALID_QUERY_POOL; }
    void DestroyQueryPool(QueryPoolHandle /*handle*/) override {}
    SamplerHandle CreateSampler(const SamplerDesc& /*desc*/) override { return handles::INVALID_SAMPLER; }
    void DestroySampler(SamplerHandle /*handle*/) override {}
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& /*desc*/) override { return handles::INVALID_DESCRIPTOR_SET_LAYOUT; }
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle /*handle*/) override {}
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& /*desc*/) override { return handles::INVALID_PIPELINE_LAYOUT; }
    void DestroyPipelineLayout(PipelineLayoutHandle /*handle*/) override {}
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& /*desc*/) override { return handles::INVALID_DESCRIPTOR_SET; }
    void DestroyDescriptorSet(DescriptorSetHandle /*handle*/) override {}
    void UpdateDescriptorSets(uint32_t /*writeCount*/, const WriteDescriptorSet* /*writes*/) override {}
    
    ResourceHandle CreateBuffer(const BufferDesc& desc) override {
        auto* res = new MockResource(*this, ResourceDesc(ResourceType::Buffer, (ResourceUsage)desc.bindFlags, desc.usage, desc.size));
        ResourceHandle h = reinterpret_cast<ResourceHandle>(res);
        res->SetPublicHandle(h);
        ResourceManager::Instance().RegisterResource(res);
        return h;
    }
    
    ResourceHandle CreateTexture(const TextureDesc& /*desc*/) override { return handles::INVALID_RESOURCE; }
    
    void DestroyBuffer(ResourceHandle /*handle*/) override {
        // Mock implementation
    }
    
    void DestroyTexture(ResourceHandle /*handle*/) override {}
    
    ShaderHandle CreateShader(const void* /*data*/, size_t /*size*/, ShaderStage /*stage*/, const char* /*entryPoint*/) override { return handles::INVALID_SHADER; }
    void DestroyShader(ShaderHandle /*handle*/) override {}
    
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& /*desc*/) override { return handles::INVALID_PIPELINE; }
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc& /*desc*/) override { return handles::INVALID_PIPELINE; }
    void DestroyPipeline(PipelineHandle /*handle*/) override {}
    void* MapBuffer(ResourceHandle /*handle*/, u64 /*offset*/, u64 /*size*/) override { return nullptr; }
    void UnmapBuffer(ResourceHandle /*handle*/) override {}

    RHIGarbageCollector& GetGarbageCollector() override {
        static RHIGarbageCollector gc;
        return gc;
    }

    // Helper for test (overrides RHIDeviceBase)
    CommandBufferHandle CreateCommandBuffer(CommandQueueType type = CommandQueueType::Graphics) override { return reinterpret_cast<CommandBufferHandle>(new MockCommandBuffer(*this)); }
    void DestroyCommandBuffer(CommandBufferHandle cmd) { delete reinterpret_cast<MockCommandBuffer*>(cmd); }

private:
    DeviceDesc desc_;
    DeviceInfo info_;
};

// Test Cases

TestResult TestRenderMeshCreation() {
    MockDevice device;
    RenderMesh mesh;
    
    struct Vertex { float x, y, z; };
    std::vector<Vertex> vertices = {{0,0,0}, {1,1,1}, {2,2,2}};
    
    // Create Mesh
    primal::id::id_type testEntityId = 1001;
    bool result = mesh.Create(&device, testEntityId, vertices.data(), vertices.size(), sizeof(Vertex));
    
    TEST_ASSERT(result, "RenderMesh creation should succeed");
    TEST_ASSERT(mesh.IsValid(), "RenderMesh should be valid");
    TEST_ASSERT_EQ(mesh.GetVertexCount(), 3, "Vertex count should be 3");
    TEST_ASSERT_EQ(mesh.GetEntityId(), testEntityId, "Entity ID should match");
    
    mesh.Destroy(&device);
    return TestResult::Passed;
}

TestResult TestRenderMeshDraw() {
    MockDevice device;
    RenderMesh mesh;
    
    struct Vertex { float x, y, z; };
    std::vector<Vertex> vertices = {{0,0,0}, {1,1,1}, {2,2,2}};
    
    mesh.Create(&device, 0, vertices.data(), vertices.size(), sizeof(Vertex));
    
    MockCommandBuffer* cmd = reinterpret_cast<MockCommandBuffer*>(device.CreateCommandBuffer());
    cmd->Begin();
    cmd->calls.clear(); // Clear "Begin" call
    mesh.Draw(cmd);
    
    // Expect: BindVertexBuffers, Draw
    TEST_ASSERT_EQ(cmd->calls.size(), 2, "Draw should trigger 2 calls (BindVB + Draw)");
    if (cmd->calls.size() >= 2) {
        TEST_ASSERT_STR_EQ("BindVertexBuffers", cmd->calls[0].c_str(), "First call should be BindVertexBuffers");
        TEST_ASSERT_STR_EQ("Draw", cmd->calls[1].c_str(), "Second call should be Draw");
    }
    
    cmd->End();
    device.DestroyCommandBuffer(reinterpret_cast<CommandBufferHandle>(cmd));
    mesh.Destroy(&device);
    return TestResult::Passed;
}

TestResult TestRenderMeshIndexedDraw() {
    MockDevice device;
    RenderMesh mesh;
    
    struct Vertex { float x, y, z; };
    std::vector<Vertex> vertices = {{0.0f, 0.5f, 0.0f}, {0.5f, -0.5f, 0.0f}, {-0.5f, -0.5f, 0.0f}, {0.5f, 0.5f, 0.0f}};
    std::vector<uint16_t> indices = {0, 1, 2, 2, 1, 3};
    
    mesh.Create(&device, 0, vertices.data(), vertices.size(), sizeof(Vertex), indices.data(), indices.size(), DataIndexType::UInt16);
    
    MockCommandBuffer* cmd = reinterpret_cast<MockCommandBuffer*>(device.CreateCommandBuffer());
    cmd->Begin();
    cmd->calls.clear(); // Clear "Begin" call
    mesh.Draw(cmd);
    
    // Expect: BindVertexBuffers, BindIndexBuffer, DrawIndexed
    TEST_ASSERT_EQ(cmd->calls.size(), 3, "Indexed Draw should trigger 3 calls (BindVB + BindIB + DrawIndexed)");
    if (cmd->calls.size() >= 3) {
        TEST_ASSERT_STR_EQ("BindVertexBuffers", cmd->calls[0].c_str(), "First call should be BindVertexBuffers");
        TEST_ASSERT_STR_EQ("BindIndexBuffer", cmd->calls[1].c_str(), "Second call should be BindIndexBuffer");
        TEST_ASSERT_STR_EQ("DrawIndexed", cmd->calls[2].c_str(), "Third call should be DrawIndexed");
    }
    
    cmd->End();
    device.DestroyCommandBuffer(reinterpret_cast<CommandBufferHandle>(cmd));
    mesh.Destroy(&device);
    return TestResult::Passed;
}

TestResult TestRenderMeshRegistry() {
    MockDevice device;
    
    // Test Case 1: Registry Lookup
    RenderMesh mesh1;
    RenderMesh mesh2;
    struct Vertex { float x, y, z; };
    std::vector<Vertex> vertices = {{0,0,0}, {1,1,1}, {2,2,2}};
    
    primal::id::id_type id1 = 1001;
    primal::id::id_type id2 = 1002;
    
    // Before creation, should be nullptr
    TEST_ASSERT(RenderMesh::GetByEntityId(id1) == nullptr, "Should be nullptr before creation");
    
    // Create Mesh 1
    bool res1 = mesh1.Create(&device, id1, vertices.data(), vertices.size(), sizeof(Vertex));
    TEST_ASSERT(res1, "Mesh1 creation failed");
    RenderMesh* found1 = RenderMesh::GetByEntityId(id1);
    TEST_ASSERT(found1 == &mesh1, "Should find mesh1 by id1");
    
    // Create Mesh 2
    bool res2 = mesh2.Create(&device, id2, vertices.data(), vertices.size(), sizeof(Vertex));
    TEST_ASSERT(res2, "Mesh2 creation failed");
    TEST_ASSERT(RenderMesh::GetByEntityId(id2) == &mesh2, "Should find mesh2 by id2");
    TEST_ASSERT(RenderMesh::GetByEntityId(id1) == &mesh1, "Should still find mesh1 by id1");
    
    // Test Case 2: Unregister on Destroy
    mesh1.Destroy(&device);
    TEST_ASSERT(RenderMesh::GetByEntityId(id1) == nullptr, "Should be nullptr after destroy");
    TEST_ASSERT(RenderMesh::GetByEntityId(id2) == &mesh2, "mesh2 should still exist");
    
    // Test Case 3: Unregister on Destructor
    // mesh2 will be destroyed when going out of scope
    // We can't easily test this side effect inside this function without a nested scope
    {
        RenderMesh mesh3;
        primal::id::id_type id3 = 1003;
        mesh3.Create(&device, id3, vertices.data(), vertices.size(), sizeof(Vertex));
        TEST_ASSERT(RenderMesh::GetByEntityId(id3) == &mesh3, "Should find mesh3");
    } // mesh3 destructor called here
    TEST_ASSERT(RenderMesh::GetByEntityId(1003) == nullptr, "Should be nullptr after destructor");
    
    // Clean up mesh2
    mesh2.Destroy(&device);
    
    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("RenderMesh Tests");
    
    TEST_CASE((*suite), "RenderMesh Creation", TestRenderMeshCreation);
    TEST_CASE((*suite), "RenderMesh Draw", TestRenderMeshDraw);
    TEST_CASE((*suite), "RenderMesh Indexed Draw", TestRenderMeshIndexedDraw);
    TEST_CASE((*suite), "RenderMesh Registry", TestRenderMeshRegistry);
    
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    
    return 0;
}
