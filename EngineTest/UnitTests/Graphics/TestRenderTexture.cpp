#include "Graphics/RenderTexture.h"
#include "Engine/Graphics/RHI/Core/RHIDevice.h"
#include "Engine/Graphics/RHI/Core/RHIResource.h"
#include "Engine/Graphics/RHI/Core/RHICommand.h"
#include "Engine/Graphics/RHI/Core/RHITypes.h"
#include <iostream>
#include <vector>
#include <memory>
#include <cstring>
#include <cassert>

using namespace primal::graphics;
using namespace primal::graphics::rhi;

// === Mock Classes ===

class MockResource : public RHIResource {
public:
    MockResource(RHIDeviceBase& device, const ResourceDesc& desc)
        : RHIResource(device, desc) {
        // Allocate some dummy memory for mapping
        if (desc.memoryUsage != GPUMemoryUsage::Static && desc.memoryUsage != GPUMemoryUsage::Immutable) {
            data_.resize(std::max(1024ull, desc.size));
        }
    }

    bool Initialize() override { return true; }

    void* mapImpl(uint64_t offset, uint64_t size) override {
        if (data_.empty()) return nullptr;
        if (offset + size > data_.size()) return nullptr;
        return data_.data() + offset;
    }
    
    void unmapImpl() override {}
    
    bool updateDataImpl(const void* data, uint64_t size, uint64_t offset) override {
        if (data_.empty()) {
            // Allow update even if GPU only for mock purposes, or just return true
            return true;
        }
        if (offset + size > data_.size()) return false;
        memcpy(data_.data() + offset, data, size);
        return true;
    }

    void SetPublicHandle(ResourceHandle h) { handle_ = h; }

private:
    std::vector<uint8_t> data_;
};

class MockCommandBuffer : public RHICommandBuffer {
public:
    MockCommandBuffer(RHIDeviceBase& device, CommandQueueType type, CommandBufferHandle handle) 
        : RHICommandBuffer(device, type) {
        // Manually set handle since we are bypassing Factory
        // RHICommandBuffer::handle_ is protected, and we can access it
        this->handle_ = handle;
        this->state_ = CommandBufferState::Reset;
    }

    bool Initialize() override { return true; }
    
    // Base impls
    bool resetImpl() override { return true; }
    bool beginImpl() override { return true; }
    bool endImpl() override { return true; }
    bool submitImpl(uint32_t) override { return true; }
    bool waitForCompletionImpl() override { return true; }

    // Render Pass
    void BeginRenderPass(const RenderPassDesc&) override {}
    void BeginRenderPass(RenderPassHandle) override {}
    void EndRenderPass() override {}
    
    // Viewport/Scissor
    void SetViewport(const ViewportDesc&) override {}
    void SetScissor(const Rect&) override {}
    
    // Pipeline
    void BindGraphicsPipeline(PipelineHandle) override {}
    void BindComputePipeline(PipelineHandle) override {}
    
    // Vertex/Index
    void BindVertexBuffers(uint32_t, uint32_t, const ResourceHandle*, const uint64_t*) override {}
    void BindIndexBuffer(ResourceHandle, DataFormat, uint64_t) override {}
    void BindDescriptorSets(PipelineBindPoint, PipelineLayoutHandle, uint32_t, uint32_t, const DescriptorSetHandle*, uint32_t, const uint32_t*) override {}
    
    // Draw
    void Draw(uint32_t, uint32_t, uint32_t, uint32_t) override {}
    void DrawIndexed(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) override {}
    // DrawInstanced not in base
    // DrawIndexedInstanced not in base
    void DrawIndirect(ResourceHandle, uint64_t, uint32_t) override {}
    // DrawIndexedIndirect not in base
    
    // Dispatch
    void Dispatch(uint32_t, uint32_t, uint32_t) override {}
    void DispatchIndirect(ResourceHandle, uint64_t) override {}

    void WriteTimestamp(QueryPoolHandle queryPool, uint32_t queryIndex) override {} 
    // Resource Ops
    void CopyBuffer(ResourceHandle, ResourceHandle, uint64_t, uint64_t, uint64_t) override {}
    void CopyBufferToTexture(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void CopyTextureToBuffer(ResourceHandle, ResourceHandle, const BufferTextureCopyRegion*, uint32_t) override {}
    void BlitTexture(ResourceHandle, ResourceHandle, const TextureBlitRegion*, uint32_t, FilterMode) override {}
    void GenerateMipmaps(ResourceHandle) override {}
    // ResolveTexture not in base
    // CopyTexture not in base (use CopyTextureToTexture if needed, or Blit)

    // Barrier
    void InsertBarrier(const ResourceBarrier*, uint32_t) override {}
    
    // Removed Clear*, SetBlend*, SetStencil*, Debug* as they are not in RHICommandBuffer
};

class MockDevice : public RHIDeviceBase {
public:
    MockDevice() {}
    
    ~MockDevice() override {
        for (const auto& cmd : commandBuffers_) {
            rhi::UnregisterCommandBuffer(cmd->GetHandle());
        }
    }
    
    // Implement RHIDeviceBase pure virtuals
    bool IsValid() const override { return true; }
    const DeviceInfo& GetDeviceInfo() const override { static DeviceInfo info; return info; }
    const DeviceDesc& GetDesc() const override { static DeviceDesc desc; return desc; }
    double GetTimestampPeriod() const override { return 1.0; }
    void WaitIdle() const override {}
    void Shutdown() override {}
    
    bool Submit(const QueueSubmitInfo& info) override {
        if (info.cmdBuffer != handles::INVALID_COMMAND_BUFFER) {
            auto* cmd = rhi::GetCommandBuffer(info.cmdBuffer);
            if (cmd) {
                return cmd->Submit();
            }
        }
        return false;
    }

    // Sync
    SyncHandle CreateSync() override { return handles::INVALID_SYNC; }
    bool WaitForSync(SyncHandle, uint32_t) override { return true; }
    void DestroySync(SyncHandle) override {}
    
    // QueryPool
    QueryPoolHandle CreateQueryPool(const QueryPoolDesc&) override { return handles::INVALID_QUERY_POOL; }
    void DestroyQueryPool(QueryPoolHandle) override {}
    bool GetQueryPoolResults(QueryPoolHandle handle, uint32_t firstQuery, uint32_t queryCount, void* data, size_t stride) override { return false; }
    
    // Sampler
    SamplerHandle CreateSampler(const SamplerDesc&) override { return handles::INVALID_SAMPLER; }
    void DestroySampler(SamplerHandle) override {}
    
    // Descriptors & PipelineLayout
    DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc&) override { return handles::INVALID_DESCRIPTOR_SET_LAYOUT; }
    void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle) override {}
    
    PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc&) override { return handles::INVALID_PIPELINE_LAYOUT; }
    void DestroyPipelineLayout(PipelineLayoutHandle) override {}
    
    DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc&) override { return handles::INVALID_DESCRIPTOR_SET; }
    void DestroyDescriptorSet(DescriptorSetHandle) override {}
    void UpdateDescriptorSets(uint32_t, const WriteDescriptorSet*) override {}
    RHISwapChain* CreateSwapChain(const SwapChainDesc& /*desc*/) override { return nullptr; }
    void DestroySwapChain(RHISwapChain* /*swapChain*/) override {}
    
    RenderPassHandle CreateRenderPass(const RenderPassDesc& /*desc*/) override { return handles::INVALID_RENDER_PASS; }
    void DestroyRenderPass(RenderPassHandle /*handle*/) override {}

    // Resources
    ResourceHandle CreateBuffer(const BufferDesc& desc) override {
        // Fix: BufferDesc::usage is GPUMemoryUsage, bindFlags is ResourceUsage
        GPUMemoryUsage memUsage = desc.memoryUsage;
        if (memUsage == GPUMemoryUsage::Unknown && desc.usage != GPUMemoryUsage::Unknown) {
            memUsage = desc.usage;
        }
        
        ResourceUsage resUsage = (ResourceUsage)desc.bindFlags;
        // If Staging buffer has no usage flags, assume CopySource/Dest for compatibility
        if (memUsage == GPUMemoryUsage::Staging && resUsage == ResourceUsage::None) {
            resUsage = ResourceUsage::CopySource | ResourceUsage::CopyDest;
        }

        auto* res = new MockResource(*this, ResourceDesc(ResourceType::Buffer, resUsage, memUsage, desc.size));
        ResourceHandle h = reinterpret_cast<ResourceHandle>(res);
        res->SetPublicHandle(h);
        ResourceManager::Instance().RegisterResource(res);
        return h;
    }
    
    ResourceHandle CreateTexture(const TextureDesc& desc) override {
        auto* res = new MockResource(*this, ResourceDesc(ResourceType::Texture, (ResourceUsage)desc.usage, desc.memoryUsage, 0));
        ResourceHandle h = reinterpret_cast<ResourceHandle>(res);
        res->SetPublicHandle(h);
        ResourceManager::Instance().RegisterResource(res);
        return h;
    }
    
    void DestroyBuffer(ResourceHandle h) override {
        auto* res = ResourceManager::Instance().GetResource(h);
        if (res) {
            ResourceManager::Instance().UnregisterResource(h);
            delete res;
        }
    }
    
    void DestroyTexture(ResourceHandle h) override {
        DestroyBuffer(h); // Same logic
    }
    
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

    // Command Buffer
    CommandBufferHandle CreateCommandBuffer(CommandQueueType type) override {
        static uint64_t nextHandle = 1;
        CommandBufferHandle handle = nextHandle++;
        
        auto cmd = std::make_unique<MockCommandBuffer>(*this, type, handle);
        
        rhi::RegisterCommandBuffer(cmd.get());
        
        // Keep ownership
        commandBuffers_.push_back(std::move(cmd));
        
        return handle;
    }

    void DestroyCommandBuffer(CommandBufferHandle handle) override {}
    
private:
    std::vector<std::unique_ptr<MockCommandBuffer>> commandBuffers_;
};

// === Tests ===

class TestSuite {
public:
    static TestSuite& Instance() {
        static TestSuite instance;
        return instance;
    }
    
    void RegisterTest(const std::string& name, void (*testFunc)()) {
        tests_.push_back({name, testFunc});
    }
    
    int Run() {
        int passed = 0;
        int failed = 0;
        for (const auto& test : tests_) {
            std::cout << "Running " << test.name << "... ";
            try {
                test.func();
                std::cout << "PASSED" << std::endl;
                passed++;
            } catch (const std::exception& e) {
                std::cout << "FAILED: " << e.what() << std::endl;
                failed++;
            } catch (...) {
                std::cout << "FAILED: Unknown error" << std::endl;
                failed++;
            }
        }
        std::cout << "Result: " << passed << " passed, " << failed << " failed." << std::endl;
        return failed > 0 ? 1 : 0;
    }
    
private:
    struct Test {
        std::string name;
        void (*func)();
    };
    std::vector<Test> tests_;
};

#define TEST_CASE(name) \
    void name(); \
    struct Register##name { Register##name() { TestSuite::Instance().RegisterTest(#name, name); } } register##name; \
    void name()

#define ASSERT(condition) \
    if (!(condition)) throw std::runtime_error("Assertion failed: " #condition);

// --- Test Cases ---

TEST_CASE(TestRenderTextureCreation) {
    MockDevice device;
    RenderTexture texture;
    TextureDesc desc;
    desc.size = {1024, 768, 1};
    desc.format = DataFormat::RG8B8A8_UNorm;
    
    ASSERT(texture.Create(&device, primal::id::invalid_id, desc));
    ASSERT(texture.IsValid());
    ASSERT(texture.GetWidth() == 1024);
    ASSERT(texture.GetHeight() == 768);
}

TEST_CASE(TestRenderTextureUploadAsync) {
    MockDevice device;
    RenderTexture texture;
    TextureDesc desc;
    desc.size = {4, 4, 1};
    desc.format = DataFormat::RG8B8A8_UNorm;
    desc.memoryUsage = GPUMemoryUsage::Static; // Destination
    
    ASSERT(texture.Create(&device, primal::id::invalid_id, desc));
    
    uint32_t data[16];
    memset(data, 0xFF, sizeof(data));
    
    // UploadDataAsync should use staging buffer
    ASSERT(texture.UploadDataAsync(&device, data, sizeof(data)));
}

TEST_CASE(TestRenderTextureGenerateMipmaps) {
    MockDevice device;
    RenderTexture texture;
    TextureDesc desc;
    desc.size = {128, 128, 1};
    desc.format = DataFormat::RG8B8A8_UNorm;
    desc.mipLevels = 8;
    desc.usage = (TextureUsage)((uint32_t)TextureUsage::CopySource | (uint32_t)TextureUsage::CopyDest | (uint32_t)TextureUsage::ShaderResource);
    
    ASSERT(texture.Create(&device, primal::id::invalid_id, desc));
    ASSERT(texture.GenerateMipmaps(&device));
}

TEST_CASE(TestRenderTextureReadBack) {
    MockDevice device;
    RenderTexture texture;
    TextureDesc desc;
    desc.size = {4, 4, 1};
    desc.format = DataFormat::RG8B8A8_UNorm;
    desc.usage = TextureUsage::CopySource;
    desc.memoryUsage = GPUMemoryUsage::Static;
    
    ASSERT(texture.Create(&device, primal::id::invalid_id, desc));
    
    uint32_t data[16];
    ASSERT(texture.ReadBack(&device, data, sizeof(data)));
}

int main() {
    return TestSuite::Instance().Run();
}
