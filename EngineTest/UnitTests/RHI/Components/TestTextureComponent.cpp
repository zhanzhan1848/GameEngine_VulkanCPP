#include "EngineTest/UnitTests/TestFramework.h"
#include "Engine/Graphics/RHI/Components/TextureComponent.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include "Engine/Graphics/RHI/Core/RHIResource.h"
#include <vector>
#include <map>

using namespace primal::graphics::rhi;
using namespace Engine::Test;

// === Mock Classes (Simplified for Texture) ===

class MockRHIDeviceForTexture : public RHIDeviceBase {
public:
    uint64_t nextHandle = 1;
    std::vector<ResourceHandle> createdTextures;
    std::vector<ResourceHandle> createdViews;

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
    ShaderHandle CreateShader(const void*, size_t, ShaderStage, const char*) override { return 0; }
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc&) override { return 0; }
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc&) override { return 0; }
    RenderPassHandle CreateRenderPass(const RenderPassDesc&) override { return 0; }
    void DestroyRenderPass(RenderPassHandle) override {}
    CommandBufferHandle CreateCommandBuffer(CommandQueueType) override { return 0; }
    void DestroyCommandBuffer(CommandBufferHandle) override {}
    void DestroyShader(ShaderHandle) override {}
    void DestroyPipeline(PipelineHandle) override {}
    bool GetQueryPoolResults(QueryPoolHandle, uint32_t, uint32_t, void*, size_t) override { return true; }
    double GetTimestampPeriod() const override { return 1.0; }
    RHIGarbageCollector& GetGarbageCollector() override { static RHIGarbageCollector gc; return gc; }
    ResourceHandle CreateBuffer(const BufferDesc&) override { return 0; }
    void DestroyBuffer(ResourceHandle) override {}
    
    // Implementing missing pure virtual methods
    void* MapBuffer(ResourceHandle, u64 = 0, u64 = 0) override { return nullptr; }
    void UnmapBuffer(ResourceHandle) override {}

    // === Target Methods Implementation ===
    
    ResourceHandle CreateTexture(const TextureDesc& /*desc*/) override {
        ResourceHandle h = nextHandle++;
        createdTextures.push_back(h);
        return h;
    }

    ResourceHandle CreateTextureView(const TextureViewDesc& /*desc*/) override {
        ResourceHandle h = nextHandle++;
        createdViews.push_back(h);
        return h;
    }
    
    void DestroyTexture(ResourceHandle handle) override {
        // Remove from textures
        for (auto it = createdTextures.begin(); it != createdTextures.end(); ++it) {
            if (*it == handle) {
                createdTextures.erase(it);
                return;
            }
        }
        // Remove from views
        for (auto it = createdViews.begin(); it != createdViews.end(); ++it) {
            if (*it == handle) {
                createdViews.erase(it);
                return;
            }
        }
    }
};

class TestTextureComponent : public TestSuite {
public:
    TestTextureComponent() : TestSuite("TextureComponentTests") {
        AddTestCase(TestCase("Lifecycle Test", [this]() { return TestLifecycle(); }));
    }

    TestResult TestLifecycle() {
        MockRHIDeviceForTexture device;
        
        // 1. Test Construction
        TextureDesc desc;
        desc.size = {256, 256, 1};
        desc.format = DataFormat::RGBA8_UNorm;
        desc.type = TextureType::Texture2D;
        desc.usage = TextureUsage::ShaderResource;
        
        TextureComponent texComp(desc, "TestTexture");
        
        TEST_ASSERT(texComp.handle == handles::INVALID_RESOURCE, "Handle should be invalid initially");
        TEST_ASSERT(texComp.desc.size.x == 256, "Width should match");
        TEST_ASSERT(texComp.desc.name == "TestTexture", "Name should match");
        
        // 2. Test Create
        bool created = texComp.Create(&device);
        TEST_ASSERT(created, "Create should succeed");
        TEST_ASSERT(texComp.IsValid(), "Component should be valid after creation");
        TEST_ASSERT(texComp.handle != handles::INVALID_RESOURCE, "Handle should be valid");
        TEST_ASSERT(texComp.defaultView != handles::INVALID_RESOURCE, "Default view should be created");
        TEST_ASSERT(device.createdTextures.size() == 1, "Device should have 1 texture");
        TEST_ASSERT(device.createdViews.size() == 1, "Device should have 1 view");
        
        // 3. Test CreateView
        TextureViewDesc viewDesc;
        viewDesc.viewType = TextureType::Texture2D;
        viewDesc.format = DataFormat::RGBA8_UNorm;
        viewDesc.mipCount = 1;
        
        ResourceHandle viewHandle = texComp.CreateView(&device, viewDesc);
        TEST_ASSERT(viewHandle != handles::INVALID_RESOURCE, "CreateView should succeed");
        TEST_ASSERT(device.createdViews.size() == 2, "Device should have 2 views now");
        
        // 4. Test Destroy
        texComp.Destroy(&device);
        TEST_ASSERT(!texComp.IsValid(), "Component should be invalid after destroy");
        TEST_ASSERT(texComp.handle == handles::INVALID_RESOURCE, "Handle should be reset");
        TEST_ASSERT(device.createdTextures.empty(), "Device texture should be destroyed");
        
        return TestResult::Passed;
    }
};

int main() {
    TestTextureComponent suite;
    TestStats stats = suite.RunAllTests();
    return stats.failedTests > 0 ? 1 : 0;
}
