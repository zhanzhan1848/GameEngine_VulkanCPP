#include "EngineTest/UnitTests/TestFramework.h"
#include "Engine/Graphics/RHI/Components/GPUBufferComponent.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIResource.h"
#include <vector>
#include <cstring>
#include <map>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

// === Mock Classes ===

class MockResource : public RHIResource {
public:
    std::vector<uint8_t> data;
    
    MockResource(RHIDeviceBase& device, const ResourceDesc& desc) : RHIResource(device, desc) {
        if (desc.size > 0) data.resize(desc.size);
    }
    
    bool Initialize() override { return true; }
    void* mapImpl(uint64_t offset, uint64_t size) override { 
        if (offset + size > data.size()) return nullptr;
        return data.data() + offset; 
    }
    void unmapImpl() override {}
    bool updateDataImpl(const void* src, uint64_t size, uint64_t offset) override {
        if (offset + size > data.size()) return false;
        memcpy(data.data() + offset, src, size);
        return true;
    }
    void SetPublicHandle(ResourceHandle h) { handle_ = h; }
};

class MockRHIDevice : public RHIDeviceBase {
public:
    std::map<ResourceHandle, MockResource*> resources;
    uint64_t nextHandle = 1;

    ~MockRHIDevice() {
        for (auto& pair : resources) delete pair.second;
        resources.clear();
    }

    // Required overrides
    bool IsValid() const override { return true; }
    const DeviceInfo& GetDeviceInfo() const override { static DeviceInfo i; return i; }
    const DeviceDesc& GetDesc() const override { static DeviceDesc d; return d; }
    void WaitIdle() const override {}
    void Shutdown() override {}
    bool Submit(const QueueSubmitInfo&) override { return true; }
    SyncHandle CreateSync() override { return 0; }
    bool WaitForSync(SyncHandle, u32) override { return true; }
    void DestroySync(SyncHandle) override {}
    QueryPoolHandle CreateQueryPool(const QueryPoolDesc&) override { return 0; }
    void DestroyQueryPool(QueryPoolHandle) override {}
    SamplerHandle CreateSampler(const SamplerDesc&) override { return 0; }
    void DestroySampler(SamplerHandle) override {}
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc&) override { return 0; }
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle) override {}
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc&) override { return 0; }
    void DestroyPipelineLayout(PipelineLayoutHandle) override {}
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc&) override { return 0; }
    void DestroyDescriptorSet(DescriptorSetHandle) override {}
    void UpdateDescriptorSets(uint32_t, const WriteDescriptorSet*) override {}
    RHISwapChain* CreateSwapChain(const SwapChainDesc&) override { return nullptr; }
    void DestroySwapChain(RHISwapChain*) override {}
    ResourceHandle CreateTexture(const TextureDesc&) override { return 0; }
    ResourceHandle CreateTextureView(const TextureViewDesc&) override { return 0; }
    ShaderHandle CreateShader(const void*, size_t, ShaderStage, const char*) override { return 0; }
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override { return 0; }
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc&) override { return 0; }
    RenderPassHandle CreateRenderPass(const RenderPassDesc&) override { return 0; }
    void DestroyRenderPass(RenderPassHandle) override {}
    CommandBufferHandle CreateCommandBuffer(CommandQueueType) override { return 0; }
    void DestroyCommandBuffer(CommandBufferHandle) override {}
    void DestroyTexture(ResourceHandle) override {}
    void DestroyShader(ShaderHandle) override {}
    void DestroyPipeline(PipelineHandle) override {}
    bool GetQueryPoolResults(QueryPoolHandle, uint32_t, uint32_t, void*, size_t) override { return true; }
    double GetTimestampPeriod() const override { return 1.0; }
    RHIGarbageCollector& GetGarbageCollector() override { static RHIGarbageCollector gc; return gc; }

    // === Target Methods Implementation ===
    
    ResourceHandle CreateBuffer(const BufferDesc& desc) override {
        // Create MockResource
        auto* res = new MockResource(*this, ResourceDesc(ResourceType::Buffer, ResourceUsage::None, desc.usage, desc.size));
        ResourceHandle h = nextHandle++;
        res->SetPublicHandle(h);
        resources[h] = res;
        return h;
    }
    
    void DestroyBuffer(ResourceHandle handle) override {
        auto it = resources.find(handle);
        if (it != resources.end()) {
            delete it->second;
            resources.erase(it);
        }
    }
    
    void* MapBuffer(ResourceHandle handle, u64 offset = 0, u64 size = 0) override {
        auto it = resources.find(handle);
        if (it != resources.end()) {
            // Map entire buffer if size is 0
            uint64_t mapSize = (size == 0) ? it->second->GetDesc().size : size;
            return it->second->Map(offset, mapSize);
        }
        return nullptr;
    }
    
    void UnmapBuffer(ResourceHandle handle) override {
        auto it = resources.find(handle);
        if (it != resources.end()) {
            it->second->Unmap();
        }
    }
};

class TestGPUBufferComponent : public TestSuite {
public:
    TestGPUBufferComponent() : TestSuite("GPUBufferComponentTests") {
        AddTestCase(TestCase("Constructor Initialization", [this]() { return TestConstructor(); }));
        AddTestCase(TestCase("Validity Check", [this]() { return TestValidity(); }));
        AddTestCase(TestCase("Functional Create", [this]() { return TestCreate(); }));
        AddTestCase(TestCase("Functional Map/Unmap", [this]() { return TestMapUnmap(); }));
    }

private:
    TestResult TestConstructor() {
        GPUBufferComponent defaultComp;
        TEST_ASSERT(defaultComp.handle == handles::INVALID_RESOURCE, "Default handle should be INVALID_RESOURCE");
        TEST_ASSERT(defaultComp.mappedPointer == nullptr, "Default mappedPointer should be nullptr");
        return TestResult::Passed;
    }

    TestResult TestValidity() {
        GPUBufferComponent comp;
        TEST_ASSERT(!comp.IsValid(), "Empty component should be invalid");
        
        comp.handle = 1;
        comp.size = 100;
        TEST_ASSERT(comp.IsValid(), "Initialized component should be valid");
        return TestResult::Passed;
    }
    
    TestResult TestCreate() {
        MockRHIDevice device;
        GPUBufferComponent comp;
        comp.size = 1024;
        comp.type = BufferType::Vertex;
        comp.usage = GPUMemoryUsage::Static;
        
        bool result = comp.Create(&device);
        TEST_ASSERT(result, "Create should succeed");
        TEST_ASSERT(comp.handle != handles::INVALID_RESOURCE, "Handle should be valid after create");
        TEST_ASSERT(comp.IsValid(), "Component should be valid after create");
        
        // Verify underlying resource was created
        TEST_ASSERT(device.resources.find(comp.handle) != device.resources.end(), "Resource should exist in device");
        
        comp.Destroy(&device);
        TEST_ASSERT(comp.handle == handles::INVALID_RESOURCE, "Handle should be invalid after destroy");
        TEST_ASSERT(device.resources.find(1) == device.resources.end(), "Resource should be removed from device"); // Assuming handle was 1
        return TestResult::Passed;
    }
    
    TestResult TestMapUnmap() {
        MockRHIDevice device;
        GPUBufferComponent comp;
        comp.size = 256;
        comp.usage = GPUMemoryUsage::Dynamic; // Host visible
        
        TEST_ASSERT(comp.Create(&device), "Create should succeed");
        
        void* ptr = comp.Map(&device);
        TEST_ASSERT(ptr != nullptr, "Map should return valid pointer");
        TEST_ASSERT(comp.mappedPointer == ptr, "Component should store mapped pointer");
        
        // Write some data
        int* intPtr = static_cast<int*>(ptr);
        *intPtr = 42;
        TEST_ASSERT(*intPtr == 42, "Should be able to write to mapped memory");
        
        comp.Unmap(&device);
        TEST_ASSERT(comp.mappedPointer == nullptr, "Unmap should clear mapped pointer");
        
        // Test Upload
        int data = 123;
        bool uploadRes = comp.Upload(&device, &data, sizeof(int));
        TEST_ASSERT(uploadRes, "Upload should succeed");
        
        // Verify data in mock resource
        MockResource* res = device.resources[comp.handle];
        int* resData = reinterpret_cast<int*>(res->data.data());
        TEST_ASSERT(*resData == 123, "Data should be uploaded correctly");
        
        comp.Destroy(&device);
        return TestResult::Passed;
    }
};

int main() {
    TestGPUBufferComponent suite;
    TestStats stats = suite.RunAllTests();
    return stats.failedTests > 0 ? 1 : 0;
}
