#include "RenderGraph.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include <algorithm>
#include <iostream>
#include <sstream>
#include <stack>
#include <unordered_set>

namespace primal::graphics::rendergraph {

RenderGraph::RenderGraph(rhi::RHIDeviceBase& device) : device_(device) {
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
    ResolveTimestamps();
    currentQueryFrameIndex_ = 1 - currentQueryFrameIndex_;

    passes_.clear();
    resources_.clear();
    resourceMap_.clear();
    activePasses_.clear();
    
    currentFrame_++;
    CleanupPool();
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

RGResourceHandle RenderGraph::ImportResource(const std::string& name, rhi::ResourceHandle resource) {
    RGResourceHandle handle = {static_cast<u32>(resources_.size() + 1), 0};
    auto rgResource = std::make_unique<RenderGraphResource>(name, handle, RGResourceType::Unknown);
    rgResource->SetImportedResource(resource);
    
    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RGResourceHandle RenderGraph::ImportTexture(const std::string& name, rhi::ResourceHandle resource, const rhi::TextureDesc& desc) {
    RGResourceHandle handle = {static_cast<u32>(resources_.size() + 1), 0};
    auto rgResource = std::make_unique<RenderGraphTexture>(name, handle, desc);
    rgResource->SetImportedResource(resource);
    rgResource->AddFlag(RGResourceFlags::Imported);
    
    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RGResourceHandle RenderGraph::ImportBuffer(const std::string& name, rhi::ResourceHandle resource, const rhi::BufferDesc& desc) {
    RGResourceHandle handle = {static_cast<u32>(resources_.size() + 1), 0};
    auto rgResource = std::make_unique<RenderGraphBuffer>(name, handle, desc);
    rgResource->SetImportedResource(resource);
    rgResource->AddFlag(RGResourceFlags::Imported);
    
    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RGResourceHandle RenderGraph::CreateTexture(const std::string& name, const rhi::TextureDesc& desc) {
    RGResourceHandle handle = {static_cast<u32>(resources_.size() + 1), 0};
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
    // 1. 剔除未使用的 Pass
    CullPasses();

    // 2. 计算资源生命周期
    CalculateResourceLifetimes();

    // 3. 分配资源
    AllocateResources();

    // 4. 插入 Barriers
    InsertBarriers();

    // 5. 准备 Timestamp Query Pool
    auto& currentFrameData = queryFrames_[currentQueryFrameIndex_];
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
    utl::vector<rhi::ResourceState> resourceStates(resources_.size() + 1, rhi::ResourceState::Unknown);

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
    RenderGraphContext context;
    context.cmdBuffer = cmdBuffer;
    context.graph = this;

    auto& currentFrameData = queryFrames_[currentQueryFrameIndex_];
    rhi::QueryPoolHandle queryPool = currentFrameData.queryPool;
    bool enableTimestamp = (queryPool != rhi::handles::INVALID_QUERY_POOL);

    for (size_t i = 0; i < activePasses_.size(); ++i) {
        auto* pass = activePasses_[i];
        
        // std::cout << "RenderGraph: Preparing Pass " << pass->GetName() << std::endl;

        bool hasRenderPass = pass->GetRenderPassDesc().has_value();

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
        pass->Execute(context);

        if (hasRenderPass) {
            cmdBuffer->EndRenderPass();
        }

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
    if (device_.GetQueryPoolResults(frameData.queryPool, 0, passCount * 2, results.data(), sizeof(u64))) {
        passExecutionTimes_.clear();
        
        // 假设 1 tick = 1 nanosecond (Apple Silicon Metal)
        // 转换为毫秒: / 1,000,000.0
        double timestampPeriod = device_.GetTimestampPeriod();
        
        for (u32 i = 0; i < passCount; ++i) {
            u64 start = results[2 * i];
            u64 end = results[2 * i + 1];
            
            // 简单的溢出检查和有效性检查
            if (end > start && start != 0) {
                double durationMs = static_cast<double>(end - start) * timestampPeriod / 1000000.0;
                passExecutionTimes_[frameData.passNames[i]] = durationMs;
            }
        }
    }
    
    frameData.ready = false;
    // frameData.passNames.clear(); // 不在这里清除，而在 Compile 开始时清除并重新填充
}

} // namespace primal::graphics::rendergraph
