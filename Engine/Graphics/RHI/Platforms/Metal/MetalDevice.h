/**
 * @file MetalDevice.h
 * @brief Metal 设备实现
 * @details 遵循 CRTP 模式，实现 Metal 图形设备接口
 * @author GameEngine VulkanCPP Team
 * @date 2026-01-06
 * @version 0.1.0
 */

#pragma once

#include "MetalCommon.h"
#include "MetalBuffer.h"
#include "MetalTexture.h"
#include "MetalCommandBuffer.h"
#include "MetalSync.h"
#include "MetalQuery.h"
#include "MetalShader.h"
#include "MetalPipeline.h"
#include "MetalSampler.h"
#include "MetalDescriptorSet.h"
#include "MetalDescriptorSetLayout.h"
#include "MetalPipelineLayout.h"
#include "MetalRenderPass.h"
#include "MetalStagingAllocator.h"
#include "../../Core/RHIDevice.h"
#include "../../Core/RHIAllocator.h"
#include "../../Core/RHIAdaptiveMemoryPool.h"
#include <iostream>
#include <atomic>

namespace primal::graphics::rhi {

/**
 * @brief Metal 设备实现类
 * @details 继承自 RHIDevice<MetalDevice>，通过 CRTP 实现多态
 */
class MetalDevice : public RHIDevice<MetalDevice> {
    friend class RHIDevice<MetalDevice>;

public:
    /**
     * @brief 构造函数
     * @param desc 设备描述符
     */
    explicit MetalDevice(const DeviceDesc& desc);

    /**
     * @brief 析构函数
     */
    ~MetalDevice() override;

    /**
     * @brief 获取Metal设备对象
     */
    MTL::Device* GetNativeDevice() const { return mtlDevice_; }

    /**
     * @brief 获取图形队列
     */
    MTL::CommandQueue* GetGraphicsQueue() const { return graphicsQueue_; }

    /**
     * @brief 获取计算队列
     */
    MTL::CommandQueue* GetComputeQueue() const { return computeQueue_; }

    /**
     * @brief 获取传输队列
     */
    MTL::CommandQueue* GetTransferQueue() const { return transferQueue_; }

    /**
     * @brief 获取 staging allocator (per-frame ring pool)
     * @details Used by MetalBuffer/MetalTexture slow paths to avoid blocking
     *          waitUntilCompleted on Private-storage updates. Allocator must
     *          be initialized before any UpdateBufferData/UpdateTextureData call.
     */
    MetalStagingAllocator& GetStagingAllocator() { return stagingAllocator_; }

    /**
     * @brief 获取缓冲区对象 (内部使用)
     */
    MetalBuffer* GetBuffer(ResourceHandle handle);

    /**
     * @brief 获取纹理对象 (内部使用)
     */
    MetalTexture* GetTexture(ResourceHandle handle);

    /**
     * @brief 获取命令缓冲区对象 (内部使用)
     */
    MetalCommandBuffer* GetCommandBuffer(CommandBufferHandle handle);

    /**
     * @brief 获取同步对象 (内部使用)
     */
    MetalSync* GetSync(SyncHandle handle);

    /**
     * @brief 获取查询池对象 (内部使用)
     */
    MetalQueryPool* GetQueryPool(QueryPoolHandle handle);

    /**
     * @brief 获取着色器对象 (内部使用)
     */
    MetalShader* GetShader(ShaderHandle handle);

    /**
     * @brief 获取管线对象 (内部使用)
     */
    MetalPipeline* GetPipeline(PipelineHandle handle);

    /**
     * @brief 注册管线对 Shader 的依赖
     */
    void RegisterPipelineDependency(PipelineHandle pipeline, ShaderHandle shader);

    /**
     * @brief 注销管线对 Shader 的依赖
     */
    void UnregisterPipelineDependency(PipelineHandle pipeline);

    /**
     * @brief 热重载 Shader
     * @details 更新 Shader 内容并重建所有依赖的 Pipeline
     */
    bool ReloadShader(ShaderHandle shader, const void* data, size_t size) override;

    /// Upload data into an existing buffer using this device's own handle→buffer
    /// allocator. Bypasses the global ResourceManager singleton, which isn't shared
    /// across dylib boundaries (each translation unit gets its own Meyers singleton
    /// instance, so the executable's "create" side and the dylib's "update" side
    /// disagree on the handle→resource map).
    bool UpdateBufferData(ResourceHandle handle, const void* data, u64 size, u64 offset = 0) override;

    /**
     * @brief 获取采样器对象 (内部使用)
     */
    MetalSampler* GetSampler(SamplerHandle handle);

    /**
     * @brief 获取描述符集布局对象 (内部使用)
     */
    MetalDescriptorSetLayout* GetDescriptorSetLayout(DescriptorSetLayoutHandle handle);

    /**
     * @brief 获取描述符集对象 (内部使用)
     */
    MetalDescriptorSet* GetDescriptorSet(DescriptorSetHandle handle);

    /**
     * @brief 获取渲染通道对象 (内部使用)
     */
    MetalRenderPass* GetRenderPass(RenderPassHandle handle);

protected:
    // === CRTP 实现接口 ===

    /**
     * @brief 初始化设备实现
     * @return 初始化是否成功
     */
    bool initializeImpl();

    /**
     * @brief 销毁设备实现
     */
    void shutdownImpl();

    /**
     * @brief 等待设备空闲实现
     */
    void waitIdleImpl() const;

    /**
     * @brief 开始新帧实现
     */
    void beginFrameImpl();

    /**
     * @brief 结束当前帧实现
     */
    void endFrameImpl();

    /**
     * @brief 呈现实现
     */
    void presentImpl();

    /**
     * @brief 查询设备信息实现
     * @param info 设备信息结构体引用
     */
    void queryDeviceInfo(DeviceInfo& info);

    /**
     * @brief 获取当前帧索引实现
     * @return 当前帧索引
     */
    u32 getCurrentFrameIndexImpl() const;
    
    /**
     * @brief 提交命令缓冲区实现
     */
    bool submitImpl(const QueueSubmitInfo& info);
    
    /**
     * @brief 创建同步对象实现
     */
    SyncHandle createSyncImpl();
    
    /**
     * @brief 等待同步对象实现
     */
    bool waitForSyncImpl(SyncHandle handle, u32 timeoutMs);

    /**
     * @brief 销毁同步对象实现
     */
    void destroySyncImpl(SyncHandle handle);

    /**
     * @brief 创建查询池实现
     */
    QueryPoolHandle createQueryPoolImpl(const QueryPoolDesc& desc);

    /**
     * @brief 销毁查询池实现
     */
    void destroyQueryPoolImpl(QueryPoolHandle handle);

    bool getQueryPoolResultsImpl(QueryPoolHandle handle, u32 firstQuery, u32 queryCount, void* data, size_t stride);

    /**
     * @brief 获取时间戳周期实现
     * @return 时间戳周期（纳秒）
     */
    double getTimestampPeriodImpl() const { return 1.0; }

    /**
     * @brief 创建交换链实现
     */
    RHISwapChain* createSwapChainImpl(const SwapChainDesc& desc);

    /**
     * @brief 销毁交换链实现
     */
    void destroySwapChainImpl(RHISwapChain* swapChain);

    // === 资源创建接口实现 ===
    
    /**
     * @brief 创建缓冲区实现
     */
    ResourceHandle createBufferImpl(const BufferDesc& desc);
    ResourceHandle createTextureImpl(const TextureDesc& desc);
    ResourceHandle createTextureViewImpl(const TextureViewDesc& desc);
    ShaderHandle createShaderImpl(const void* data, size_t size, ShaderStage stage, const char* entryPoint);

    /**
     * @brief 创建图形管线实现
     */
    PipelineHandle createGraphicsPipelineImpl(const GraphicsPipelineDesc& desc);

    /**
     * @brief 创建计算管线实现
     */
    PipelineHandle createComputePipelineImpl(const ComputePipelineDesc& desc);
    CommandBufferHandle createCommandBufferImpl(CommandQueueType type);
    SamplerHandle createSamplerImpl(const SamplerDesc& desc);
    DescriptorSetLayoutHandle createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc& desc);
    PipelineLayoutHandle createPipelineLayoutImpl(const PipelineLayoutDesc& desc);
    DescriptorSetHandle createDescriptorSetImpl(const DescriptorSetDesc& desc);
    void updateDescriptorSetsImpl(u32 writeCount, const WriteDescriptorSet* writes);

    /**
     * @brief 创建渲染通道实现
     */
    RenderPassHandle createRenderPassImpl(const RenderPassDesc& desc);

    // === 资源销毁接口实现 ===

    void destroyBufferImpl(ResourceHandle handle);
    
    // 内存管理辅助
    void* mapBufferImpl(ResourceHandle handle, u64 offset, u64 size);
    void unmapBufferImpl(ResourceHandle handle);
    void destroyTextureImpl(ResourceHandle handle);

    /**
     * @brief 销毁着色器实现
     */
    void destroyShaderImpl(ShaderHandle handle);

    /**
     * @brief 销毁管线实现
     */
    void destroyPipelineImpl(PipelineHandle handle);
    void destroyCommandBufferImpl(CommandBufferHandle handle);
    void destroySamplerImpl(SamplerHandle handle);
    void destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle handle);
    void destroyPipelineLayoutImpl(PipelineLayoutHandle handle);
    void destroyDescriptorSetImpl(DescriptorSetHandle handle);

    /**
     * @brief 销毁渲染通道实现
     */
    void destroyRenderPassImpl(RenderPassHandle handle);

private:
    MTL::Device* mtlDevice_{nullptr};           ///< Metal 设备对象
    MTL::CommandQueue* graphicsQueue_{nullptr}; ///< 图形命令队列
    MTL::CommandQueue* computeQueue_{nullptr};  ///< 计算命令队列
    MTL::CommandQueue* transferQueue_{nullptr}; ///< 传输命令队列
    
    // Shader 依赖图: ShaderHandle -> [PipelineHandle]
    std::mutex pipelineDependencyMutex_;
    std::unordered_map<ShaderHandle, utl::vector<PipelineHandle>> shaderToPipelines_;

    // === 显存管理 ===
    class RHIAdaptiveMemoryPool* memoryPool_{nullptr}; ///< 自适应内存池 (Shared)
    MTL::Heap* heap_{nullptr};                         ///< Metal堆 (Shared)

    // Per-frame staging allocator (eliminates waitUntilCompleted from slow paths)
    MetalStagingAllocator stagingAllocator_;
    
    std::atomic<u32> currentFrameIndex_{0};             ///< 当前帧索引
    
    RHIAllocator<MetalBuffer> bufferAllocator_; ///< 缓冲区分配器
    RHIAllocator<MetalTexture> textureAllocator_; ///< 纹理分配器
    RHIAllocator<MetalCommandBuffer> commandBufferAllocator_; ///< 命令缓冲区分配器
    RHIAllocator<MetalSync> syncAllocator_; ///< 同步对象分配器
    RHIAllocator<MetalQueryPool> queryPoolAllocator_; ///< 查询池分配器
    RHIAllocator<MetalShader> shaderAllocator_; ///< 着色器分配器
    RHIAllocator<MetalPipeline> pipelineAllocator_; ///< 管线分配器
    RHIAllocator<MetalSampler> samplerAllocator_; ///< 采样器分配器
    RHIAllocator<MetalDescriptorSetLayout> descriptorSetLayoutAllocator_; ///< 描述符集布局分配器
    RHIAllocator<MetalPipelineLayout> pipelineLayoutAllocator_; ///< 管线布局分配器
    RHIAllocator<MetalDescriptorSet> descriptorSetAllocator_; ///< 描述符集分配器
    RHIAllocator<MetalRenderPass> renderPassAllocator_; ///< 渲染通道分配器
    
    void initializeMemoryPool();
    void shutdownMemoryPool();
};

} // namespace primal::graphics::rhi
