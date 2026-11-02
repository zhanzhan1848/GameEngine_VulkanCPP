#include "RenderGraph.h"
#include "Graphics/Utilities/CPUStallAudit.h"
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
#include "Graphics/RHI/Platforms/Vulkan/VulkanPassTimestampAudit.h"
#endif
#include "LumenCompareCapture.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stack>
#include <unordered_set>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics::rendergraph {

RenderGraph::RenderGraph(rhi::RHIDeviceBase& device) : device_(device) {
    const char* timestamps=std::getenv("PRIMAL_RG_TIMESTAMPS");
    timestampsEnabled_=!(timestamps && std::strcmp(timestamps,"0")==0);
    // Log each actual policy once per process, not once per rebuilt graph.
    static std::atomic<bool> logged[2]{};
    if(!logged[timestampsEnabled_?1:0].exchange(true,std::memory_order_relaxed)) {
        PRIMAL_LOG_STREAM(Info,"Render")<<"[RG_TIMESTAMPS] enabled="<<timestampsEnabled_
            <<" policy=construction-time diagnostic_only=1"<<std::endl;
    }
}

RenderGraph::~RenderGraph() {
    Clear();
    
    // Destroy all pooled resources
    for (const auto& res : resourcePool_) {
        if (res.isTexture) {
            device_.DestroyTexture(res.handle);
        } else {
            device_.DestroyBuffer(res.handle);
        }
    }
    resourcePool_.clear();

    // Cleanup Query Pools
    for (int i = 0; i < 2; ++i) {
        if (queryFrames_[i].queryPool != rhi::handles::INVALID_QUERY_POOL) {
            device_.DestroyQueryPool(queryFrames_[i].queryPool);
            queryFrames_[i].queryPool = rhi::handles::INVALID_QUERY_POOL;
        }
    }
}


void RenderGraph::Clear() {
    cpu_stall_audit::BeginGraph();
    cpu_stall_audit::Scope cpuClear("graph.clear");
    if(timestampsEnabled_) {
        ResolveTimestamps();
        currentQueryFrameIndex_ = 1 - currentQueryFrameIndex_;
    }

    passes_.clear();
    resources_.clear();
    resourceMap_.clear();
    activePasses_.clear();
    // importedPhysical_ is per-frame bookkeeping (see header): stale entries
    // from previous frames would flag every legitimate re-import of a
    // persistent texture (frame-slot rotation, shadow visibility, ...) as a
    // duplicate and hide real per-frame alias merges.
    importedPhysical_.clear();

    currentFrame_++;
    CleanupPool();
    cpu_stall_audit::BuildStart();
}

void RenderGraph::CleanupPool() {
    // Keep resources for some frames to reduce thrashing
    const u64 kKeepFrames = 3;

    // Dawn/WebGPU: DestroyTexture is immediate (no GPU fence defer).
    // Destroying pooled textures while GPU still references them corrupts
    // the DawnTexture free list. Since all passes run every frame with
    // fixed parameters, the pool stabilizes quickly — just keep everything.
    bool isDawn = (device_.GetPlatform() == rhi::RHIPlatform::Dawn);
    if (isDawn) return;

    // Destroy expired resources and compact the pool using move-assignment.
    // utl::vector::erase() uses memcpy internally, which is UB for
    // PooledResource (contains TextureDesc with std::string).
    size_t writeIdx = 0;
    for (size_t readIdx = 0; readIdx < resourcePool_.size(); ++readIdx) {
        if (currentFrame_ > resourcePool_[readIdx].lastUsedFrame + kKeepFrames) {
            if (resourcePool_[readIdx].isTexture) {
                device_.DestroyTexture(resourcePool_[readIdx].handle);
            } else {
                device_.DestroyBuffer(resourcePool_[readIdx].handle);
            }
        } else {
            if (writeIdx != readIdx) {
                resourcePool_[writeIdx] = std::move(resourcePool_[readIdx]);
            }
            ++writeIdx;
        }
    }
    // Shrink to actual size by erasing trailing elements from the end
    while (resourcePool_.size() > writeIdx) {
        resourcePool_.erase_unordered(resourcePool_.size() - 1);
    }
}

// T1.1 (C2): one physical resource = one RG import identity per frame.
// Duplicate imports under different names used to hide cross-identity RAW
// dependencies from the scheduler. Since P0-B the graph merges alias imports
// onto the existing identity instead of creating a second, blind one —
// Read/Write declarations from every alias land on the same resource state,
// so producer/consumer edges and barriers stay visible.
RGResourceHandle RenderGraph::ResolveImportIdentity(const std::string& name, rhi::ResourceHandle resource) {
    auto it = importedPhysical_.find(static_cast<u32>(resource));
    if (it == importedPhysical_.end()) {
        return kInvalidRGResourceHandle;
    }
    if (std::getenv("PRIMAL_RG_STRICT_IMPORT")) {
        static thread_local std::unordered_set<std::string> traced;
        std::string key = name + "#" + std::to_string(static_cast<u32>(resource));
        if (traced.insert(std::move(key)).second) {
            PRIMAL_LOG_STREAM(Info, "RG")
                << "[RG] Import alias '" << name << "' merged onto identity "
                << it->second.index << " for rhi resource " << static_cast<u32>(resource)
                << std::endl;
        }
    }
    return it->second;
}

RGResourceHandle RenderGraph::ImportResource(const std::string& name, rhi::ResourceHandle resource,
                                              rhi::ResourceState initialState) {
    RGResourceHandle existing = ResolveImportIdentity(name, resource);
    if (existing.IsValid()) {
        resourceMap_[name] = existing;
        return existing;
    }
    RGResourceHandle handle = {static_cast<u32>(resources_.size() + 1), 0};
    importedPhysical_.emplace(static_cast<u32>(resource), handle);
    auto rgResource = std::make_unique<RenderGraphResource>(name, handle, RGResourceType::Unknown);
    rgResource->SetImportedResource(resource);
    rgResource->SetInitialState(initialState);


    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RGResourceHandle RenderGraph::ImportTexture(const std::string& name, rhi::ResourceHandle resource,
                                             const rhi::TextureDesc& desc,
                                             rhi::ResourceState initialState) {
    RGResourceHandle existing = ResolveImportIdentity(name, resource);
    if (existing.IsValid()) {
        resourceMap_[name] = existing;
        return existing;
    }
    RGResourceHandle handle = {static_cast<u32>(resources_.size() + 1), 0};
    importedPhysical_.emplace(static_cast<u32>(resource), handle);
    auto rgResource = std::make_unique<RenderGraphTexture>(name, handle, desc);
    rgResource->SetImportedResource(resource);
    rgResource->AddFlag(RGResourceFlags::Imported);
    rgResource->SetInitialState(initialState);


    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RGResourceHandle RenderGraph::ImportBuffer(const std::string& name, rhi::ResourceHandle resource,
                                            const rhi::BufferDesc& desc,
                                            rhi::ResourceState initialState) {
    RGResourceHandle existing = ResolveImportIdentity(name, resource);
    if (existing.IsValid()) {
        resourceMap_[name] = existing;
        return existing;
    }
    RGResourceHandle handle = {static_cast<u32>(resources_.size() + 1), 0};
    importedPhysical_.emplace(static_cast<u32>(resource), handle);
    auto rgResource = std::make_unique<RenderGraphBuffer>(name, handle, desc);
    rgResource->SetImportedResource(resource);
    rgResource->AddFlag(RGResourceFlags::Imported);
    rgResource->SetInitialState(initialState);

    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RGResourceHandle RenderGraph::CreateTexture(const std::string& name, const rhi::TextureDesc& desc) {
    RGResourceHandle handle = {static_cast<u32>(resources_.size() + 1), 0};
    if (desc.size.x >= 1000 && desc.size.y >= 500) {
        static int rgTexDbg = 0;
        if (rgTexDbg < 40) { ++rgTexDbg;
            PRIMAL_LOG_STREAM(Error, "RG") << "[RG_TEX] handle=" << handle.index << " name=" << name
                      << " usage=" << (int)desc.usage << " " << desc.size.x << "x" << desc.size.y << std::endl;
        }
    }
    auto rgResource = std::make_unique<RenderGraphTexture>(name, handle, desc);
    rgResource->AddFlag(RGResourceFlags::Transient);
    
    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RGResourceHandle RenderGraph::CreateBuffer(const std::string& name, const rhi::BufferDesc& desc) {
    RGResourceHandle handle = {static_cast<u32>(resources_.size() + 1), 0};
    auto rgResource = std::make_unique<RenderGraphBuffer>(name, handle, desc);
    rgResource->AddFlag(RGResourceFlags::Transient);

    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RenderGraphResource* RenderGraph::GetResource(RGResourceHandle handle) {
    if (handle.index == 0 || handle.index > resources_.size()) {
        return nullptr;
    }
    return resources_[handle.index - 1].get();
}

void RenderGraph::RegisterResourceRead(RenderGraphPass* pass, RGResourceHandle handle, rhi::ResourceState state) {
    auto* resource = GetResource(handle);
    if (resource) {
        pass->AddInput(resource, state);
    }
}

void RenderGraph::RegisterResourceWrite(RenderGraphPass* pass, RGResourceHandle handle, rhi::ResourceState state) {
    auto* resource = GetResource(handle);
    if (resource) {
        pass->AddOutput(resource, state);
        resource->SetProducer(pass);
    }
}

void RenderGraph::MarkAsOutput(RGResourceHandle handle) {
    auto* resource = GetResource(handle);
    if (resource) {
        resource->AddFlag(RGResourceFlags::Output);
    }
}

void RenderGraph::Compile() {
    cpu_stall_audit::BuildEnd();
    cpu_stall_audit::Scope cpuCompile("graph.compile");
    // 1. 剔除未使用的 Pass
    CullPasses();

    // 2. 计算资源生命周期
    CalculateResourceLifetimes();

    // 3. 分配资源
    AllocateResources();

    // 4. 插入 Barriers
    InsertBarriers();

    // All render graph culling, allocation and synchronization above are
    // production work. Only the following diagnostic query work is optional.
    if(!timestampsEnabled_)return;
    // 5. 准备 Timestamp Query Pool
    auto& currentFrameData = queryFrames_[currentQueryFrameIndex_];
    currentFrameData.recordingSequence = ++timestampRecordingSequence_;
    u32 requiredQueries = static_cast<u32>(activePasses_.size() * 2);

    // 记录 Pass Names
    currentFrameData.passNames.clear();
    currentFrameData.passNames.reserve(activePasses_.size());
    for(auto* pass : activePasses_) {
        currentFrameData.passNames.push_back(pass->GetName());
    }

    // 检查并创建/重建 QueryPool
    if (requiredQueries > 0) {
        if (currentFrameData.queryPool == rhi::handles::INVALID_QUERY_POOL || currentFrameData.capacity < requiredQueries) {
            if (currentFrameData.queryPool != rhi::handles::INVALID_QUERY_POOL) {
                device_.DestroyQueryPool(currentFrameData.queryPool);
            }

            u32 newCapacity = std::max(requiredQueries, 64u); // 最小 64，避免频繁重建
            // 向上取整到 64 的倍数
            newCapacity = (newCapacity + 63) & ~63;

            rhi::QueryPoolDesc desc;
            desc.type = rhi::QueryType::Timestamp;
            desc.queryCount = newCapacity;
            currentFrameData.queryPool = device_.CreateQueryPool(desc);
            currentFrameData.capacity = newCapacity;
        }
    }
    currentFrameData.ready = false;
}

void RenderGraph::CullPasses() {
    std::stack<RenderGraphPass*> passStack;
    std::unordered_set<RenderGraphPass*> visited;

    // 1. 标记所有 Pass 为剔除状态
    for (auto& pass : passes_) {
        pass->SetCulled(true);
    }
    activePasses_.clear();

    // 2. 收集根节点 Pass
    for (auto& pass : passes_) {
        bool isRoot = pass->HasSideEffect();
        
        if (!isRoot) {
            for (const auto& output : pass->GetOutputs()) {
                auto* resource = output.resource;
                if (HasFlag(resource->GetFlags(), RGResourceFlags::Output) ||
                    HasFlag(resource->GetFlags(), RGResourceFlags::Imported)) {
                    isRoot = true;
                    break;
                }
            }
        }

        if (isRoot) {
            passStack.push(pass.get());
        }
    }

    // 3. 遍历依赖图
    while (!passStack.empty()) {
        auto* pass = passStack.top();
        passStack.pop();

        if (visited.count(pass)) {
            continue;
        }
        visited.insert(pass);

        // 标记为活跃
        pass->SetCulled(false);

        // 遍历输入资源，将生产者 Pass 加入栈
        for (const auto& input : pass->GetInputs()) {
            auto* resource = input.resource;
            auto* producer = resource->GetProducer();
            if (producer && !visited.count(producer)) {
                passStack.push(producer);
            }
        }
    }

    // 4. 构建 activePasses_ 列表 (保持原有拓扑顺序)
    for (auto& pass : passes_) {
        if (!pass->IsCulled()) {
            activePasses_.push_back(pass.get());
        }
    }
}

void RenderGraph::CalculateResourceLifetimes() {
    for (auto* pass : activePasses_) {
        for (const auto& input : pass->GetInputs()) {
            auto* resource = input.resource;
            if (!resource->GetFirstPass()) {
                resource->SetFirstPass(pass);
            }
            resource->SetLastPass(pass);
        }
        for (const auto& output : pass->GetOutputs()) {
            auto* resource = output.resource;
            if (!resource->GetFirstPass()) {
                resource->SetFirstPass(pass);
            }
            resource->SetLastPass(pass);
        }
    }
}



// Helper to check texture compatibility
static bool IsCompatible(const rhi::TextureDesc& a, const rhi::TextureDesc& b) {
    return a.size.x == b.size.x &&
           a.size.y == b.size.y &&
           a.size.z == b.size.z &&
           a.mipLevels == b.mipLevels &&
           a.arraySize == b.arraySize &&
           a.format == b.format &&
           a.type == b.type &&
           a.usage == b.usage &&
           a.memoryUsage == b.memoryUsage;
}

// Helper to check buffer compatibility
static bool IsCompatible(const rhi::BufferDesc& a, const rhi::BufferDesc& b) {
    return a.size == b.size &&
           a.type == b.type &&
           a.usage == b.usage &&
           a.memoryUsage == b.memoryUsage &&
           a.bindFlags == b.bindFlags;
}

void RenderGraph::AllocateResources() {
    // 1. Build Pass Index Map for O(1) lookup
    std::unordered_map<RenderGraphPass*, u32> passIndexMap;
    for (u32 i = 0; i < activePasses_.size(); ++i) {
        passIndexMap[activePasses_[i]] = i;
    }

    // 2. Build Allocation/Deallocation Events
    // resourcesStartingAt[i] contains resources that are FIRST used in pass i
    // resourcesEndingAt[i] contains resources that are LAST used in pass i
    utl::vector<utl::vector<RenderGraphResource*>> resourcesStartingAt(activePasses_.size());
    utl::vector<utl::vector<RenderGraphResource*>> resourcesEndingAt(activePasses_.size());

    for (const auto& resource : resources_) {
        // Only manage transient resources that are not imported
        if (!HasFlag(resource->GetFlags(), RGResourceFlags::Transient) || 
            HasFlag(resource->GetFlags(), RGResourceFlags::Imported)) {
            continue;
        }
        
        // If resource is not used, skip
        if (!resource->GetFirstPass()) continue;

        u32 startIdx = passIndexMap[resource->GetFirstPass()];
        u32 endIdx = passIndexMap[resource->GetLastPass()];

        resourcesStartingAt[startIdx].push_back(resource.get());
        resourcesEndingAt[endIdx].push_back(resource.get());
    }

    // 3. Track pool usage for current frame
    // poolLocked[k] == true means resourcePool_[k] is assigned to a resource in the current active interval
    utl::vector<bool> poolLocked(resourcePool_.size(), false);

    // 4. Simulate Execution to Allocate Resources with Aliasing
    for (u32 i = 0; i < activePasses_.size(); ++i) {
        // A. Allocate resources starting at this pass
        for (auto* res : resourcesStartingAt[i]) {
            int foundIdx = -1;

            if (auto* tex = dynamic_cast<RenderGraphTexture*>(res)) {
                // Find compatible free texture in pool
                for(size_t k = 0; k < resourcePool_.size(); ++k) {
                    if (!poolLocked[k] && resourcePool_[k].isTexture && 
                        IsCompatible(resourcePool_[k].texDesc, tex->GetDesc())) {
                        foundIdx = static_cast<int>(k);
                        break;
                    }
                }
                
                if (foundIdx != -1) {
                    // Reuse existing
                    poolLocked[foundIdx] = true;
                    resourcePool_[foundIdx].lastUsedFrame = currentFrame_;
                    tex->SetPhysicalHandle(resourcePool_[foundIdx].handle);
                } else {
                    // Create new
                    rhi::ResourceHandle handle = device_.CreateTexture(tex->GetDesc());
                    // P0-B（validation 预热瞬态定位）: 新物理纹理立即 prime 到
                    // SRO——瞬态纹理在被 RG 声明触达前就可能被 raw descriptor
                    // 绑成 SampledImage（承诺 SHADER_READ_ONLY）；池预热期
                    // （kKeepFrames=3）首帧绑定早于首次转换会产生
                    // "expects SRO, actual UNDEFINED"。非 Vulkan 平台为 no-op。
                    device_.PrimeTextureLayout(handle);
                    if (handle == rhi::handles::INVALID_RESOURCE) {
                        // 分配失败若静默，下游 BeginRenderPass 会拿到 INVALID
                        // 物理句柄构造出畸形 render pass（Vulkan validation 崩溃
                        // / 黑屏，2026-09-02 排查记录）。
                        static bool s_allocFailLogged = false;
                        if (!s_allocFailLogged) {
                            s_allocFailLogged = true;
                            std::cerr << "[RG] texture alloc FAILED: " << tex->GetName() << std::endl;
                        }
                    }
                    
                    PooledResource pooled{
                        handle,
                        tex->GetDesc(),
                        {},
                        static_cast<u32>(currentFrame_),
                        true
                    };
                    
                    resourcePool_.push_back(pooled);
                    poolLocked.push_back(true);
                    
                    tex->SetPhysicalHandle(handle);
                }
            } else if (auto* buf = dynamic_cast<RenderGraphBuffer*>(res)) {
                // Find compatible free buffer in pool
                for(size_t k = 0; k < resourcePool_.size(); ++k) {
                    if (!poolLocked[k] && !resourcePool_[k].isTexture && 
                        IsCompatible(resourcePool_[k].bufDesc, buf->GetDesc())) {
                        foundIdx = static_cast<int>(k);
                        break;
                    }
                }
                
                if (foundIdx != -1) {
                    // Reuse existing
                    poolLocked[foundIdx] = true;
                    resourcePool_[foundIdx].lastUsedFrame = currentFrame_;
                    buf->SetPhysicalHandle(resourcePool_[foundIdx].handle);
                } else {
                    // Create new
                    rhi::ResourceHandle handle = device_.CreateBuffer(buf->GetDesc());
                    
                    PooledResource pooled{
                        handle,
                        {},
                        buf->GetDesc(),
                        static_cast<u32>(currentFrame_),
                        false
                    };
                    
                    resourcePool_.push_back(pooled);
                    poolLocked.push_back(true);
                    
                    buf->SetPhysicalHandle(handle);
                }
            }
        }

        // B. Release resources ending at this pass (make available for reuse in SAME frame)
        for (auto* res : resourcesEndingAt[i]) {
            rhi::ResourceHandle handle = res->GetPhysicalHandle();
            // Find index in pool
            for(size_t k = 0; k < resourcePool_.size(); ++k) {
                if (resourcePool_[k].handle == handle) {
                    poolLocked[k] = false; // Unlock
                    break;
                }
            }
        }
    }
}

void RenderGraph::InsertBarriers() {
    // 追踪每个资源的当前状态
    // index -> state
    // 对导入资源,优先用 SetInitialState 设置的真实状态;否则保持 Unknown
    // (Unknown 在 Metal Render encoder 中是 no-op,跨 encoder 类型可能失去同步保障)
    utl::vector<rhi::ResourceState> resourceStates(resources_.size() + 1, rhi::ResourceState::Unknown);
    for (size_t i = 0; i < resources_.size(); ++i) {
        const auto& res = resources_[i];
        if (HasFlag(res->GetFlags(), RGResourceFlags::Imported)) {
            rhi::ResourceState init = res->GetInitialState();
            if (init != rhi::ResourceState::Unknown) {
                resourceStates[i + 1] = init;
            }
        }
    }

    for (auto* pass : activePasses_) {
        // 处理输入资源 (Read)
        for (const auto& input : pass->GetInputs()) {
            auto* resource = input.resource;
            u32 index = resource->GetHandle().index;
            rhi::ResourceState currentState = resourceStates[index];
            rhi::ResourceState requiredState = input.state;

            if (currentState != requiredState && currentState != rhi::ResourceState::Unknown) {
                rhi::ResourceBarrier barrier;
                barrier.resource = resource->GetPhysicalHandle();
                barrier.beforeState = currentState;
                barrier.afterState = requiredState;
                barrier.subresource = rhi::RHI_ALL_SUBRESOURCES; 
                barrier.queueFamily = 0xFFFFFFFF; 
                
                pass->AddBarrier(barrier);
            } else if (currentState == rhi::ResourceState::Unknown) {
                 // 初始转换
                rhi::ResourceBarrier barrier;
                barrier.resource = resource->GetPhysicalHandle();
                barrier.beforeState = rhi::ResourceState::Unknown; 
                barrier.afterState = requiredState;
                barrier.subresource = rhi::RHI_ALL_SUBRESOURCES;
                barrier.queueFamily = 0xFFFFFFFF;

                pass->AddBarrier(barrier);
            }

            resourceStates[index] = requiredState;
        }

        // 处理输出资源 (Write)
        for (const auto& output : pass->GetOutputs()) {
            auto* resource = output.resource;
            u32 index = resource->GetHandle().index;
            rhi::ResourceState currentState = resourceStates[index];
            rhi::ResourceState requiredState = output.state;

            if (currentState != requiredState) {
                rhi::ResourceBarrier barrier;
                barrier.resource = resource->GetPhysicalHandle();
                barrier.beforeState = currentState == rhi::ResourceState::Unknown ? rhi::ResourceState::Unknown : currentState;
                barrier.afterState = requiredState;
                barrier.subresource = rhi::RHI_ALL_SUBRESOURCES;
                barrier.queueFamily = 0xFFFFFFFF;

                pass->AddBarrier(barrier);
            }
            
            resourceStates[index] = requiredState;
        }
    }
}

void RenderGraph::Execute(rhi::RHICommandBuffer* cmdBuffer) {
    cpu_stall_audit::Scope cpuExecute("graph.execute");
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
    auto* timestampAuditCmd=device_.GetPlatform()==rhi::RHIPlatform::Vulkan?static_cast<rhi::VulkanCommandBuffer*>(cmdBuffer):nullptr;
    rhi::pass_timestamp::instance.Graph(timestampAuditCmd);
#endif
    RenderGraphContext context;
    context.cmdBuffer = cmdBuffer;
    context.graph = this;

    auto& currentFrameData = queryFrames_[currentQueryFrameIndex_];
    rhi::QueryPoolHandle queryPool = currentFrameData.queryPool;
    bool enableTimestamp = timestampsEnabled_ && (queryPool != rhi::handles::INVALID_QUERY_POOL);

    // T4.6.5 part 24.2 (B6 fix): Vulkan spec requires queries to be reset
    // between uses. Pool is reused across frames (rebuilt only on capacity
    // growth at line 200); without per-frame reset, every frame after the
    // first fires VUID-vkCmdWriteTimestamp-None-00830. GPU-side vkCmdResetQueryPool
    // recorded into the cmd buffer is more reliable than CPU-side vkResetQueryPool
    // on MoltenVK + validation layer. No-op on Metal/Dawn (empty base virtual).
    if (enableTimestamp) {
        cmdBuffer->ResetQueryPool(queryPool, 0, currentFrameData.capacity);
    }

    for (size_t i = 0; i < activePasses_.size(); ++i) {
        auto* pass = activePasses_[i];
        
        // std::cout << "RenderGraph: Preparing Pass " << pass->GetName() << std::endl;

        bool hasRenderPass = pass->GetRenderPassDesc().has_value();
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
        const int timestampAuditIndex=rhi::pass_timestamp::instance.BeginPass(timestampAuditCmd,pass->GetName().c_str(),hasRenderPass);
#endif

        // 1. Write Begin Timestamp
        // Only write manually if NOT using RenderPass (RenderPass handles it via desc to support Apple Silicon)
        if (enableTimestamp && !hasRenderPass) {
            cmdBuffer->WriteTimestamp(queryPool, static_cast<u32>(i * 2));
        }

        // 执行 Pre-Pass Barriers
        const auto& barriers = pass->GetBarriers();
        if (!barriers.empty()) {
            // std::cout << "RenderGraph: Inserting Barriers for " << pass->GetName() << std::endl;
            cmdBuffer->InsertBarrier(barriers.data(), static_cast<u32>(barriers.size()));
        }

        // 处理自动 RenderPass Begin/End
        if (hasRenderPass) {
            // std::cout << "RenderGraph: BeginRenderPass for " << pass->GetName() << std::endl;
            const auto& rgDesc = pass->GetRenderPassDesc().value();
            rhi::RenderPassDesc desc;
            
            // 转换 Color Attachments
            desc.colorAttachments.resize(rgDesc.colors.size());
            for (size_t k = 0; k < rgDesc.colors.size(); ++k) {
                const auto& src = rgDesc.colors[k];
                auto& dst = desc.colorAttachments[k];
                
                auto* res = GetResource(src.texture);
                if (res) {
                    dst.texture = res->GetPhysicalHandle();
                }
                dst.mipLevel = src.level;
                dst.arrayLayer = src.slice; // Assuming slice maps to arrayLayer
                dst.loadOp = src.loadOp;
                dst.storeOp = src.storeOp;
                dst.clearValue = src.clearColor;
            }
            
            // 转换 Depth/Stencil Attachment
            if (rgDesc.depthStencil.texture.IsValid()) {
                auto* res = GetResource(rgDesc.depthStencil.texture);
                if (res) {
                    desc.depthAttachment.texture = res->GetPhysicalHandle();
                    desc.stencilAttachment.texture = res->GetPhysicalHandle(); // Same texture for depth/stencil
                }
                desc.depthAttachment.mipLevel = rgDesc.depthStencil.level;
                desc.depthAttachment.arrayLayer = rgDesc.depthStencil.slice;
                desc.depthAttachment.loadOp = rgDesc.depthStencil.depthLoadOp;
                desc.depthAttachment.storeOp = rgDesc.depthStencil.depthStoreOp;
                desc.depthAttachment.clearValue = rhi::ClearValue{ math::v4{ static_cast<float>(rgDesc.depthStencil.clearDepth), 0.f, 0.f, 1.f } }; // Depth clear value
                
                desc.stencilAttachment.mipLevel = rgDesc.depthStencil.level;
                desc.stencilAttachment.arrayLayer = rgDesc.depthStencil.slice;
                desc.stencilAttachment.loadOp = rgDesc.depthStencil.stencilLoadOp;
                desc.stencilAttachment.storeOp = rgDesc.depthStencil.stencilStoreOp;
                desc.stencilAttachment.clearValue = rhi::ClearValue{ math::v4{ 1.0f, static_cast<float>(rgDesc.depthStencil.clearStencil), 0.f, 1.f } }; // Stencil clear value
            }

            // Set Multi-View / Layered Rendering
            desc.renderTargetArrayLength = rgDesc.renderTargetArrayLength;

            // Timestamp in RenderPass (Metal optimization)
            if (enableTimestamp) {
                desc.enableTimestamp = true;
                desc.timestampQueryPool = queryPool;
                desc.beginTimestampIndex = static_cast<u32>(i * 2);
                desc.endTimestampIndex = static_cast<u32>(i * 2 + 1);
            }

            // Begin RenderPass
            cmdBuffer->BeginRenderPass(desc);
            // std::cout << "RenderGraph: BeginRenderPass Done" << std::endl;
        }

        // std::cout << "RenderGraph: Executing Pass " << pass->GetName() << std::endl;
        { cpu_stall_audit::Scope cpuPass(pass->GetName().c_str()); pass->Execute(context); }

        if (hasRenderPass) {
            cmdBuffer->EndRenderPass();
        }

        lumencompare::AfterPass(device_, cmdBuffer, *pass);
#if defined(ENABLE_VULKAN) && ENABLE_VULKAN
        rhi::pass_timestamp::instance.EndPass(timestampAuditCmd,timestampAuditIndex);
#endif

        // 2. Write End Timestamp (only if NOT in RenderPass, because RenderPass handles it automatically via desc)
        // BUT wait, WriteTimestamp in MetalCommandBuffer.cpp is disabled for RenderPassEncoder on Apple Silicon.
        // So we rely on RenderPassDesc to capture timestamps.
        // However, if the pass is NOT a RenderPass (e.g. Compute), we MUST use WriteTimestamp.
        if (enableTimestamp && !hasRenderPass) {
            cmdBuffer->WriteTimestamp(queryPool, static_cast<u32>(i * 2 + 1));
        }
    }

    if (enableTimestamp) {
        currentFrameData.ready = true;
    }
}


// Helper for Debug
static std::string GetResourceStateString(rhi::ResourceState state) {
    switch (state) {
        case rhi::ResourceState::Unknown: return "Unknown";
        case rhi::ResourceState::General: return "General";
        case rhi::ResourceState::ShaderResource: return "ShaderResource";
        case rhi::ResourceState::RenderTarget: return "RenderTarget";
        case rhi::ResourceState::DepthStencilReadOnly: return "DepthStencilReadOnly";
        case rhi::ResourceState::DepthStencil: return "DepthStencil";
        case rhi::ResourceState::UnorderedAccess: return "UnorderedAccess";
        case rhi::ResourceState::CopyDest: return "CopyDest";
        case rhi::ResourceState::CopySource: return "CopySource";
        case rhi::ResourceState::Present: return "Present";
        default: return "Unknown";
    }
}

std::string RenderGraph::DumpGraphViz() const {
    std::stringstream ss;
    ss << "digraph RenderGraph {\n";
    ss << "  rankdir=LR;\n";
    ss << "  node [shape=box, style=filled, fontname=\"Helvetica\"];\n";

    // Passes
    for (const auto* pass : activePasses_) {
        std::string color = "lightgrey";
        switch (pass->GetCategory()) {
            case RGPassCategory::Visibility: color = "lightblue"; break;
            case RGPassCategory::Depth: color = "lightcyan"; break;
            case RGPassCategory::Main: color = "lightgreen"; break;
            case RGPassCategory::Lighting: color = "yellow"; break;
            case RGPassCategory::PostProcess: color = "orange"; break;
            case RGPassCategory::UI: color = "pink"; break;
            case RGPassCategory::Present: color = "red"; break;
            default: break;
        }
        ss << "  \"" << pass->GetName() << "\" [fillcolor=\"" << color << "\"];\n";
    }

    // Resources and Edges
    for (const auto* pass : activePasses_) {
        // Inputs
        for (const auto& input : pass->GetInputs()) {
            auto* res = input.resource;
            ss << "  \"" << res->GetName() << "\" -> \"" << pass->GetName() << "\" [label=\"" << GetResourceStateString(input.state) << "\"];\n";
            ss << "  \"" << res->GetName() << "\" [shape=ellipse, fillcolor=white];\n";
        }
        // Outputs
        for (const auto& output : pass->GetOutputs()) {
            auto* res = output.resource;
            ss << "  \"" << pass->GetName() << "\" -> \"" << res->GetName() << "\" [label=\"" << GetResourceStateString(output.state) << "\"];\n";
            ss << "  \"" << res->GetName() << "\" [shape=ellipse, fillcolor=white];\n";
        }
    }

    ss << "}\n";
    return ss.str();
}

void RenderGraph::ResolveTimestamps() {
    if(!timestampsEnabled_)return;
    timestampResultsFresh_ = false;
    timestampQueryReadOk_ = false;
    timestampQueryReadCpuMs_ = -1.0;
    timestampReadMin_ = timestampReadMax_ = 0;
    timestampPreviousBound_ = timestampLastTickBound_;
    u32 prevFrameIndex = 1 - currentQueryFrameIndex_;
    auto& frameData = queryFrames_[prevFrameIndex];

    if (!frameData.ready || frameData.queryPool == rhi::handles::INVALID_QUERY_POOL) {
        return;
    }

    u32 passCount = static_cast<u32>(frameData.passNames.size());
    if (passCount == 0) return;

    utl::vector<u64> results(passCount * 2);
    // 使用 0 作为 offset (假设 RHI 实现正确处理)
    // 注意：GetQueryPoolResults 是我们刚添加到 RHIDevice 的接口
    static const bool timingEnabled = [] { const char* e=std::getenv("PRIMAL_FRAME_TIMING");return e && std::strcmp(e,"1")==0; }();
    const auto queryStart = timingEnabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    bool readOk = device_.GetQueryPoolResults(frameData.queryPool, 0, passCount * 2, results.data(), sizeof(u64));
    if (timingEnabled) timestampQueryReadCpuMs_ =
        std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-queryStart).count();
    timestampQueryReadOk_ = readOk;
    // In the opt-in Vulkan nonblocking path, false means at least one query
    // may still be pending. The driver is permitted to have written a subset:
    // do not publish those pairs or clear the last accepted timing map. This
    // records a non-fresh, unsuccessful read, not a zero-cost frame. The
    // existing two-slot ring does not retain/retry this sample after reuse.
    const char* nonblocking = std::getenv("PRIMAL_VK_QUERY_NONBLOCKING");
    if (!readOk && device_.GetPlatform()==rhi::RHIPlatform::Vulkan &&
        nonblocking && std::strcmp(nonblocking,"1")==0) {
        frameData.ready = false;
        return;
    }
    static u32 dbgCount = 0;
    if ((dbgCount++ % 120) == 0) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[RG-TSdbg] readOk=" << readOk << " passes=" << passCount
                  << " t[0]=" << results[0] << " t[1]=" << results[1] << std::endl;
    }
    // T8.13/P0: MoltenVK quirk — vkGetQueryPoolResults can return
    // VK_NOT_READY even when WAIT_BIT was set, while the results buffer
    // already holds valid staged timestamps. Trust the per-pair validity
    // filter below instead of the boolean.
    {
        passExecutionTimes_.clear();
        
        // 假设 1 tick = 1 nanosecond (Apple Silicon Metal)
        // 转换为毫秒: / 1,000,000.0
        double timestampPeriod = device_.GetTimestampPeriod();
        
        for (u32 i = 0; i < passCount; ++i) {
            u64 start = results[2 * i];
            u64 end = results[2 * i + 1];
            
            // 简单的溢出检查和有效性检查
            if (end > start && start != 0) {
                if (!timestampReadMin_ || start < timestampReadMin_) timestampReadMin_ = start;
                if (end > timestampReadMax_) timestampReadMax_ = end;
                double durationMs = static_cast<double>(end - start) * timestampPeriod / 1000000.0;
                passExecutionTimes_[frameData.passNames[i]] = durationMs;
            }
        }
        // MoltenVK may return false despite valid staged pairs. Authenticate
        // the complete valid-pair tick range against the last accepted range;
        // stale or overlapping values are rejected even when readOk is true.
        if (timestampReadMin_ && timestampReadMax_ > timestampReadMin_ &&
            timestampReadMin_ > timestampLastTickBound_ &&
            frameData.recordingSequence > timestampResolvedSequence_) {
            timestampResolvedSequence_ = frameData.recordingSequence;
            timestampLastTickBound_ = timestampReadMax_;
            timestampResultsFresh_ = true;
        }
    }
    
    frameData.ready = false;
    // frameData.passNames.clear(); // 不在这里清除，而在 Compile 开始时清除并重新填充
}

} // namespace primal::graphics::rendergraph
