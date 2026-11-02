#pragma once

#include "RenderGraphDefinitions.h"
#include "RenderGraphResource.h"
#include "RenderGraphPass.h"
#include "RenderGraphBuilder.h"
#include <memory>
#include <memory>
#include <unordered_map>

namespace primal::graphics::rendergraph {

/**
 * @brief 渲染图管理器
 * @details 负责管理 Pass、资源以及图的编译和执行
 */
class RenderGraph {
    friend class RenderGraphBuilder;
public:
    RenderGraph(rhi::RHIDeviceBase& device);
    ~RenderGraph();

    // 清空图 (每帧调用)
    void Clear();

    /**
     * @brief 添加一个 Pass
     */
    template<typename Data, typename SetupFn, typename ExecuteFn>
    const Data& AddPass(const std::string& name, RGPassType type, RGPassCategory category, SetupFn setup, ExecuteFn execute) {
        auto pass = std::make_unique<RenderGraphPassImpl<Data>>(name, type, category, setup, execute);
        auto* passPtr = pass.get();
        passes_.push_back(std::move(pass));

        RenderGraphBuilder builder(*this, passPtr);
        passPtr->Setup(builder);

        return passPtr->GetData();
    }

    /**
     * @brief 添加一个 Pass (Legacy)
     */
    template<typename Data, typename SetupFn, typename ExecuteFn>
    const Data& AddPass(const std::string& name, RGPassType type, SetupFn setup, ExecuteFn execute) {
        return AddPass<Data>(name, type, RGPassCategory::None, setup, execute);
    }

    /**
     * @brief 导入外部资源 (已废弃，请使用 ImportTexture 或 ImportBuffer)
     * @param initialState 资源在导入时的真实 GPU 状态。Unknown 表示调用方未声明,
     *                     RG 无法生成正确的 from-initial-state barrier。
     *                     对于已知状态(如刚被作为 RenderTarget 写入)的导入资源,
     *                     应显式传入以保障跨 encoder 同步。
     */
    RGResourceHandle ImportResource(const std::string& name, rhi::ResourceHandle resource,
                                    rhi::ResourceState initialState = rhi::ResourceState::Unknown);

    /**
     * @brief 导入外部纹理
     * @param initialState 纹理在导入时的真实 GPU 状态(如 RenderTarget / UnorderedAccess / ShaderResource)
     */
    RGResourceHandle ImportTexture(const std::string& name, rhi::ResourceHandle resource,
                                    const rhi::TextureDesc& desc,
                                    rhi::ResourceState initialState = rhi::ResourceState::Unknown);

    /**
     * @brief 导入外部缓冲区
     * @param initialState 缓冲区在导入时的真实 GPU 状态
     */
    RGResourceHandle ImportBuffer(const std::string& name, rhi::ResourceHandle resource,
                                   const rhi::BufferDesc& desc,
                                   rhi::ResourceState initialState = rhi::ResourceState::Unknown);

    /**
     * @brief 获取输出资源 (标记为 Output，防止被剔除)
     */
    void MarkAsOutput(RGResourceHandle handle);

    /**
     * @brief 编译图
     * @details 执行剔除、依赖分析、资源分配等
     */
    void Compile();

    /**
     * @brief 执行图
     */
    void Execute(rhi::RHICommandBuffer* cmdBuffer);

    // 内部接口供 Builder 使用
    rhi::RHIDeviceBase& GetDevice() { return device_; }
    RGResourceHandle CreateTexture(const std::string& name, const rhi::TextureDesc& desc);
    RGResourceHandle CreateBuffer(const std::string& name, const rhi::BufferDesc& desc);

    // Debug 接口
    const utl::vector<RenderGraphPass*>& GetPasses() const { return activePasses_; }
    const utl::vector<std::unique_ptr<RenderGraphResource>>& GetResources() const { return resources_; }
    std::string DumpGraphViz() const;

    RenderGraphResource* GetResource(RGResourceHandle handle);

    // 获取上一帧各Pass的GPU耗时 (ms)
    const std::unordered_map<std::string, double>& GetPassExecutionTimes() const { return passExecutionTimes_; }
    bool AreTimestampsEnabled() const { return timestampsEnabled_; }
    u64 GetTimestampRecordingSequence() const { return timestampRecordingSequence_; }
    u64 GetTimestampResolvedSequence() const { return timestampResolvedSequence_; }
    bool HasFreshTimestampResults() const { return timestampResultsFresh_; }
    bool WasTimestampQueryReadSuccessful() const { return timestampQueryReadOk_; }
    double GetTimestampQueryReadCpuMs() const { return timestampQueryReadCpuMs_; }
    u64 GetTimestampReadMin() const { return timestampReadMin_; }
    u64 GetTimestampReadMax() const { return timestampReadMax_; }
    u64 GetTimestampPreviousBound() const { return timestampPreviousBound_; }
    size_t GetPoolSize() const { return resourcePool_.size(); }

private:
    void CleanupPool();
    void RegisterResourceRead(RenderGraphPass* pass, RGResourceHandle handle, rhi::ResourceState state);
    void RegisterResourceWrite(RenderGraphPass* pass, RGResourceHandle handle, rhi::ResourceState state);

private:
    struct PooledResource {
        rhi::ResourceHandle handle;
        rhi::TextureDesc texDesc;
        rhi::BufferDesc bufDesc;
        u64 lastUsedFrame = 0;
        bool isTexture = false;
    };

    // 编译阶段子步骤
    void CullPasses();
    void CalculateResourceLifetimes();
    void AllocateResources();
    void InsertBarriers();

    rhi::RHIDeviceBase& device_;
    utl::vector<std::unique_ptr<RenderGraphPass>> passes_;
    utl::vector<std::unique_ptr<RenderGraphResource>> resources_;
    
    // 资源查找表 (Name -> Handle) - 仅用于调试或查找
    std::unordered_map<std::string, RGResourceHandle> resourceMap_;
    // T1.1 (C2): single physical import identity — one rhi resource may map to
    // at most ONE RG identity per frame. Duplicate imports create alias
    // blind spots (RAW deps across identities are invisible to the
    // scheduler). Enforcement: alias imports are now MERGED onto the first
    // identity (P0-B), so all declarations share one resource state; enable
    // a per-frame trace with PRIMAL_RG_STRICT_IMPORT=1.
    std::unordered_map<u32, RGResourceHandle> importedPhysical_;
    // Returns the existing identity for this physical resource this frame,
    // or an invalid handle when this is the first import.
    RGResourceHandle ResolveImportIdentity(const std::string& name, rhi::ResourceHandle resource);

    // 执行顺序 (经过拓扑排序和剔除后)
    utl::vector<RenderGraphPass*> activePasses_;

    // 资源池
    utl::vector<PooledResource> resourcePool_;
    u64 currentFrame_ = 0;

    // Immutable for this graph's lifetime. Exact PRIMAL_RG_TIMESTAMPS=0 only
    // disables diagnostics, independently of all production synchronization.
    bool timestampsEnabled_=true;
    // GPU时间戳查询
    struct FrameQueryData {
        rhi::QueryPoolHandle queryPool = rhi::handles::INVALID_QUERY_POOL;
        u32 capacity = 0;
        utl::vector<std::string> passNames; // Index i corresponds to queries 2*i and 2*i+1
        u64 recordingSequence = 0;
        bool ready = false;
    };
    FrameQueryData queryFrames_[2]; // Ping-pong
    u32 currentQueryFrameIndex_ = 0;
    
    std::unordered_map<std::string, double> passExecutionTimes_;
    u64 timestampRecordingSequence_ = 0;
    u64 timestampResolvedSequence_ = 0;
    bool timestampResultsFresh_ = false;
    bool timestampQueryReadOk_ = false;
    double timestampQueryReadCpuMs_ = -1.0;
    u64 timestampLastTickBound_ = 0, timestampReadMin_ = 0, timestampReadMax_ = 0, timestampPreviousBound_ = 0;

    void ResolveTimestamps();
};

} // namespace primal::graphics::rendergraph
