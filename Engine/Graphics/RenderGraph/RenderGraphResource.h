#pragma once

#include "RenderGraphDefinitions.h"
#include "Graphics/RHI/Core/RHIResource.h"

namespace primal::graphics::rendergraph {

class RenderGraphPass;

/**
 * @brief 渲染图资源基类
 * @details 代表渲染图中的一个资源节点
 */
class RenderGraphResource {
public:
    RenderGraphResource(const std::string& name, RGResourceHandle handle, RGResourceType type)
        : name_(name), handle_(handle), type_(type) {}
    
    virtual ~RenderGraphResource() = default;

    const std::string& GetName() const { return name_; }
    RGResourceHandle GetHandle() const { return handle_; }
    RGResourceType GetType() const { return type_; }
    
    void SetImportedResource(rhi::ResourceHandle handle) {
        physicalHandle_ = handle;
        flags_ = flags_ | RGResourceFlags::Imported;
    }

    /**
     * @brief 设置导入资源的初始 GPU 状态
     * @details 用于 ImportTexture/ImportBuffer 时显式声明资源在导入时的真实状态。
     *          RenderGraph::InsertBarriers 会基于此插入正确的 from-initial-state barrier。
     *          不设置则默认 Unknown,RG 只能生成 beforeState=Unknown 的 barrier,
     *          在 Metal Render encoder 中是 no-op,跨 encoder 类型(Compute→Render)可能
     *          无法正确同步,导致采样到 stale/clear 数据。
     */
    void SetInitialState(rhi::ResourceState state) { initialState_ = state; }
    rhi::ResourceState GetInitialState() const { return initialState_; }

    // 获取实际的 GPU 资源 (如果已分配)
    rhi::ResourceHandle GetPhysicalHandle() const { return physicalHandle_; }
    void SetPhysicalHandle(rhi::ResourceHandle handle) { physicalHandle_ = handle; }

    RGResourceFlags GetFlags() const { return flags_; }
    void AddFlag(RGResourceFlags flag) { flags_ = flags_ | flag; }

    // 生命周期管理
    void SetFirstPass(RenderGraphPass* pass) { firstPass_ = pass; }
    void SetLastPass(RenderGraphPass* pass) { lastPass_ = pass; }
    RenderGraphPass* GetFirstPass() const { return firstPass_; }
    RenderGraphPass* GetLastPass() const { return lastPass_; }

    void SetProducer(RenderGraphPass* pass) { producer_ = pass; }
    RenderGraphPass* GetProducer() const { return producer_; }

    u32 GetRefCount() const { return refCount_; }
    void AddRef() { refCount_++; }
    void Release() { if (refCount_ > 0) refCount_--; }

protected:
    std::string name_;
    RGResourceHandle handle_;
    RGResourceType type_;
    RGResourceFlags flags_ = RGResourceFlags::None;

    rhi::ResourceHandle physicalHandle_ = rhi::handles::INVALID_RESOURCE; // 实际分配或导入的资源句柄

    // 导入资源在导入时的真实 GPU 状态(Unknown 表示调用方未声明,RG 视为不可靠)
    rhi::ResourceState initialState_ = rhi::ResourceState::Unknown;

    // 生命周期信息
    RenderGraphPass* firstPass_ = nullptr;
    RenderGraphPass* lastPass_ = nullptr;
    RenderGraphPass* producer_ = nullptr;
    u32 refCount_ = 0;
};

/**
 * @brief 纹理资源
 */
class RenderGraphTexture : public RenderGraphResource {
public:
    RenderGraphTexture(const std::string& name, RGResourceHandle handle, const rhi::TextureDesc& desc)
        : RenderGraphResource(name, handle, RGResourceType::Texture), desc_(desc) {}

    const rhi::TextureDesc& GetDesc() const { return desc_; }

private:
    rhi::TextureDesc desc_;
};

/**
 * @brief 缓冲区资源
 */
class RenderGraphBuffer : public RenderGraphResource {
public:
    RenderGraphBuffer(const std::string& name, RGResourceHandle handle, const rhi::BufferDesc& desc)
        : RenderGraphResource(name, handle, RGResourceType::Buffer), desc_(desc) {}

    const rhi::BufferDesc& GetDesc() const { return desc_; }

private:
    rhi::BufferDesc desc_;
};

} // namespace primal::graphics::rendergraph
