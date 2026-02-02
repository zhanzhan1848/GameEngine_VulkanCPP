#include "TestFramework.h"
#include "Graphics/SceneDataAdapter.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIResource.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RenderMesh.h"

using namespace primal::graphics;
using namespace primal::graphics::rhi;
using namespace Engine::Test;

// --- Mock Classes ---

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
};

class MockDevice : public RHIDeviceBase {
public:
    MockDevice() { desc_.platform = RHIPlatform::Metal; }
    ~MockDevice() override {}

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
        return h;
    }
    
    ResourceHandle CreateTexture(const TextureDesc&) override { return handles::INVALID_RESOURCE; }
    ResourceHandle CreateTextureView(const TextureViewDesc&) override { return handles::INVALID_RESOURCE; }
    void DestroyBuffer(ResourceHandle h) override {
        delete reinterpret_cast<MockResource*>(h);
    }
    void DestroyTexture(ResourceHandle) override {}
    ShaderHandle CreateShader(const void*, size_t, ShaderStage, const char*) override { return handles::INVALID_SHADER; }
    void DestroyShader(ShaderHandle) override {}
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override { return handles::INVALID_PIPELINE; }
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc&) override { return handles::INVALID_PIPELINE; }
    void DestroyPipeline(PipelineHandle) override {}
    void* MapBuffer(ResourceHandle, u64, u64) override { return nullptr; }
    void UnmapBuffer(ResourceHandle) override {}
    RHIGarbageCollector& GetGarbageCollector() override { static RHIGarbageCollector gc; return gc; }
    
    CommandBufferHandle CreateCommandBuffer(CommandQueueType) override { return reinterpret_cast<CommandBufferHandle>(new MockCommandBuffer(*this)); }
    void DestroyCommandBuffer(CommandBufferHandle cmd) override { delete reinterpret_cast<MockCommandBuffer*>(cmd); }

private:
    DeviceDesc desc_;
    DeviceInfo info_;
};

// --- Test Suite ---

class TestSceneDataAdapter : public TestSuite {
public:
    TestSceneDataAdapter() : TestSuite("SceneDataAdapter Tests") {
        AddTestCase({"LoadSimpleScene", [this]() { return LoadSimpleScene(); }});
    }

private:
    void WriteString(std::vector<uint8_t>& buffer, const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.length());
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&len);
        buffer.insert(buffer.end(), ptr, ptr + sizeof(uint32_t));
        buffer.insert(buffer.end(), s.begin(), s.end());
    }

    template<typename T>
    void Write(std::vector<uint8_t>& buffer, T value) {
        const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
        buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
    }

    TestResult LoadSimpleScene() {
        MockDevice device;
        SceneDataAdapter adapter;

        // Construct Mock Data
        std::vector<uint8_t> data;
        
        // Scene Name
        WriteString(data, "TestScene");
        
        // Num LOD Groups
        Write<uint32_t>(data, 1);
        
        // LOD Group 0
        WriteString(data, "LOD0"); // LOD Name
        Write<uint32_t>(data, 1); // Num Meshes

        // Mesh 0
        WriteString(data, "Mesh0"); // Mesh Name
        Write<uint32_t>(data, 0); // LOD ID
        
        uint32_t vertexSize = sizeof(float) * 3 + sizeof(float) * 2; // Pos + UV
        uint32_t numVertices = 3;
        Write<uint32_t>(data, vertexSize); // Vertex Size
        Write<uint32_t>(data, numVertices); // Num Vertices
        
        uint32_t indexSize = sizeof(uint16_t);
        uint32_t numIndices = 3;
        Write<uint32_t>(data, indexSize); // Index Size
        Write<uint32_t>(data, numIndices); // Num Indices
        
        Write<float>(data, 0.5f); // LOD Threshold

        // Position Buffer (3 vertices * 3 floats)
        for (int i = 0; i < 3; ++i) {
            Write<float>(data, (float)i);
            Write<float>(data, (float)i);
            Write<float>(data, (float)i);
        }

        // Element Buffer (3 vertices * 2 floats)
        for (int i = 0; i < 3; ++i) {
            Write<float>(data, 0.0f);
            Write<float>(data, 1.0f);
        }

        // Index Buffer (3 indices * 2 bytes)
        Write<uint16_t>(data, 0);
        Write<uint16_t>(data, 1);
        Write<uint16_t>(data, 2);

        // Execute
        auto results = adapter.Load(&device, data.data(), (uint32_t)data.size());

        // Verify
        if (results.size() != 1) {
            std::cout << "Expected 1 mesh, got " << results.size() << std::endl;
            return TestResult::Failed;
        }
        
        const auto& info = results[0];
        if (info.name != "Mesh0") {
             std::cout << "Name mismatch: " << info.name << std::endl;
             return TestResult::Failed;
        }
        if (info.lodId != 0) return TestResult::Failed;
        if (info.mesh == nullptr) return TestResult::Failed;
        
        // Clean up
        for (auto& res : results) {
            if (res.mesh) {
                res.mesh->Destroy(&device);
                delete res.mesh;
            }
        }

        return TestResult::Passed;
    }
};

int main() {
    auto suite = std::make_shared<TestSceneDataAdapter>();
    TestRunner::RegisterTestSuite(suite);
    TestRunner::RunAllSuites();
    return 0;
}
