/**
 * @file TestRHIBatchRenderer.cpp
 * @brief RHI智能批渲染优化器单元测试
 * @details 测试渲染项智能分类、动态批处理优化和GPU实例化功能
 * @author GameEngine VulkanCPP Team
 * @date 2025-12-31
 * @version 0.1.0
 */

#include "../../TestFramework.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHIBatchRenderer.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include <chrono>
#include <thread>
#include <random>
#include <fstream>
#include <vector>
#include <sstream>
#include <unordered_set>
#include <cstring>
#include <atomic>

using namespace Engine::Test;
using namespace primal::graphics::rhi;

// === 模拟设备类 ===

// 简单的RHIDevice实现用于测试
class MockRHIDevice : public RHIDevice<MockRHIDevice> {
public:
    MockRHIDevice() : RHIDevice<MockRHIDevice>(DeviceDesc{}) {
        auto& desc = const_cast<DeviceDesc&>(GetDesc());
        desc.platform = RHIPlatform::Metal;
        desc.enableDebug = false;
        // 初始化设备
        Initialize();
    }
    
    // === CRTP实现方法 ===
    bool initializeImpl() {
        return true;
    }
    
    void queryDeviceInfo(DeviceInfo& info) {
        strcpy(info.deviceName, "MockDevice");
        strcpy(info.driverVersion, "1.0.0");
    }
    
    uint32_t getCurrentFrameIndexImpl() const {
        return 0;
    }
    
    void waitIdleImpl() const {
        // Mock implementation
    }
    
    void shutdownImpl() {
        // Mock implementation
    }

    double getTimestampPeriodImpl() const { return 1.0; }
    
    // 资源创建实现（测试用，返回无效句柄）
    ResourceHandle createBufferImpl(const BufferDesc& desc) {
        return handles::INVALID_RESOURCE;
    }
    
    ResourceHandle createTextureImpl(const TextureDesc& desc) {
        return handles::INVALID_RESOURCE;
    }
    
    ShaderHandle createShaderImpl(const void* data, size_t size, ShaderStage stage, const char* entryPoint) {
        return handles::INVALID_SHADER;
    }
    
    PipelineHandle createGraphicsPipelineImpl(const GraphicsPipelineDesc& desc) {
        return handles::INVALID_PIPELINE;
    }
    
    PipelineHandle createComputePipelineImpl(const ComputePipelineDesc& desc) {
        return handles::INVALID_PIPELINE;
    }

    RenderPassHandle createRenderPassImpl(const RenderPassDesc& desc) {
        return handles::INVALID_RENDER_PASS;
    }

    void* mapBufferImpl(ResourceHandle handle, u64 offset, u64 size) { return nullptr; }
    void unmapBufferImpl(ResourceHandle handle) {}
    
    // 创建管线布局实现（测试用）
    PipelineLayoutHandle createPipelineLayoutImpl(const PipelineLayoutDesc& desc) {
        return handles::INVALID_PIPELINE_LAYOUT;
    }

    DescriptorSetLayoutHandle createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc& desc) {
        return handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }

    DescriptorSetHandle createDescriptorSetImpl(const DescriptorSetDesc& desc) {
        return handles::INVALID_DESCRIPTOR_SET;
    }

    SamplerHandle createSamplerImpl(const SamplerDesc& desc) {
        return handles::INVALID_SAMPLER;
    }

    QueryPoolHandle createQueryPoolImpl(const QueryPoolDesc& desc) {
        return handles::INVALID_QUERY_POOL;
    }
    
    bool getQueryPoolResultsImpl(QueryPoolHandle handle, uint32_t firstQuery, uint32_t queryCount, void* data, size_t stride) {
        return false;
    }

    CommandBufferHandle createCommandBufferImpl(CommandQueueType type) {
        return handles::INVALID_COMMAND_BUFFER;
    }
    
    // 资源销毁实现（测试用，空实现）
    void destroyBufferImpl(ResourceHandle handle) {}
    void destroyTextureImpl(ResourceHandle handle) {}
    void destroyShaderImpl(ShaderHandle handle) {}
    void destroyPipelineImpl(PipelineHandle handle) {}
    void destroyRenderPassImpl(RenderPassHandle handle) {}
    void destroyCommandBufferImpl(CommandBufferHandle handle) {}
    // 销毁管线布局实现
    void destroyPipelineLayoutImpl(PipelineLayoutHandle handle) {}
    void destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle handle) {}
    void destroyDescriptorSetImpl(DescriptorSetHandle handle) {}
    void destroySamplerImpl(SamplerHandle handle) {}
    void destroyQueryPoolImpl(QueryPoolHandle handle) {}
    void destroySyncImpl(SyncHandle handle) {}

    void updateDescriptorSetsImpl(uint32_t writeCount, const WriteDescriptorSet* writes) {}

    // SwapChain 实现
    RHISwapChain* createSwapChainImpl(const SwapChainDesc& desc) {
        return nullptr;
    }

    void destroySwapChainImpl(RHISwapChain* swapChain) {
    }
    
    // 命令提交实现
    bool submitImpl(const QueueSubmitInfo& info) {
        return true; // Mock实现，总是返回成功
    }
    
    // 同步对象创建实现
    SyncHandle createSyncImpl() {
        return nextSyncHandle_++;
    }
    
    // 同步等待实现
    bool waitForSyncImpl(SyncHandle handle, u32 timeoutMs) {
        return WaitForSync(handle, timeoutMs);
    }
    
    SyncHandle CreateSync() {
        return 1; // Mock实现，返回固定句柄
    }
    
    bool WaitForSync(SyncHandle handle, u32 timeoutMs) {
        return true; // Mock实现，总是返回成功
    }
    
private:
    // === 私有成员变量 ===
    std::atomic<uint64_t> nextSyncHandle_{1};
};

// === 模拟命令缓冲区类 ===

class MockRHICommandBuffer : public RHICommandBuffer {
public:
    struct DrawCall {
        PipelineHandle pipeline;
        ResourceHandle vertexBuffer;
        ResourceHandle indexBuffer;
        u32 vertexCount;
        u32 indexCount;
        u32 instanceCount;
        u32 startIndex;
        u32 startVertex;
        bool isIndexed;
    };
    
    MockRHICommandBuffer(MockRHIDevice& device) 
        : RHICommandBuffer(device, CommandQueueType::Graphics) {}
    
    std::vector<DrawCall> drawCalls;
    
    // 提供对绘制调用的访问
    const std::vector<DrawCall>& GetDrawCalls() const { return drawCalls; }
    void ClearDrawCalls() { 
        drawCalls.clear(); 
        currentPipeline_ = handles::INVALID_PIPELINE;
        currentVertexBuffer_ = handles::INVALID_RESOURCE;
        currentIndexBuffer_ = handles::INVALID_RESOURCE;
    }
    
protected:
    // 实现基类的纯虚函数
    bool Initialize() override { 
        currentState_ = CommandBufferState::Reset;
        return true; 
    }
    
    void BeginRenderPass(const RenderPassDesc& desc) override { 
        (void)desc; 
        currentState_ = CommandBufferState::Recording;
    }

    // 开始渲染通道（使用句柄）
    void BeginRenderPass(RenderPassHandle renderPass) override { 
        (void)renderPass; 
        currentState_ = CommandBufferState::Recording;
    }
    
    void EndRenderPass() override { 
        currentState_ = CommandBufferState::RecordingEnded;
    }
    
    void WriteTimestamp(QueryPoolHandle queryPool, uint32_t queryIndex) override {}

    void SetViewport(const ViewportDesc& viewport) override { (void)viewport; }
    void SetScissor(const Rect& scissor) override { (void)scissor; }
    void BindGraphicsPipeline(PipelineHandle pipeline) override { 
        currentPipeline_ = pipeline; 
    }
    
    void BindVertexBuffers(uint32_t firstSlot, uint32_t slotCount, const ResourceHandle* buffers, const uint64_t* offsets) override { 
        (void)firstSlot; (void)offsets;
        if (slotCount > 0) {
            currentVertexBuffer_ = buffers[0]; 
        }
    }
    
    void BindIndexBuffer(ResourceHandle buffer, DataFormat format, uint64_t offset) override { 
        (void)format; (void)offset;
        currentIndexBuffer_ = buffer; 
    }
    
    void Draw(uint32_t vertexCount, uint32_t startVertex, uint32_t instanceCount, uint32_t startInstance) override { 
        DrawCall call;
        call.pipeline = currentPipeline_;
        call.vertexBuffer = currentVertexBuffer_;
        call.indexBuffer = currentIndexBuffer_;
        call.vertexCount = vertexCount;
        call.indexCount = 0;
        call.instanceCount = instanceCount;
        call.startIndex = 0;
        call.startVertex = startVertex;
        call.isIndexed = false;
        drawCalls.push_back(call);
    }
    
    void DrawIndexed(uint32_t indexCount, uint32_t startIndex, uint32_t baseVertex, uint32_t instanceCount, uint32_t startInstance) override { 
        (void)startInstance;
        DrawCall call;
        call.pipeline = currentPipeline_;
        call.vertexBuffer = currentVertexBuffer_;
        call.indexBuffer = currentIndexBuffer_;
        call.vertexCount = 0;
        call.indexCount = indexCount;
        call.instanceCount = instanceCount;
        call.startIndex = startIndex;
        call.startVertex = baseVertex;
        call.isIndexed = true;
        drawCalls.push_back(call);
    }
    
    void DrawIndirect(ResourceHandle buffer, uint64_t offset, uint32_t drawCount) override { (void)buffer; (void)offset; (void)drawCount; }
    void BindComputePipeline(PipelineHandle pipeline) override { (void)pipeline; }
    void BindDescriptorSets(PipelineBindPoint bindPoint, PipelineLayoutHandle pipelineLayout, uint32_t firstSet, uint32_t setCount, const DescriptorSetHandle* descriptorSets, uint32_t dynamicOffsetCount, const uint32_t* dynamicOffsets) override {
        (void)bindPoint; (void)pipelineLayout; (void)firstSet; (void)setCount; (void)descriptorSets; (void)dynamicOffsetCount; (void)dynamicOffsets;
    }
    void Dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override { (void)groupCountX; (void)groupCountY; (void)groupCountZ; }
    void DispatchIndirect(ResourceHandle buffer, uint64_t offset) override { (void)buffer; (void)offset; }
    void CopyBuffer(ResourceHandle src, ResourceHandle dst, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) override { (void)src; (void)dst; (void)srcOffset; (void)dstOffset; (void)size; }
    void CopyBufferToTexture(ResourceHandle srcBuffer, ResourceHandle dstTexture, const BufferTextureCopyRegion* regions, uint32_t regionCount) override { (void)srcBuffer; (void)dstTexture; (void)regions; (void)regionCount; }
    void CopyTextureToBuffer(ResourceHandle srcTexture, ResourceHandle dstBuffer, const BufferTextureCopyRegion* regions, uint32_t regionCount) override { (void)srcTexture; (void)dstBuffer; (void)regions; (void)regionCount; }
    void BlitTexture(ResourceHandle src, ResourceHandle dst, const TextureBlitRegion* regions, uint32_t regionCount, FilterMode filter) override { (void)src; (void)dst; (void)regions; (void)regionCount; (void)filter; }
    void GenerateMipmaps(ResourceHandle texture) override { (void)texture; }
    void InsertBarrier(const ResourceBarrier* barriers, uint32_t barrierCount) override { (void)barriers; (void)barrierCount; }
    
    void destroyImpl() override {}
    bool resetImpl() override { return true; }
    bool beginImpl() override { return true; }
    bool endImpl() override { return true; }
    bool submitImpl(uint32_t waitFlags) override { (void)waitFlags; return true; }
    bool waitForCompletionImpl() override { return true; }
    
private:
    const CommandStats& GetStats() const { 
        static CommandStats stats; 
        return stats; 
    }
    
    // === 私有成员变量 ===
    PipelineHandle currentPipeline_ = handles::INVALID_PIPELINE;
    ResourceHandle currentVertexBuffer_ = handles::INVALID_RESOURCE;
    ResourceHandle currentIndexBuffer_ = handles::INVALID_RESOURCE;
    CommandBufferState currentState_ = CommandBufferState::Reset;
};

// === 测试辅助函数 ===

RenderItem CreateTestRenderItem(u32 id, bool isInstanced = false) {
    RenderItem item;
    item.key.pipeline = static_cast<PipelineHandle>(id % 3 + 1); // 3种管线
    item.key.vertexBuffer = static_cast<ResourceHandle>(id % 2 + 1); // 2种顶点缓冲
    item.key.indexBuffer = static_cast<ResourceHandle>(id % 2 + 1);
    item.key.material = static_cast<ResourceHandle>(id % 4 + 1); // 4种材质
    item.key.materialSlot = id % 2;
    item.key.vertexStride = 32;
    item.key.indexFormat = DataFormat::R32_UInt;
    item.key.renderTarget = id % 2;
    
    // 变换矩阵
    item.worldMatrix = primal::graphics::rhi::math::MatrixIdentity();
    item.worldMatrix = primal::graphics::rhi::math::MatrixTranslation(
        simd::float3{
            static_cast<f32>(id * 10.0f),
            static_cast<f32>(id * 5.0f),
            static_cast<f32>(id * 2.0f)
        }
    );
    
    item.color = primal::math::v4{1.0f, 1.0f, 1.0f, 1.0f};
    item.startVertex = id * 4;
    item.vertexCount = 4 + id % 8;
    item.startIndex = id * 6;
    item.indexCount = 6 + id % 12;
    item.instanceCount = isInstanced ? (1 + id % 5) : 1;
    item.startInstance = 0;
    item.depth = static_cast<f32>(id % 100);
    item.materialID = id % 10;
    item.visibilityMask = 0xFF;
    item.isInstanced = isInstanced;
    
    return item;
}

// === 测试用例 ===

/**
 * @brief 测试批渲染器基本功能
 */
TestResult TestBatchRendererBasicFunctionality() {
    MockRHIDevice device{};
    device.Initialize();
    
    BatchConfig config;
    RHIBatchRenderer batchRenderer(device, config);
    
    // 测试初始化
    TEST_ASSERT(batchRenderer.Initialize(), "批渲染器应该能够初始化");
    
    // 测试添加渲染项
    RenderItem item = CreateTestRenderItem(1);
    batchRenderer.AddRenderItem(item);
    
    // 测试处理批次
    primal::math::m4x4 viewMatrix = primal::graphics::rhi::math::MatrixIdentity();
    primal::math::m4x4 projMatrix = primal::graphics::rhi::math::MatrixIdentity();
    
    u32 batchCount = batchRenderer.ProcessBatches(viewMatrix, projMatrix);
    (void)batchCount;
    TEST_ASSERT(batchCount > 0, "处理批次应该产生至少一个批次");
    
    // 测试统计信息
    const BatchStats& stats = batchRenderer.GetStats();
    TEST_ASSERT(stats.totalRenderItems == 1, "应该有1个渲染项");
    TEST_ASSERT(stats.totalBatches == 1, "应该有1个批次");
    
    batchRenderer.Shutdown();
    device.Shutdown();
    
    return TestResult::Passed;
}

/**
 * @brief 测试渲染项分类功能
 */
TestResult TestRenderItemClassification() {
    MockRHIDevice device{};
    device.Initialize();
    
    BatchConfig config;
    RHIBatchRenderer batchRenderer(device, config);
    batchRenderer.Initialize();
    
    // 创建不同类型的渲染项
    std::vector<RenderItem> items;
    for (u32 i = 0; i < 10; ++i) {
        items.push_back(CreateTestRenderItem(i));
    }
    
    batchRenderer.AddRenderItems(items.data(), static_cast<u32>(items.size()));
    
    // 处理批次
    primal::math::m4x4 viewMatrix = primal::graphics::rhi::math::MatrixIdentity();
    primal::math::m4x4 projMatrix = primal::graphics::rhi::math::MatrixIdentity();
    
    batchRenderer.ProcessBatches(viewMatrix, projMatrix);
    
    // 验证分类结果
    const auto& batches = batchRenderer.GetBatches();
    TEST_ASSERT(!batches.empty(), "应该有批次生成");
    
    // 验证批次内渲染项具有相同的键
    for (const auto& batch : batches) {
        if (batch.items.empty()) continue;
        
        const RenderItemKey& firstKey = batch.items[0].key;
        for (const auto& item : batch.items) {
            TEST_ASSERT(item.key == firstKey, "批次内所有渲染项应该具有相同的键");
        }
    }
    
    batchRenderer.Shutdown();
    device.Shutdown();
    
    return TestResult::Passed;
}

/**
 * @brief 测试批处理优化功能
 */
TestResult TestBatchOptimization() {
    MockRHIDevice device{};
    device.Initialize();
    
    BatchConfig config;
    config.maxBatchSize = 3; // 设置较小的批次大小以触发分割
    RHIBatchRenderer batchRenderer(device, config);
    batchRenderer.Initialize();
    
    // 创建多个相同类型的渲染项
    std::vector<RenderItem> items;
    for (u32 i = 0; i < 8; ++i) {
        RenderItem item = CreateTestRenderItem(1); // 相同类型
        item.depth = static_cast<f32>(i);
        items.push_back(item);
    }
    
    batchRenderer.AddRenderItems(items.data(), static_cast<u32>(items.size()));
    
    // 处理批次
    primal::math::m4x4 viewMatrix = primal::graphics::rhi::math::MatrixIdentity();
    primal::math::m4x4 projMatrix = primal::graphics::rhi::math::MatrixIdentity();
    
    batchRenderer.ProcessBatches(viewMatrix, projMatrix);
    
    // 验证批次分割
    const BatchStats& stats = batchRenderer.GetStats();
    TEST_ASSERT(stats.totalBatches > 1, "超过最大批次大小的批次应该被分割");
    TEST_ASSERT(stats.mergedBatches >= 0, "合并批次数应该非负");
    
    batchRenderer.Shutdown();
    device.Shutdown();
    
    return TestResult::Passed;
}

/**
 * @brief 测试实例化渲染功能
 */
TestResult TestInstancedRendering() {
    MockRHIDevice device{};
    device.Initialize();
    
    BatchConfig config;
    config.enableInstancing = true;
    config.maxInstancesPerBatch = 10;
    
    RHIBatchRenderer batchRenderer(device, config);
    batchRenderer.Initialize();
    
    // 创建实例化渲染项
    std::vector<RenderItem> items;
    for (u32 i = 0; i < 5; ++i) {
        RenderItem item = CreateTestRenderItem(1, true); // 实例化
        item.instanceCount = 2 + i;
        items.push_back(item);
    }
    
    batchRenderer.AddRenderItems(items.data(), static_cast<u32>(items.size()));
    
    // 处理批次
    primal::math::m4x4 viewMatrix = primal::graphics::rhi::math::MatrixIdentity();
    primal::math::m4x4 projMatrix = primal::graphics::rhi::math::MatrixIdentity();
    
    batchRenderer.ProcessBatches(viewMatrix, projMatrix);
    
    // 验证实例化批次
    const BatchStats& stats = batchRenderer.GetStats();
    TEST_ASSERT(stats.instancedBatches > 0, "应该生成实例化批次");
    
    const auto& batches = batchRenderer.GetBatches();
    for (const auto& batch : batches) {
        if (batch.isInstancedBatch) {
            TEST_ASSERT(batch.totalInstances > 0, "实例化批次应该有实例数据");
        }
    }
    
    batchRenderer.Shutdown();
    device.Shutdown();
    
    return TestResult::Passed;
}

/**
 * @brief 测试视锥剔除功能
 */
TestResult TestFrustumCulling() {
    MockRHIDevice device{};
    device.Initialize();
    
    BatchConfig config;
    config.enableFrustumCulling = true;
    
    RHIBatchRenderer batchRenderer(device, config);
    batchRenderer.Initialize();
    
    // 创建渲染项（一些在视锥内，一些在视锥外）
    std::vector<RenderItem> items;
    for (u32 i = 0; i < 10; ++i) {
        RenderItem item = CreateTestRenderItem(i);
        // 将一半的物体放在很远的地方（会被剔除）
        if (i >= 5) {
            item.worldMatrix = primal::graphics::rhi::math::MatrixTranslation(
                simd::float3{10000.0f, 0.0f, 0.0f});
        }
        items.push_back(item);
    }
    
    batchRenderer.AddRenderItems(items.data(), static_cast<u32>(items.size()));
    
    // 设置视锥（默认在原点附近）
    primal::math::m4x4 viewMatrix = primal::graphics::rhi::math::MatrixIdentity();
    primal::math::m4x4 projMatrix = primal::graphics::rhi::math::MatrixPerspective(
        45.0f, 1.0f, 0.1f, 100.0f);
    
    batchRenderer.ProcessBatches(viewMatrix, projMatrix);
    
    // 验证剔除效果
    const BatchStats& stats = batchRenderer.GetStats();
    TEST_ASSERT(stats.culledItems > 0, "应该有渲染项被剔除");
    
    batchRenderer.Shutdown();
    device.Shutdown();
    
    return TestResult::Passed;
}

/**
 * @brief 测试深度排序功能
 */
TestResult TestDepthSorting() {
    MockRHIDevice device{};
    device.Initialize();
    
    BatchConfig config;
    config.enableDepthSorting = true;
    
    RHIBatchRenderer batchRenderer(device, config);
    batchRenderer.Initialize();
    
    // 创建不同深度的渲染项
    std::vector<RenderItem> items;
    for (u32 i = 0; i < 10; ++i) {
        RenderItem item = CreateTestRenderItem(1); // 相同类型
        item.depth = 100.0f - static_cast<f32>(i * 10.0f); // 逆序深度
        items.push_back(item);
    }
    
    batchRenderer.AddRenderItems(items.data(), static_cast<u32>(items.size()));
    
    // 处理批次
    primal::math::m4x4 viewMatrix = primal::graphics::rhi::math::MatrixIdentity();
    primal::math::m4x4 projMatrix = primal::graphics::rhi::math::MatrixIdentity();
    
    batchRenderer.ProcessBatches(viewMatrix, projMatrix);
    
    // 验证深度排序
    const auto& batches = batchRenderer.GetBatches();
    for (const auto& batch : batches) {
        if (batch.items.size() <= 1) continue;
        
        // 检查是否按深度排序（从前到后）
        for (size_t i = 1; i < batch.items.size(); ++i) {
            TEST_ASSERT(batch.items[i-1].depth <= batch.items[i].depth, 
                       "渲染项应该按深度排序");
        }
    }
    
    batchRenderer.Shutdown();
    device.Shutdown();
    
    return TestResult::Passed;
}

/**
 * @brief 测试命令提交功能
 */
TestResult TestCommandSubmission() {
    MockRHIDevice device{};
    device.Initialize();
    
    BatchConfig config;
    RHIBatchRenderer batchRenderer(device, config);
    batchRenderer.Initialize();
    
    // 创建测试渲染项
    std::vector<RenderItem> items;
    for (u32 i = 0; i < 5; ++i) {
        items.push_back(CreateTestRenderItem(i));
    }
    
    batchRenderer.AddRenderItems(items.data(), static_cast<u32>(items.size()));
    
    // 处理批次
    primal::math::m4x4 viewMatrix = primal::graphics::rhi::math::MatrixIdentity();
    primal::math::m4x4 projMatrix = primal::graphics::rhi::math::MatrixIdentity();
    
    u32 batchCount = batchRenderer.ProcessBatches(viewMatrix, projMatrix);
    
    // 模拟命令提交
    MockRHICommandBuffer commandBuffer(device);
    u32 submittedCount = batchRenderer.SubmitBatches(&commandBuffer);
    
    // 验证命令生成
    TEST_ASSERT(submittedCount == batchCount, "提交的批次数量应该匹配");
    TEST_ASSERT(!commandBuffer.drawCalls.empty(), "应该生成绘制调用");
    
    // 验证绘制调用的正确性
    for (const auto& drawCall : commandBuffer.drawCalls) {
        TEST_ASSERT(drawCall.pipeline != handles::INVALID_PIPELINE, 
                   "绘制调用应该有有效的管线");
        TEST_ASSERT(drawCall.vertexCount > 0 || drawCall.indexCount > 0,
                   "绘制调用应该有顶点或索引数据");
    }
    
    batchRenderer.Shutdown();
    device.Shutdown();
    
    return TestResult::Passed;
}

/**
 * @brief 测试性能和统计功能
 */
TestResult TestPerformanceAndStatistics() {
    MockRHIDevice device{};
    device.Initialize();
    
    BatchConfig config;
    RHIBatchRenderer batchRenderer(device, config);
    batchRenderer.Initialize();
    
    // 创建大量渲染项用于性能测试
    std::vector<RenderItem> items;
    for (u32 i = 0; i < 1000; ++i) {
        items.push_back(CreateTestRenderItem(i % 20)); // 20种不同类型
    }
    
    auto startTime = std::chrono::high_resolution_clock::now();
    
    batchRenderer.AddRenderItems(items.data(), static_cast<u32>(items.size()));
    
    // 处理批次
    primal::math::m4x4 viewMatrix = primal::graphics::rhi::math::MatrixIdentity();
    primal::math::m4x4 projMatrix = primal::graphics::rhi::math::MatrixIdentity();
    
    u32 batchCount = batchRenderer.ProcessBatches(viewMatrix, projMatrix);
    (void)batchCount;
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(endTime - startTime);
    
    // 验证性能
    TEST_ASSERT(duration.count() < 100, "处理1000个渲染项应该在100ms内完成");
    
    // 验证统计信息
    const BatchStats& stats = batchRenderer.GetStats();
    TEST_ASSERT(stats.totalRenderItems == 1000, "应该处理1000个渲染项");
    TEST_ASSERT(stats.totalBatches > 0, "应该生成批次");
    TEST_ASSERT(stats.batchingEfficiency > 0.0f, "批处理效率应该大于0");
    TEST_ASSERT(stats.totalProcessingTime > 0.0, "处理时间应该大于0");
    TEST_ASSERT(stats.memoryUsage > 0, "内存使用量应该大于0");
    
    // 打印统计信息
    printf("性能测试结果:\n");
    printf("  处理时间: %llu ms\n", static_cast<unsigned long long>(duration.count()));
    printf("  批处理时间: %.3f ms\n", stats.totalProcessingTime);
    printf("  批次效率: %.2f%%\n", stats.batchingEfficiency * 100.0f);
    printf("  内存使用: %llu bytes\n", static_cast<unsigned long long>(stats.memoryUsage));
    
    batchRenderer.Shutdown();
    device.Shutdown();
    
    return TestResult::Passed;
}

/**
 * @brief 测试哈希功能
 * @details 验证新的 MurmurHash3 实现与原有 std::hash 功能的兼容性
 */
TestResult TestHashFunctionality() {
    std::cout << "测试哈希函数功能..." << std::endl;
    
    // 创建测试用的渲染项键
    std::vector<RenderItemKey> testKeys;
    
    // 创建不同的测试键
    for (u32 i = 0; i < 100; ++i) {
        RenderItemKey key;
        key.pipeline = static_cast<PipelineHandle>(i % 10);
        key.vertexBuffer = static_cast<ResourceHandle>((i / 10) % 5);
        key.indexBuffer = static_cast<ResourceHandle>((i / 50) % 3);
        key.material = static_cast<ResourceHandle>(i % 7);
        key.materialSlot = i % 4;
        key.vertexStride = 32 + (i % 3) * 16; // 32, 48, 64
        key.indexFormat = static_cast<DataFormat>(i % 3); // 假设有3种格式
        key.renderTarget = static_cast<u8>(i % 2);
        
        testKeys.push_back(key);
    }
    
    // 测试哈希函数的一致性
    RenderItemKeyHash hasher;
    std::unordered_map<RenderItemKey, size_t, RenderItemKeyHash> hashMap;
    
    // 验证相同键的哈希值一致性
    for (const auto& key : testKeys) {
        size_t hash1 = hasher(key);
        size_t hash2 = hasher(key);
        TEST_ASSERT(hash1 == hash2, "相同键的哈希值应该一致");
        
        // 验证哈希值非零（排除无效哈希）
        TEST_ASSERT(hash1 != 0, "哈希值不应该为零");
    }
    
    // 测试哈希分布（简单验证）
    std::vector<size_t> hashValues;
    for (const auto& key : testKeys) {
        hashValues.push_back(hasher(key));
    }
    
    // 检查是否有足够的哈希多样性
    std::unordered_set<size_t> uniqueHashes(hashValues.begin(), hashValues.end());
    float diversity = static_cast<float>(uniqueHashes.size()) / hashValues.size();
    TEST_ASSERT(diversity > 0.8f, "哈希值应该有足够的多样性，当前多样性: " << diversity);
    
    // 测试 unordered_map 的使用
    hashMap.clear();
    for (size_t i = 0; i < testKeys.size(); ++i) {
        hashMap[testKeys[i]] = i;
    }
    
    // 验证映射的正确性
    for (size_t i = 0; i < testKeys.size(); ++i) {
        auto it = hashMap.find(testKeys[i]);
        TEST_ASSERT(it != hashMap.end(), "应该能在哈希表中找到键");
        TEST_ASSERT(it->second == i, "映射的值应该正确");
    }
    
    // 测试不同键的哈希值差异
    for (size_t i = 1; i < testKeys.size(); ++i) {
        if (testKeys[i] == testKeys[i-1]) {
            // 相同键应该有相同哈希
            TEST_ASSERT(hasher(testKeys[i]) == hasher(testKeys[i-1]), 
                       "相同键应该有相同哈希值");
        }
    }
    
    // 性能基准测试
    const int PERFORMANCE_TEST_COUNT = 10000;
    auto startTime = std::chrono::high_resolution_clock::now();
    
    for (int i = 0; i < PERFORMANCE_TEST_COUNT; ++i) {
        hasher(testKeys[i % testKeys.size()]);
    }
    
    auto endTime = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(endTime - startTime);
    
    float avgTimePerHash = static_cast<float>(duration.count()) / PERFORMANCE_TEST_COUNT;
    TEST_ASSERT(avgTimePerHash < 1.0f, "哈希计算平均时间应该小于1微秒，当前: " << avgTimePerHash << "微秒");
    
    std::cout << "哈希功能测试通过！" << std::endl;
    std::cout << "  哈希多样性: " << (diversity * 100.0f) << "%" << std::endl;
    std::cout << "  平均哈希时间: " << avgTimePerHash << " 微秒" << std::endl;
    
    return TestResult::Passed;
}

/**
 * @brief 测试边界情况和错误处理
 */
TestResult TestEdgeCasesAndErrorHandling() {
    MockRHIDevice device{};
    device.Initialize();
    
    BatchConfig config;
    RHIBatchRenderer batchRenderer(device, config);
    
    // 测试未初始化的操作
    batchRenderer.AddRenderItem(CreateTestRenderItem(1));
    
    primal::math::m4x4 viewMatrix = primal::graphics::rhi::math::MatrixIdentity();
    primal::math::m4x4 projMatrix = primal::graphics::rhi::math::MatrixIdentity();
    
    u32 batchCount = batchRenderer.ProcessBatches(viewMatrix, projMatrix);
    TEST_ASSERT(batchCount == 0, "未初始化时应该返回0个批次");
    
    // 初始化后测试空数据
    batchRenderer.Initialize();
    
    batchCount = batchRenderer.ProcessBatches(viewMatrix, projMatrix);
    TEST_ASSERT(batchCount == 0, "空渲染项列表应该返回0个批次");
    
    // 测试空指针和零数量
    batchRenderer.AddRenderItems(nullptr, 0);
    batchCount = batchRenderer.ProcessBatches(viewMatrix, projMatrix);
    TEST_ASSERT(batchCount == 0, "空指针应该被安全处理");
    
    // 测试配置边界值
    BatchConfig extremeConfig;
    extremeConfig.maxBatchSize = 1;
    extremeConfig.maxVerticesPerBatch = 1;
    extremeConfig.maxIndicesPerBatch = 1;
    extremeConfig.maxInstancesPerBatch = 1;
    
    RHIBatchRenderer extremeBatchRenderer(device, extremeConfig);
    extremeBatchRenderer.Initialize();
    
    // 添加一些渲染项
    for (u32 i = 0; i < 3; ++i) {
        extremeBatchRenderer.AddRenderItem(CreateTestRenderItem(i));
    }
    
    batchCount = extremeBatchRenderer.ProcessBatches(viewMatrix, projMatrix);
    TEST_ASSERT(batchCount >= 3, "极端配置下应该生成足够多的批次");
    
    batchRenderer.Shutdown();
    extremeBatchRenderer.Shutdown();
    device.Shutdown();
    
    return TestResult::Passed;
}

// === 主测试函数 ===

int main() {
    // 初始化测试输出
    std::ofstream testOutput("rhi_batch_renderer_test_results.txt");
    if (!testOutput.is_open()) {
        std::cerr << "无法创建测试输出文件" << std::endl;
        return 1;
    }
    
    testOutput << "🚀 开始RHI智能批渲染优化器单元测试\n";
    testOutput << "=====================================\n\n";
    
    // 定义测试用例
    struct TestCase {
        std::string name;
        std::function<TestResult()> testFunc;
    };
    
    std::vector<TestCase> testCases = {
        {"测试批渲染器基本功能", TestBatchRendererBasicFunctionality},
        {"测试渲染项分类功能", TestRenderItemClassification},
        {"测试批处理优化功能", TestBatchOptimization},
        {"测试实例化渲染功能", TestInstancedRendering},
        {"测试视锥剔除功能", TestFrustumCulling},
        {"测试深度排序功能", TestDepthSorting},
        {"测试命令提交功能", TestCommandSubmission},
        {"测试性能和统计功能", TestPerformanceAndStatistics},
        {"测试哈希功能", TestHashFunctionality},
        {"测试边界情况和错误处理", TestEdgeCasesAndErrorHandling}
    };
    
    // 运行所有测试
    u32 passedTests = 0;
    u32 totalTests = static_cast<u32>(testCases.size());
    
    for (const auto& testCase : testCases) {
        std::cout << "=== " << testCase.name << " ===" << std::endl;
        testOutput << "=== " << testCase.name << " ===" << std::endl;
        
        TestResult result = testCase.testFunc();
        
        if (result == TestResult::Passed) {
            std::cout << "✅ 通过" << std::endl << std::endl;
            testOutput << "✅ 通过\n\n";
            passedTests++;
        } else {
            std::cout << "❌ 失败" << std::endl << std::endl;
            testOutput << "❌ 失败\n\n";
        }
    }
    
    // 输出测试总结
    std::cout << "=== 测试总结 ===" << std::endl;
    std::cout << "通过: " << passedTests << "/" << totalTests << std::endl;
    std::cout << "成功率: " << (passedTests * 100.0f / totalTests) << "%" << std::endl;
    
    if (passedTests == totalTests) {
        std::cout << "🎉 所有RHI智能批渲染优化器测试通过！" << std::endl;
        testOutput << "🎉 所有RHI智能批渲染优化器测试通过！\n";
    } else {
        std::cout << "⚠️  部分测试失败，请检查实现。" << std::endl;
        testOutput << "⚠️  部分测试失败，请检查实现。\n";
    }
    
    testOutput << "=====================================\n";
    testOutput.close();
    
    return (passedTests == totalTests) ? 0 : 1;
}