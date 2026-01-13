#include "CommonHeaders.h"
#include "Graphics/Material.h"
#include "Graphics/MaterialInstance.h"
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

// --- Mock Classes ---

class MockResource : public RHIResource {
public:
    MockResource(RHIDeviceBase& device, const ResourceDesc& desc)
        : RHIResource(device, desc) {
        // Mock buffer storage
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
    MockDevice() {} // Default constructor
    ~MockDevice() {}

    // Resource Creation
    ResourceHandle CreateBuffer(const BufferDesc& desc) override {
        auto* res = new MockResource(*this, ResourceDesc(ResourceType::Buffer, (ResourceUsage)desc.bindFlags, desc.usage, desc.size));
        ResourceHandle h = reinterpret_cast<ResourceHandle>(res);
        res->SetPublicHandle(h);
        return h;
    }
    
    ResourceHandle CreateTexture(const TextureDesc& /*desc*/) override { return (ResourceHandle)0x100; } // Dummy
    
    void DestroyBuffer(ResourceHandle handle) override {
        if (handle) delete reinterpret_cast<MockResource*>(handle);
    }
    void DestroyTexture(ResourceHandle /*handle*/) override {}

    // Shader & Pipeline
    ShaderHandle CreateShader(const void* /*data*/, size_t /*size*/, ShaderStage /*stage*/, const char* /*entryPoint*/) override {
        return (ShaderHandle)0x200;
    }
    void DestroyShader(ShaderHandle /*handle*/) override {}
    
    u64 pipelineCounter = 0x300;
    PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& /*desc*/) override {
        return (PipelineHandle)++pipelineCounter;
    }
    PipelineHandle CreateComputePipeline(const ComputePipelineDesc& /*desc*/) override { return handles::INVALID_PIPELINE; }
    void DestroyPipeline(PipelineHandle /*handle*/) override {}

    // Descriptor Sets
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& /*desc*/) override {
        return (DescriptorSetLayoutHandle)0x400;
    }
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle /*handle*/) override {}
    
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& /*desc*/) override {
        return (PipelineLayoutHandle)0x500;
    }
    void DestroyPipelineLayout(PipelineLayoutHandle /*handle*/) override {}
    
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& /*desc*/) override {
        return (DescriptorSetHandle)0x600;
    }
    void DestroyDescriptorSet(DescriptorSetHandle /*handle*/) override {}
    
    struct UpdateInfo {
        uint32_t binding;
        DescriptorType type;
    };
    std::vector<UpdateInfo> updates;

    void UpdateDescriptorSets(uint32_t writeCount, const WriteDescriptorSet* writes) override {
        for (uint32_t i=0; i<writeCount; ++i) {
            updates.push_back({writes[i].dstBinding, writes[i].descriptorType});
        }
    }

    // Mapping
    void* MapBuffer(ResourceHandle handle, u64 offset, u64 size) override {
        return reinterpret_cast<MockResource*>(handle)->Map(offset, size);
    }
    void UnmapBuffer(ResourceHandle handle) override {
        reinterpret_cast<MockResource*>(handle)->Unmap();
    }

    // Other unimplemented
    CommandBufferHandle CreateCommandBuffer(CommandQueueType) override { return handles::INVALID_COMMAND_BUFFER; }
    void DestroySync(SyncHandle) override {}
    void DestroyQueryPool(QueryPoolHandle) override {}
    void DestroySampler(SamplerHandle) override {}
    SamplerHandle CreateSampler(const SamplerDesc&) override { return (SamplerHandle)0x700; }
    
    // Abstract methods implementation for compilation
    
    bool Submit(const QueueSubmitInfo& /*info*/) override { return true; }
    
    SyncHandle CreateSync() override { return handles::INVALID_SYNC; }
    bool WaitForSync(SyncHandle, u32) override { return true; }
    // DestroySync is already defined above

    QueryPoolHandle CreateQueryPool(const QueryPoolDesc&) override { return handles::INVALID_QUERY_POOL; }
    // DestroyQueryPool is already defined above
    
    RHISwapChain* CreateSwapChain(const SwapChainDesc& /*desc*/) override { return nullptr; }
    void DestroySwapChain(RHISwapChain* /*swapChain*/) override {}

    // Explicitly implementing pure virtuals from RHIDeviceBase
    bool IsValid() const override { return true; }
    const DeviceInfo& GetDeviceInfo() const override { static DeviceInfo info; return info; }
    const DeviceDesc& GetDesc() const override { static DeviceDesc desc; return desc; }
    
    void WaitIdle() const override {}
    void Shutdown() override {}
    
    RHIGarbageCollector& GetGarbageCollector() override {
        static RHIGarbageCollector gc;
        return gc;
    }
};

// --- Tests ---

TestResult TestMaterialCreation() {
    MockDevice device;
    Material material;
    
    // Set some state
    material.SetUniformBlockSize(256);
    material.SetUniformBufferBinding(0);
    
    TEST_ASSERT_EQ(material.GetUniformBlockSize(), 256, "Uniform block size mismatch");
    TEST_ASSERT_EQ(material.GetUniformBufferBinding(), 0, "Uniform buffer binding mismatch");
    
    return TestResult::Passed;
}

TestResult TestMaterialPipeline() {
    MockDevice device;
    Material material;
    
    // Setup dummy shader bytecode
    std::vector<uint8_t> dummyBytecode = {1, 2, 3, 4};
    material.SetShader(ShaderStage::Vertex, dummyBytecode.data(), dummyBytecode.size(), "main");
    material.SetShader(ShaderStage::Pixel, dummyBytecode.data(), dummyBytecode.size(), "main");
    
    RenderPassHandle renderPass = (RenderPassHandle)0x800;
    PipelineHandle pipeline = material.GetPipeline(&device, renderPass);
    
    TEST_ASSERT(pipeline != handles::INVALID_PIPELINE, "Should create valid pipeline");
    
    // Verify caching
    PipelineHandle pipeline2 = material.GetPipeline(&device, renderPass);
    TEST_ASSERT_EQ(pipeline, pipeline2, "Should return cached pipeline");
    
    return TestResult::Passed;
}

TestResult TestMaterialInstanceInit() {
    MockDevice device;
    Material material;
    
    material.SetUniformBlockSize(256);
    material.SetUniformBufferBinding(0);
    material.SetDescriptorSetLayout((DescriptorSetLayoutHandle)0x400); // Mock layout
    
    MaterialInstance instance(&material);
    bool result = instance.Initialize(&device);
    
    TEST_ASSERT(result, "MaterialInstance init should succeed");
    
    // Verify UpdateDescriptorSets was called for Uniform Buffer
    // Initialize calls UpdateDescriptorSets if uniform buffer exists
    TEST_ASSERT_EQ(device.updates.size(), 1, "Should have 1 update (Uniform Buffer)");
    if (!device.updates.empty()) {
        TEST_ASSERT_EQ(device.updates[0].binding, 0, "Binding should be 0");
        TEST_ASSERT_EQ((int)device.updates[0].type, (int)DescriptorType::UniformBuffer, "Type should be UniformBuffer");
    }
    
    return TestResult::Passed;
}

TestResult TestMaterialInstanceUpdate() {
    MockDevice device;
    Material material;
    
    material.SetUniformBlockSize(256);
    material.SetDescriptorSetLayout((DescriptorSetLayoutHandle)0x400);
    
    MaterialInstance instance(&material);
    instance.Initialize(&device);
    
    device.updates.clear(); // Clear initial updates
    
    // Set Uniform Data
    float data = 1.0f;
    instance.SetUniformData(0, &data, sizeof(float));
    
    // Set Texture
    instance.SetTexture(1, (ResourceHandle)0x100);
    
    // Update
    instance.Update(&device);
    
    TEST_ASSERT_EQ(device.updates.size(), 1, "Should have 1 update (Texture)");
    if (!device.updates.empty()) {
        TEST_ASSERT_EQ(device.updates[0].binding, 1, "Binding should be 1");
        TEST_ASSERT_EQ((int)device.updates[0].type, (int)DescriptorType::SampledImage, "Type should be SampledImage");
    }
    
    return TestResult::Passed;
}

TestResult TestMaterialVariants() {
    MockDevice device;
    Material material;
    
    // Setup dummy shader bytecode
    std::vector<uint8_t> vs1 = {1};
    std::vector<uint8_t> ps1 = {2};
    std::vector<uint8_t> vs2 = {3};
    std::vector<uint8_t> ps2 = {4};
    
    // Permutation 0 (Default)
    material.SetShader(ShaderStage::Vertex, vs1.data(), vs1.size(), "main", 0);
    material.SetShader(ShaderStage::Pixel, ps1.data(), ps1.size(), "main", 0);
    
    // Permutation 1
    material.SetShader(ShaderStage::Vertex, vs2.data(), vs2.size(), "main", 1);
    material.SetShader(ShaderStage::Pixel, ps2.data(), ps2.size(), "main", 1);
    
    RenderPassHandle renderPass = (RenderPassHandle)0x800;
    
    // Get Pipeline 0
    PipelineHandle pipeline0 = material.GetPipeline(&device, renderPass, 0);
    TEST_ASSERT(pipeline0 != handles::INVALID_PIPELINE, "Should create pipeline 0");
    
    // Get Pipeline 1
    PipelineHandle pipeline1 = material.GetPipeline(&device, renderPass, 1);
    TEST_ASSERT(pipeline1 != handles::INVALID_PIPELINE, "Should create pipeline 1");
    
    // Pipelines should be different
    TEST_ASSERT(pipeline0 != pipeline1, "Pipelines for different permutations should be different");
    
    // Test Caching
    PipelineHandle pipeline0b = material.GetPipeline(&device, renderPass, 0);
    TEST_ASSERT_EQ(pipeline0, pipeline0b, "Should return cached pipeline for permutation 0");
    
    PipelineHandle pipeline1b = material.GetPipeline(&device, renderPass, 1);
    TEST_ASSERT_EQ(pipeline1, pipeline1b, "Should return cached pipeline for permutation 1");

    return TestResult::Passed;
}

int main() {
    auto suite = std::make_shared<TestSuite>("Material Tests");
    TEST_CASE((*suite), "TestMaterialCreation", TestMaterialCreation);
    TEST_CASE((*suite), "TestMaterialPipeline", TestMaterialPipeline);
    TEST_CASE((*suite), "TestMaterialInstanceInit", TestMaterialInstanceInit);
    TEST_CASE((*suite), "TestMaterialInstanceUpdate", TestMaterialInstanceUpdate);
    TEST_CASE((*suite), "TestMaterialVariants", TestMaterialVariants);
    
    TestRunner::RegisterTestSuite(suite);
    TestStats stats = TestRunner::RunAllSuites();
    
    return stats.failedTests > 0 ? 1 : 0;
}
