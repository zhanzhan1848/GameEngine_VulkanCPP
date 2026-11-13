#include "../../TestFramework.h"
#include "Engine/Graphics/RHI/Components/MaterialComponent.h"
#include "Engine/Graphics/Material.h"
#include "Engine/Graphics/MaterialInstance.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHIResource.h"
#include <vector>
#include <cstring>

using namespace primal::graphics;
using namespace primal::graphics::rhi;

// --- Mock Classes (Simplified from TestMaterial.cpp) ---

class MockResource : public RHIResource {
public:
    MockResource(RHIDeviceBase& device, const ResourceDesc& desc)
        : RHIResource(device, desc) {
        if (desc.type == ResourceType::Buffer) {
            data_.resize(desc.size);
        }
    }
    
    bool Initialize() override { return true; }
    void SetPublicHandle(ResourceHandle h) { SetHandle(h); }
    void* GetData() { return data_.data(); }

protected:
    void* mapImpl(uint64_t offset, uint64_t /*size*/) override { 
        return data_.data() + offset; 
    }
    void unmapImpl() override {}
    bool updateDataImpl(const void* src, uint64_t size, uint64_t offset) override {
        if (offset + size <= data_.size()) {
            memcpy(data_.data() + offset, src, size);
            return true;
        }
        return false;
    }
    void destroyImpl() override {}

private:
    std::vector<uint8_t> data_;
};

class MockDevice : public RHIDeviceBase {
public:
    DeviceInfo info;
    DeviceDesc desc;
    RHIGarbageCollector gc;
    std::vector<uint8_t> dummyBuffer;

    MockDevice() {
        info.maxConstantBufferSize = 65536;
        info.maxTexture2DSize = 8192;
        dummyBuffer.resize(65536);
    }

    bool IsValid() const override { return true; }
    const DeviceInfo& GetDeviceInfo() const override { return info; }
    const DeviceDesc& GetDesc() const override { return desc; }
    void WaitIdle() const override {}
    void Shutdown() override {}
    bool Submit(const QueueSubmitInfo& info) override { return true; }
    SyncHandle CreateSync() override { return 0; }
    bool WaitForSync(SyncHandle handle, u32 timeoutMs) override { return true; }
    void DestroySync(SyncHandle handle) override {}
    QueryPoolHandle CreateQueryPool(const QueryPoolDesc& desc) override { return 0; }
    void DestroyQueryPool(QueryPoolHandle handle) override {}
    SamplerHandle CreateSampler(const SamplerDesc& desc) override { return (SamplerHandle)0x500; }
    void DestroySampler(SamplerHandle handle) override {}
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override { return (DescriptorSetLayoutHandle)0x100; }
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle handle) override {}
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) override { return (PipelineLayoutHandle)0x200; }
    void DestroyPipelineLayout(PipelineLayoutHandle handle) override {}
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& desc) override { return (DescriptorSetHandle)0x600; }
    void DestroyDescriptorSet(DescriptorSetHandle handle) override {}
    void UpdateDescriptorSets(uint32_t writeCount, const WriteDescriptorSet* writes) override {}
    RHISwapChain* CreateSwapChain(const SwapChainDesc& desc) override { return nullptr; }
    void DestroySwapChain(RHISwapChain* swapChain) override {}
    ResourceHandle CreateBuffer(const BufferDesc& desc) override { return (ResourceHandle)0x300; }
    ResourceHandle CreateTexture(const TextureDesc& desc) override { return (ResourceHandle)0x400; }
    ResourceHandle CreateTextureView(const TextureViewDesc& desc) override { return (ResourceHandle)0x401; }
    ShaderHandle CreateShader(const void* data, size_t size, ShaderStage stage, const char* entryPoint) override { return (ShaderHandle)0x700; }
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override { return (PipelineHandle)0x800; }
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override { return (PipelineHandle)0x801; }
    RenderPassHandle CreateRenderPass(const RenderPassDesc& desc) override { return (RenderPassHandle)0x900; }
    void DestroyRenderPass(RenderPassHandle handle) override {}
    CommandBufferHandle CreateCommandBuffer(CommandQueueType type) override { return (CommandBufferHandle)0xA00; }
    void DestroyCommandBuffer(CommandBufferHandle handle) override {}
    void DestroyBuffer(ResourceHandle handle) override {}
    void DestroyTexture(ResourceHandle handle) override {}
    void DestroyShader(ShaderHandle handle) override {}
    void DestroyPipeline(PipelineHandle handle) override {}
    bool GetQueryPoolResults(QueryPoolHandle handle, uint32_t firstQuery, uint32_t queryCount, void* data, size_t stride) override { return true; }
    void* MapBuffer(ResourceHandle handle, u64 offset, u64 size) override { return dummyBuffer.data() + offset; }
    void UnmapBuffer(ResourceHandle handle) override {}
    double GetTimestampPeriod() const override { return 1.0; }
    RHIGarbageCollector& GetGarbageCollector() override { return gc; }
    // stale-test port: pure virtuals added to RHIDeviceBase after the Dawn era
    void SetBufferDirtySize(ResourceHandle handle, u64 size) override { (void)handle; (void)size; }
    RHIPlatform GetPlatform() const override { return RHIPlatform::Unknown; }
};

// --- Tests ---

Engine::Test::TestResult TestMaterialComponent_Create() {
    MockDevice device;
    Material material;
    
    // Setup material with valid layout
    material.SetDescriptorSetLayout((DescriptorSetLayoutHandle)0x100);
    
    MaterialComponent component;
    
    // Test Create
    bool result = component.Create(&device, &material);
    TEST_ASSERT(result, "Failed to create MaterialComponent");
    TEST_ASSERT(component.IsValid(), "Component should be valid after creation");
    TEST_ASSERT(component.GetMaterial() == &material, "Material pointer mismatch");
    
    // Test DescriptorSet
    // Note: MockDevice returns 0x600 for CreateDescriptorSet
    TEST_ASSERT(component.GetDescriptorSet() == (DescriptorSetHandle)0x600, "DescriptorSet handle mismatch");
    
    return Engine::Test::TestResult::Passed;
}

Engine::Test::TestResult TestMaterialComponent_SharedInstance() {
    MockDevice device;
    Material material;
    material.SetDescriptorSetLayout((DescriptorSetLayoutHandle)0x100);
    
    auto instance = std::make_shared<MaterialInstance>(&material);
    instance->Initialize(&device);
    
    MaterialComponent comp1(instance);
    MaterialComponent comp2(instance);
    
    TEST_ASSERT(comp1.IsValid(), "Comp1 invalid");
    TEST_ASSERT(comp2.IsValid(), "Comp2 invalid");
    TEST_ASSERT(comp1.GetMaterial() == &material, "Comp1 material mismatch");
    TEST_ASSERT(comp2.GetMaterial() == &material, "Comp2 material mismatch");
    
    TEST_ASSERT(comp1.GetDescriptorSet() == (DescriptorSetHandle)0x600, "Comp1 DS mismatch");
    TEST_ASSERT(comp2.GetDescriptorSet() == (DescriptorSetHandle)0x600, "Comp2 DS mismatch");
    
    return Engine::Test::TestResult::Passed;
}

Engine::Test::TestResult TestMaterialComponent_Parameters() {
    MockDevice device;
    Material material;
    material.SetDescriptorSetLayout((DescriptorSetLayoutHandle)0x100);
    material.SetUniformBlockSize(sizeof(float)); // Enable uniform buffer
    
    MaterialComponent component;
    component.Create(&device, &material);
    
    // Test parameter setting (just ensure no crash)
    component.SetTexture(0, (ResourceHandle)0x400);
    component.SetBuffer(1, (ResourceHandle)0x300, 256);
    component.SetSampler(2, (SamplerHandle)0x500);
    
    struct TestData {
        float value = 1.0f;
    } data;
    component.SetUniform(0, data);
    
    component.Update(&device, 1);
    
    return Engine::Test::TestResult::Passed;
}

int main() {
    Engine::Test::TestSuite suite("MaterialComponent Tests");
    
    suite.AddTestCase({"Create", TestMaterialComponent_Create});
    suite.AddTestCase({"SharedInstance", TestMaterialComponent_SharedInstance});
    suite.AddTestCase({"Parameters", TestMaterialComponent_Parameters});
    
    suite.RunAllTests();
    
    return 0;
}
