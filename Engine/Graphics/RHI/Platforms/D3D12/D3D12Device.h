// Windows-only: requires ENABLE_D3D12 (_WIN32)
//
// D3D12Device.h — D3D12 设备（M0 F0-8 最小闭环 + M1 全资源层接线）。
//
// M0 闭环（保留）：debug layer + GBV + InfoQueue fail-fast / adapter + WARP /
// DIRECT queue / D3D12MA allocator / WaitIdle / 设备丢失上报。
//
// M1 新增（阶段文档 F1-1..F1-7）：
//   - D3D12DescriptorHeapManager（F1-5：分帧槽描述符 + 持久槽位）
//   - D3D12UploadRing（F1-2：帧槽 staging 环）
//   - D3D12StateTracker（F1-6：全局资源状态跟踪，资源注册/转换/衰减）
//   - Buffer/Texture/SwapChain/Pipeline/Shader/Sampler/布局/Set/命令缓冲/
//     Sync 全类型 RHIAllocator + createXxxImpl 实装
//   - Submit（ExecuteCommandLists + fence Signal）/ BeginFrame（槽推进：
//     staging 环 + 描述符槽轮转）/ EndFrame（GC 推进 kGcOffset=3 对齐基类）
//   - UpdateBufferData / UpdateTextureRegion（ODR-bypass 虚接口：staging 环
//     帧内排队 / 帧外阻塞）
//   - 链接锚点：EnsureD3D12BackendLinksLinked()（M0 §5.1 教训——防注册式
//     桥被链接器裁剪）
//
// 帧循环契约（RenderSystem 公共流程，无 D3D12 适配代码）：
//   BeginFrame → [调用方 Wait(slot fence)] → 录制 → Submit(signalFence=slot
//   fence) → Present；D3D12 的 semaphore 参数全部"接受并忽略"（单 DIRECT
//   队列有序性，见 D3D12SwapChain.h / D3D12Sync.h）。
#pragma once
#if defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12

#include <wrl/client.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>

// F0-4: D3D12MemAlloc v3.0.1（third_party/D3D12MemAlloc，含路径由
// Engine/CMakeLists.txt 的 WIN32+ENABLE_D3D12 分支注入；实现 TU 为
// third_party/D3D12MemAlloc/src/D3D12MemAlloc.cpp，独立编译，无需
// D3D12MA_IMPLEMENTATION 宏）。
#include "D3D12MemAlloc.h"

#include "../../Core/RHIDevice.h"
#include "D3D12BackendLinks.h"
#include "D3D12Buffer.h"
#include "D3D12CommandBuffer.h"
#include "D3D12DescriptorHeaps.h"
#include "D3D12Fence.h"
#include "D3D12Pipeline.h"
#include "D3D12Shader.h"
#include "D3D12StateTracker.h"
#include "D3D12SwapChain.h"
#include "D3D12Sync.h"
#include "D3D12Texture.h"
#include "D3D12Upload.h"

namespace primal::graphics::rhi {

class D3D12Device final : public RHIDevice<D3D12Device> {
    friend class RHIDevice<D3D12Device>;
    friend class d3d12::D3D12Buffer;
    friend class d3d12::D3D12Texture;
    friend class d3d12::D3D12CommandBufferImpl;
    friend class d3d12::D3D12SwapChain;
public:
    explicit D3D12Device(const DeviceDesc& desc);
    ~D3D12Device() override;

    // --- 原生访问（后端内部与诊断；共享代码禁 include——F0-12 门禁）---
    ID3D12Device* GetNativeDevice() const { return device_.Get(); }
    ID3D12CommandQueue* GetGraphicsQueue() const { return graphicsQueue_.Get(); }
    IDXGIAdapter4* GetNativeAdapter() const { return adapter_.Get(); }
    bool IsWarp() const { return warp_; }
    bool GetDxgiFactory(IDXGIFactory2** outFactory) const {
        if (!dxgiFactory_ || !outFactory) return false;
        *outFactory = dxgiFactory_.Get();
        return true;
    }
    D3D12MA::Allocator* GetD3D12MAAllocator() const { return d3d12maAllocator_; }
    d3d12::D3D12FencePool& GetFencePool() { return fencePool_; }
    d3d12::D3D12DescriptorHeapManager& GetDescriptorHeaps() { return descriptorHeaps_; }
    d3d12::D3D12StateTracker& GetStateTracker() { return stateTracker_; }
    d3d12::D3D12UploadRing& GetUploadRing() { return uploadRing_; }

    // --- 帧录制状态（上传路径分流：帧内排队 / 帧外阻塞；对齐 VulkanDevice）---
    bool IsFrameRecording() const { return recordingCount_.load() > 0; }
    void IncrementRecording() { recordingCount_.fetch_add(1); }
    void DecrementRecording() {
        u32 v = recordingCount_.load();
        recordingCount_.store(v > 0 ? v - 1 : 0);
    }

    // --- Per-type lookup（friend 子类 + 测试宿主用）---
    d3d12::D3D12Texture* GetTexture(ResourceHandle handle);
    d3d12::D3D12Buffer* GetBuffer(ResourceHandle handle);
    d3d12::D3D12Shader* GetShader(ShaderHandle handle);
    d3d12::D3D12Pipeline* GetPipeline(PipelineHandle handle);
    d3d12::D3D12PipelineLayout* GetPipelineLayout(PipelineLayoutHandle handle);
    d3d12::D3D12DescriptorSetLayout* GetDescriptorSetLayout(DescriptorSetLayoutHandle handle);
    d3d12::D3D12DescriptorSet* GetDescriptorSet(DescriptorSetHandle handle);
    d3d12::D3D12Sampler* GetSampler(SamplerHandle handle);
    d3d12::D3D12CommandBufferImpl* GetCommandBuffer(CommandBufferHandle handle);
    d3d12::D3D12SyncObject* GetSync(SyncHandle handle);
    const RenderPassDesc* GetRenderPassDesc(RenderPassHandle handle) const;

    // F0-11 公共契约：诊断查询（M1 资源层接通）
    bool GetTextureSize(ResourceHandle texture, u32& width, u32& height) override;
    DataFormat GetTextureFormat(ResourceHandle texture) override;
    bool GetBufferByteSize(ResourceHandle buffer, u64& bytes) override;

    // --- M5 F5-2：indirect command signature（惰性创建，进程级每设备一份）---
    // 布局契约（阶段文档 §3）：以 Vulkan 现行 indirect buffer 布局为单一
    // 事实源——D3D12_DRAW_ARGUMENTS / D3D12_DRAW_INDEXED_ARGUMENTS 与
    // VkDrawIndirectCommand(4×u32) / VkDrawIndexedIndirectCommand(5×u32)
    // 字段序字节级一致，CommandSignature 按该字节布局构造，零中间翻译。
    ID3D12CommandSignature* GetDrawIndirectSignature();        // DRAW        16B
    ID3D12CommandSignature* GetDrawIndexedIndirectSignature(); // DRAW_INDEXED 20B
    // 查询签名字节步长（CommandBuffer ExecuteIndirect 直译用；供单测断言）。
    static constexpr u32 kIndirectDrawArgStride = 16;
    static constexpr u32 kIndirectDrawIndexedArgStride = 20;

    // M5 F5-3：descriptor indexing 能力运行时真实查询（覆盖 RHIDeviceBase
    // 默认 false）。语义=VulkanDevice 同款（SupportsDescriptorIndexing 为
    // 引擎消费路径的"可用"判定，非原始 tier 透传）：ResourceBindingTier>=2
    // （无界描述符数组+shader 动态索引可用）。不按 shader model 假设。
    bool SupportsDescriptorIndexing() const override { return descriptorIndexingSupported_; }
    // F5-3 记录用：原始 tier + DescriptorHeap 特性（MaxDescriptorHeapSize/
    // MaxSamplerDescriptorHeapSize，Tier1 的 SRV 堆 100 万上限是 bindless
    // 规模上限的决定项）。initializeImpl 时查询缓存。
    u32 GetResourceBindingTier() const { return resourceBindingTier_; }
    u64 GetMaxDescriptorHeapSize() const { return maxDescriptorHeapSize_; }
    // M6 F6-2/F6-3：类型化 UAV 附加格式（OPTIONS3.TypedUAVLoadAdditionalFormats）
    // ——RGBA16F/RG16F 等 typed UAV load/store 的先决能力（D3D12 仅保证
    // R32_FLOAT/UINT/SINT）。initializeImpl 期真实查询缓存；false 时
    // D3D12Texture 对需附加格式的 UAV 纹理显式失败（fail 语义）。
    bool GetTypedUavAdditionalFormats() const { return typedUavAdditionalFormats_; }

    // ODR-bypass 上传虚接口（语义与 VulkanDevice 同名 override 对齐）
    bool UpdateBufferData(ResourceHandle handle, const void* data, u64 size,
                          u64 offset = 0) override;
    bool UpdateTextureRegion(ResourceHandle texture, const void* data, u64 size,
                             const TextureRegionUpdate& region) override;

protected:
    // === CRTP 实现接口（全量实装，M1）===
    bool initializeImpl();
    void shutdownImpl();
    void waitIdleImpl() const;
    void beginFrameImpl();
    void endFrameImpl();
    void presentImpl();
    void queryDeviceInfo(DeviceInfo& info);
    u32 getCurrentFrameIndexImpl() const;
    double getTimestampPeriodImpl() const;

    RHISwapChain* createSwapChainImpl(const SwapChainDesc& desc);
    void destroySwapChainImpl(RHISwapChain* swapChain);

    ResourceHandle createBufferImpl(const BufferDesc& desc);
    void destroyBufferImpl(ResourceHandle handle);
    void* mapBufferImpl(ResourceHandle handle, u64 offset, u64 size);
    void unmapBufferImpl(ResourceHandle handle);
    void setBufferDirtySizeImpl(ResourceHandle handle, u64 size);

    ResourceHandle createTextureImpl(const TextureDesc& desc);
    void destroyTextureImpl(ResourceHandle handle);
    ResourceHandle createTextureViewImpl(const TextureViewDesc& desc);

    ShaderHandle createShaderImpl(const void* data, size_t size, ShaderStage stage,
                                  const char* entryPoint);
    void destroyShaderImpl(ShaderHandle handle);

    PipelineHandle createGraphicsPipelineImpl(const GraphicsPipelineDesc& desc);
    PipelineHandle createComputePipelineImpl(const ComputePipelineDesc& desc);
    void destroyPipelineImpl(PipelineHandle handle);

    RenderPassHandle createRenderPassImpl(const RenderPassDesc& desc);
    void destroyRenderPassImpl(RenderPassHandle handle);

    CommandBufferHandle createCommandBufferImpl(CommandQueueType type);
    void destroyCommandBufferImpl(CommandBufferHandle handle);
    bool submitImpl(const QueueSubmitInfo& info);

    SyncHandle createSyncImpl();
    bool waitForSyncImpl(SyncHandle handle, u32 timeoutMs);
    void destroySyncImpl(SyncHandle handle);

    QueryPoolHandle createQueryPoolImpl(const QueryPoolDesc& desc);
    void destroyQueryPoolImpl(QueryPoolHandle handle);
    bool getQueryPoolResultsImpl(QueryPoolHandle handle, u32 firstQuery,
                                 u32 queryCount, void* data, size_t stride);

    SamplerHandle createSamplerImpl(const SamplerDesc& desc);
    void destroySamplerImpl(SamplerHandle handle);

    DescriptorSetLayoutHandle createDescriptorSetLayoutImpl(const DescriptorSetLayoutDesc& desc);
    void destroyDescriptorSetLayoutImpl(DescriptorSetLayoutHandle handle);
    PipelineLayoutHandle createPipelineLayoutImpl(const PipelineLayoutDesc& desc);
    void destroyPipelineLayoutImpl(PipelineLayoutHandle handle);
    DescriptorSetHandle createDescriptorSetImpl(const DescriptorSetDesc& desc);
    void destroyDescriptorSetImpl(DescriptorSetHandle handle);
    void updateDescriptorSetsImpl(u32 writeCount, const WriteDescriptorSet* writes);

private:
    // M0：debug layer + GBV + InfoQueue / adapter / device+queue / allocator
    bool enableDebugLayer();
    bool selectAdapter();
    bool createDeviceAndQueue();
    bool createAllocator();
    void reportDeviceRemoval(HRESULT reason);

    // M1：M0 地基之上的资源层子系统初始化（fence pool 之后、闭环检查之前）。
    bool createResourceSubsystems();

    /// 执行命令列表 + 共享 fence Signal；返回目标值（0=失败）。
    /// submitImpl / 命令缓冲直连 Submit / upload FlushBlocking 共用。
    u64 ExecuteAndSignal(ID3D12GraphicsCommandList* list);

    /// 上传环路径（D3D12Buffer/D3D12Texture 的 update 落点）。
    bool UploadBufferDataViaRing(ID3D12Resource* dst, const void* data, u64 size,
                                 u64 dstOffset);
    bool UploadTextureDataViaRing(ID3D12Resource* texture, ResourceHandle textureId,
                                  DataFormat format, u32 mipLevels,
                                  const void* data, u64 size,
                                  const TextureRegionUpdate& region);

    /// 交换链 backbuffer 包装（wrap 纹理 + tracker 注册）。
    ResourceHandle WrapSwapChainBuffer(RHISwapChain& owner, const TextureDesc& desc,
                                       ID3D12Resource* buffer);
    void UnwrapSwapChainBuffer(ResourceHandle handle);

    /// set 区段迁移（长寿命 set 的跨帧复用护栏）：set 区段属主槽 != 当前
    /// 帧槽 → 当前槽重分配 + CopyDescriptorsSimple 迁移内容 + 更新属主。
    void EnsureSetRegionCurrent(d3d12::D3D12DescriptorSet* set);

    Microsoft::WRL::ComPtr<ID3D12Debug1> debugController_;
    Microsoft::WRL::ComPtr<ID3D12InfoQueue> infoQueue_;
    Microsoft::WRL::ComPtr<IDXGIFactory6> dxgiFactory_;
    Microsoft::WRL::ComPtr<IDXGIAdapter4> adapter_;
    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> graphicsQueue_;

    d3d12::D3D12FencePool fencePool_;
    // F0-4: D3D12MA 主分配器（原生指针 + Release() 销毁，D3D12MA 规范用法）。
    D3D12MA::Allocator* d3d12maAllocator_ = nullptr;

    // === M1 资源层子系统 ===
    d3d12::D3D12DescriptorHeapManager descriptorHeaps_;
    d3d12::D3D12UploadRing uploadRing_;
    d3d12::D3D12StateTracker stateTracker_;

    // === Per-type free_list 分配器（延迟释放与 Vulkan 同款护栏）===
    RHIAllocator<d3d12::D3D12Buffer> bufferAllocator_;
    RHIAllocator<d3d12::D3D12Texture> textureAllocator_;
    RHIAllocator<d3d12::D3D12Shader> shaderAllocator_;
    RHIAllocator<d3d12::D3D12Pipeline> pipelineAllocator_;
    RHIAllocator<d3d12::D3D12Sampler> samplerAllocator_;
    RHIAllocator<d3d12::D3D12DescriptorSetLayout> descriptorSetLayoutAllocator_;
    RHIAllocator<d3d12::D3D12PipelineLayout> pipelineLayoutAllocator_;
    RHIAllocator<d3d12::D3D12DescriptorSet> descriptorSetAllocator_;
    RHIAllocator<d3d12::D3D12CommandBufferImpl> commandBufferAllocator_;
    RHIAllocator<d3d12::D3D12SyncObject> syncAllocator_;
    /// RenderPass 对象（Vulkan 式 renderpass 在 D3D12 无原生对应——存 desc
    /// 供 BeginRenderPass(handle) 重放；创建销毁全量登记）。
    utl::vector<std::pair<RenderPassHandle, RenderPassDesc>> renderPasses_;
    RenderPassHandle nextRenderPassHandle_ = 1;

    std::atomic<u32> recordingCount_{0};
    bool warp_ = false;
    u32 currentFrameIndex_ = 0;
    double timestampPeriodNs_ = 1.0;  // D3D12 timestamp unit 恒 1 tick

    // --- M5 F5-2/F5-3 ---
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> drawIndirectSignature_;
    Microsoft::WRL::ComPtr<ID3D12CommandSignature> drawIndexedIndirectSignature_;
    bool descriptorIndexingSupported_ = false;  // F5-3：tier>=2 判定结果
    u32 resourceBindingTier_ = 0;               // D3D12_RESOURCE_BINDING_TIER_*
    u64 maxDescriptorHeapSize_ = 0;             // OPTIONS.MaxDescriptorHeapSize
    bool typedUavAdditionalFormats_ = false;    // M6：OPTIONS3.TypedUAVLoadAdditionalFormats
};

} // namespace primal::graphics::rhi

#endif // defined(_WIN32) && defined(ENABLE_D3D12) && ENABLE_D3D12
