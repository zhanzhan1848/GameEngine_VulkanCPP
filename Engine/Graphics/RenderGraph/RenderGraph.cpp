#include "RenderGraph.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include <algorithm>
#include <iostream>
#include <stack>
#include <unordered_set>

namespace primal::graphics::rendergraph {

RenderGraph::RenderGraph(rhi::RHIDeviceBase& device) : device_(device) {
}

RenderGraph::~RenderGraph() {
    Clear();
}


void RenderGraph::Clear() {
    passes_.clear();
    resources_.clear();
    resourceMap_.clear();
    activePasses_.clear();
    
    currentFrame_++;
    CleanupPool();
}

void RenderGraph::CleanupPool() {
    // Keep resources for some frames to reduce thrashing
    const uint64_t kKeepFrames = 30; 
    
    auto it = std::remove_if(resourcePool_.begin(), resourcePool_.end(), 
        [this, kKeepFrames](const PooledResource& res) {
            if (currentFrame_ > res.lastUsedFrame + kKeepFrames) {
                if (res.isTexture) {
                    device_.DestroyTexture(res.handle);
                } else {
                    device_.DestroyBuffer(res.handle);
                }
                return true;
            }
            return false;
        });
    
    resourcePool_.erase(it, resourcePool_.end());
}

RGResourceHandle RenderGraph::ImportResource(const std::string& name, rhi::ResourceHandle resource) {
    RGResourceHandle handle = {static_cast<uint32_t>(resources_.size() + 1), 0};
    auto rgResource = std::make_unique<RenderGraphResource>(name, handle);
    rgResource->SetImportedResource(resource);
    
    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RGResourceHandle RenderGraph::ImportTexture(const std::string& name, rhi::ResourceHandle resource, const rhi::TextureDesc& desc) {
    RGResourceHandle handle = {static_cast<uint32_t>(resources_.size() + 1), 0};
    auto rgResource = std::make_unique<RenderGraphTexture>(name, handle, desc);
    rgResource->SetImportedResource(resource);
    rgResource->AddFlag(RGResourceFlags::Imported);
    
    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RGResourceHandle RenderGraph::ImportBuffer(const std::string& name, rhi::ResourceHandle resource, const rhi::BufferDesc& desc) {
    RGResourceHandle handle = {static_cast<uint32_t>(resources_.size() + 1), 0};
    auto rgResource = std::make_unique<RenderGraphBuffer>(name, handle, desc);
    rgResource->SetImportedResource(resource);
    rgResource->AddFlag(RGResourceFlags::Imported);
    
    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RGResourceHandle RenderGraph::CreateTexture(const std::string& name, const rhi::TextureDesc& desc) {
    RGResourceHandle handle = {static_cast<uint32_t>(resources_.size() + 1), 0};
    auto rgResource = std::make_unique<RenderGraphTexture>(name, handle, desc);
    rgResource->AddFlag(RGResourceFlags::Transient);
    
    resources_.push_back(std::move(rgResource));
    resourceMap_[name] = handle;
    return handle;
}

RGResourceHandle RenderGraph::CreateBuffer(const std::string& name, const rhi::BufferDesc& desc) {
    RGResourceHandle handle = {static_cast<uint32_t>(resources_.size() + 1), 0};
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
    std::unordered_map<RenderGraphPass*, uint32_t> passIndexMap;
    for (uint32_t i = 0; i < activePasses_.size(); ++i) {
        passIndexMap[activePasses_[i]] = i;
    }

    // 2. Build Allocation/Deallocation Events
    // resourcesStartingAt[i] contains resources that are FIRST used in pass i
    // resourcesEndingAt[i] contains resources that are LAST used in pass i
    std::vector<std::vector<RenderGraphResource*>> resourcesStartingAt(activePasses_.size());
    std::vector<std::vector<RenderGraphResource*>> resourcesEndingAt(activePasses_.size());

    for (const auto& resource : resources_) {
        // Only manage transient resources that are not imported
        if (!HasFlag(resource->GetFlags(), RGResourceFlags::Transient) || 
            HasFlag(resource->GetFlags(), RGResourceFlags::Imported)) {
            continue;
        }
        
        // If resource is not used, skip
        if (!resource->GetFirstPass()) continue;

        uint32_t startIdx = passIndexMap[resource->GetFirstPass()];
        uint32_t endIdx = passIndexMap[resource->GetLastPass()];

        resourcesStartingAt[startIdx].push_back(resource.get());
        resourcesEndingAt[endIdx].push_back(resource.get());
    }

    // 3. Track pool usage for current frame
    // poolLocked[k] == true means resourcePool_[k] is assigned to a resource in the current active interval
    std::vector<bool> poolLocked(resourcePool_.size(), false);

    // 4. Simulate Execution to Allocate Resources with Aliasing
    for (uint32_t i = 0; i < activePasses_.size(); ++i) {
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
                    
                    PooledResource pooled;
                    pooled.handle = handle;
                    pooled.texDesc = tex->GetDesc();
                    pooled.isTexture = true;
                    pooled.lastUsedFrame = currentFrame_;
                    
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
                    
                    PooledResource pooled;
                    pooled.handle = handle;
                    pooled.bufDesc = buf->GetDesc();
                    pooled.isTexture = false;
                    pooled.lastUsedFrame = currentFrame_;
                    
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
    std::vector<rhi::ResourceState> resourceStates(resources_.size() + 1, rhi::ResourceState::Unknown);

    for (auto* pass : activePasses_) {
        // 处理输入资源 (Read)
        for (const auto& input : pass->GetInputs()) {
            auto* resource = input.resource;
            uint32_t index = resource->GetHandle().index;
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
            uint32_t index = resource->GetHandle().index;
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

    for (auto* pass : activePasses_) {
        // 执行 Pre-Pass Barriers
        const auto& barriers = pass->GetBarriers();
        if (!barriers.empty()) {
            cmdBuffer->InsertBarrier(barriers.data(), static_cast<uint32_t>(barriers.size()));
        }

        pass->Execute(context);
    }
}

} // namespace primal::graphics::rendergraph
