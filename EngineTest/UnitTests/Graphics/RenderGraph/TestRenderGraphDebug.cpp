/**
 * @file TestRenderGraphDebug.cpp
 * @brief Unit tests for the RenderGraphDebug system.
 * 
 * This file contains unit tests to verify the functionality of the RenderGraphDebug class,
 * specifically focusing on texture preview rendering and debug visualization.
 * It uses mock RHI components to simulate the graphics backend.
 */

#include "../../TestFramework.h"
#include "Graphics/RenderGraph/RenderGraphDebug.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include <iostream>

using namespace primal::graphics::rendergraph;
using namespace primal::graphics::rhi;

/**
 * @brief Mock RHI Device for Debug Testing.
 * 
 * Simulates a valid RHI device and returns valid handles for resource creation
 * to ensure RenderGraphDebug can proceed without errors.
 */
class MockDebugRHIDevice : public RHIDeviceBase {
public:
    virtual bool IsValid() const override { return true; }
    virtual const DeviceInfo& GetDeviceInfo() const override { static DeviceInfo info; return info; }
    virtual const DeviceDesc& GetDesc() const override { static DeviceDesc desc; return desc; }
    virtual void WaitIdle() const override {}
    virtual void Shutdown() override {}
    virtual bool Submit(const QueueSubmitInfo& info) override { return true; }
    virtual SyncHandle CreateSync() override { return handles::INVALID_SYNC; }
    virtual bool WaitForSync(SyncHandle handle, u32 timeoutMs) override { return true; }
    virtual void DestroySync(SyncHandle handle) override {}
    virtual QueryPoolHandle CreateQueryPool(const QueryPoolDesc& desc) override { return handles::INVALID_QUERY_POOL; }
    virtual void DestroyQueryPool(QueryPoolHandle handle) override {}
    
    // Return valid handles to simulate successful creation
    virtual SamplerHandle CreateSampler(const SamplerDesc& desc) override { return {1}; }
    virtual void DestroySampler(SamplerHandle handle) override {}
    
    virtual DescriptorSetLayoutHandle CreateDescriptorSetLayout(const DescriptorSetLayoutDesc& desc) override { return {1}; }
    virtual void DestroyDescriptorSetLayout(DescriptorSetLayoutHandle handle) override {}
    
    virtual PipelineLayoutHandle CreatePipelineLayout(const PipelineLayoutDesc& desc) override { return {1}; }
    virtual void DestroyPipelineLayout(PipelineLayoutHandle handle) override {}
    
    virtual DescriptorSetHandle CreateDescriptorSet(const DescriptorSetDesc& desc) override { return {static_cast<uint32_t>(dsCounter_++)}; }
    virtual void DestroyDescriptorSet(DescriptorSetHandle handle) override {}
    virtual void UpdateDescriptorSets(uint32_t writeCount, const WriteDescriptorSet* writes) override {}
    
    virtual RHISwapChain* CreateSwapChain(const SwapChainDesc& desc) override { return nullptr; }
    virtual void DestroySwapChain(RHISwapChain* swapChain) override {}

    virtual RenderPassHandle CreateRenderPass(const RenderPassDesc& desc) override { return {1}; }
    virtual void DestroyRenderPass(RenderPassHandle handle) override {}
    
    virtual ResourceHandle CreateBuffer(const BufferDesc& desc) override { return {1}; }
    virtual ResourceHandle CreateTexture(const TextureDesc& desc) override { return {1}; }
    
    virtual ShaderHandle CreateShader(const void* data, size_t size, ShaderStage stage, const char* entryPoint = "main") override { return {1}; }
    
    virtual PipelineHandle CreateGraphicsPipeline(const GraphicsPipelineDesc& desc) override { return {1}; }
    virtual PipelineHandle CreateComputePipeline(const ComputePipelineDesc& desc) override { return {1}; }
    
    virtual CommandBufferHandle CreateCommandBuffer(CommandQueueType type) override { return {1}; }
    virtual void DestroyCommandBuffer(CommandBufferHandle handle) override {}
    
    virtual void DestroyBuffer(ResourceHandle handle) override {}
    virtual void DestroyTexture(ResourceHandle handle) override {}
    virtual void DestroyShader(ShaderHandle) override {}
    virtual void DestroyPipeline(PipelineHandle) override {}
    
    virtual void* MapBuffer(ResourceHandle, u64 = 0, u64 = 0) override { 
        static char buffer[1024]; // Dummy buffer
        return buffer; 
    }
    virtual void UnmapBuffer(ResourceHandle) override {}
    
    virtual RHIGarbageCollector& GetGarbageCollector() override { static RHIGarbageCollector gc; return gc; }

private:
    uint32_t dsCounter_ = 1;
};

/**
 * @brief Mock RHI Command Buffer for Debug Testing.
 * 
 * Captures command buffer calls to verify that RenderGraphDebug issues the correct
 * drawing commands.
 */
class MockDebugCommandBuffer : public RHICommandBuffer {
public:
    MockDebugCommandBuffer(RHIDeviceBase& device) : RHICommandBuffer(device, CommandQueueType::Graphics) {}

    // Track calls
    int drawCalls = 0;
    int bindPipelineCalls = 0;
    int bindDescriptorSetCalls = 0;

    virtual void BeginRenderPass(const RenderPassDesc& desc) override {}
    virtual void BeginRenderPass(RenderPassHandle renderPass) override {}
    virtual void EndRenderPass() override {}
    virtual void SetViewport(const ViewportDesc& viewport) override {}
    virtual void SetScissor(const Rect& scissor) override {}
    
    virtual void BindGraphicsPipeline(PipelineHandle pipeline) override {
        bindPipelineCalls++;
    }
    virtual void BindVertexBuffers(uint32_t firstSlot, uint32_t slotCount, const ResourceHandle* buffers, const uint64_t* offsets) override {}
    virtual void BindIndexBuffer(ResourceHandle buffer, DataFormat format, uint64_t offset = 0) override {}
    
    virtual void BindDescriptorSets(PipelineBindPoint bindPoint, PipelineLayoutHandle pipelineLayout, uint32_t firstSet, uint32_t setCount, const DescriptorSetHandle* descriptorSets, uint32_t dynamicOffsetCount, const uint32_t* dynamicOffsets) override {
        bindDescriptorSetCalls++;
    }
    
    virtual void Draw(uint32_t vertexCount, uint32_t startVertex = 0, uint32_t instanceCount = 1, uint32_t startInstance = 0) override {
        drawCalls++;
    }
    virtual void DrawIndexed(uint32_t indexCount, uint32_t startIndex = 0, uint32_t baseVertex = 0, uint32_t instanceCount = 1, uint32_t startInstance = 0) override {}
    virtual void DrawIndirect(ResourceHandle buffer, uint64_t offset = 0, uint32_t drawCount = 1) override {}
    
    virtual void BindComputePipeline(PipelineHandle pipeline) override {}
    virtual void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override {}
    virtual void DispatchIndirect(ResourceHandle buffer, uint64_t offset = 0) override {}
    
    virtual void CopyBuffer(ResourceHandle src, ResourceHandle dst, uint64_t srcOffset = 0, uint64_t dstOffset = 0, uint64_t size = 0) override {}
    virtual void CopyBufferToTexture(ResourceHandle srcBuffer, ResourceHandle dstTexture, const BufferTextureCopyRegion* regions, uint32_t regionCount) override {}
    virtual void CopyTextureToBuffer(ResourceHandle srcTexture, ResourceHandle dstBuffer, const BufferTextureCopyRegion* regions, uint32_t regionCount) override {}
    virtual void BlitTexture(ResourceHandle src, ResourceHandle dst, const TextureBlitRegion* regions, uint32_t regionCount, FilterMode filter) override {}
    virtual void GenerateMipmaps(ResourceHandle texture) override {}
    virtual void InsertBarrier(const ResourceBarrier* barriers, uint32_t barrierCount) override {}
    virtual bool Initialize() override { return true; }

protected:
    virtual bool resetImpl() override { return true; }
    virtual bool beginImpl() override { return true; }
    virtual bool endImpl() override { return true; }
    virtual bool submitImpl(uint32_t waitFlags) override { return true; }
    virtual bool waitForCompletionImpl() override { return true; }
};

/**
 * @brief Tests the texture preview functionality of RenderGraphDebug.
 * 
 * Verifies that:
 * 1. Texture preview drawing is triggered.
 * 2. Correct number of draw calls are issued.
 * 3. Pipeline and descriptor sets are bound correctly.
 */
Engine::Test::TestResult TestTexturePreview() {
    MockDebugRHIDevice device;
    MockDebugCommandBuffer cmdBuffer(device);
    // Use relative path from build directory to source
    RenderGraphDebug debug(device, "../Engine/Graphics/Metal/shaders/DebugGraph.metal");
    
    // Enable debug
    debug.ToggleEnabled(); // Default false -> true
    
    // Setup dummy RenderGraph
    RenderGraph graph(device);
    
    // Add a dummy texture resource to debug list
    // We need to simulate a resource that has a valid physical handle
    
    // Create a dummy resource manually
    TextureDesc texDesc;
    texDesc.size = {100, 100, 1};
    RenderGraphTexture resource("TestTexture", {1, 0}, texDesc);
    resource.SetPhysicalHandle({1}); // Valid handle
    
    std::vector<std::pair<std::string, RenderGraphResource*>> debugResources;
    debugResources.push_back({"TestTexture", &resource});
    
    debug.SetDebugResources(debugResources);
    
    // Draw
    debug.Draw(&cmdBuffer, graph, 800, 600);
    
    // Check results
    // Expect 1 draw call for graph (if not empty) and 1 draw call for texture
    // Actually graph is empty, so BuildGraphMesh produces 0 vertices.
    // So only texture preview should draw.
    
    if (cmdBuffer.drawCalls != 1) {
        std::cerr << "Expected 1 draw call (texture preview), got " << cmdBuffer.drawCalls << std::endl;
        return Engine::Test::TestResult::Failed;
    }
    
    if (cmdBuffer.bindPipelineCalls < 1) {
        std::cerr << "Expected at least 1 pipeline bind, got " << cmdBuffer.bindPipelineCalls << std::endl;
        return Engine::Test::TestResult::Failed;
    }
    
    if (cmdBuffer.bindDescriptorSetCalls != 1) {
        std::cerr << "Expected 1 descriptor set bind, got " << cmdBuffer.bindDescriptorSetCalls << std::endl;
        return Engine::Test::TestResult::Failed;
    }
    
    return Engine::Test::TestResult::Passed;
}

int main() {
    std::cout << "Running RenderGraphDebug Tests..." << std::endl;
    auto result = TestTexturePreview();
    if (result == Engine::Test::TestResult::Passed) {
        std::cout << "[PASSED] TestTexturePreview" << std::endl;
        return 0;
    } else {
        std::cout << "[FAILED] TestTexturePreview" << std::endl;
        return 1;
    }
}
