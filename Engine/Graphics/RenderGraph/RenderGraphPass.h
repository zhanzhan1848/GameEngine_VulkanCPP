#pragma once

#include "RenderGraphDefinitions.h"
#include "RenderGraphResource.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include <vector>
#include <optional>

namespace primal::graphics::rendergraph {

class RenderGraph;
class RenderGraphBuilder;

struct RenderGraphContext {
    rhi::RHICommandBuffer* cmdBuffer;
    RenderGraph* graph;
};

struct RGPassResourceDef {
    RenderGraphResource* resource;
    rhi::ResourceState state;
    RGAccessType access;
};

/**
 * @brief 渲染图通道基类
 */
class RenderGraphPass {
public:
    RenderGraphPass(const ::std::string& name, RGPassType type, RGPassCategory category = RGPassCategory::None)
        : name_(name), type_(type), category_(category) {}
    
    virtual ~RenderGraphPass() = default;

    virtual void Execute(RenderGraphContext& context) = 0;

    const ::std::string& GetName() const { return name_; }
    RGPassType GetType() const { return type_; }
    RGPassCategory GetCategory() const { return category_; }
    
    void SetCulled(bool culled) { culled_ = culled; }
    bool IsCulled() const { return culled_; }
    
    void SetSideEffect(bool sideEffect) { hasSideEffect_ = sideEffect; }
    bool HasSideEffect() const { return hasSideEffect_; }

    void AddInput(RenderGraphResource* resource, rhi::ResourceState state) {
        inputs_.push_back({resource, state, RGAccessType::Read});
    }

    void AddOutput(RenderGraphResource* resource, rhi::ResourceState state) {
        outputs_.push_back({resource, state, RGAccessType::Write});
    }

    const ::std::vector<RGPassResourceDef>& GetInputs() const { return inputs_; }
    const ::std::vector<RGPassResourceDef>& GetOutputs() const { return outputs_; }

    void AddBarrier(const rhi::ResourceBarrier& barrier) {
        barriers_.push_back(barrier);
    }
    const ::std::vector<rhi::ResourceBarrier>& GetBarriers() const { return barriers_; }

    void SetRenderPassDesc(const RGRenderPassDesc& desc) {
        renderPassDesc_ = desc;
    }
    const ::std::optional<RGRenderPassDesc>& GetRenderPassDesc() const {
        return renderPassDesc_;
    }

protected:
    ::std::string name_;
    RGPassType type_;
    RGPassCategory category_;
    bool culled_ = false;
    bool hasSideEffect_ = false;
    
    ::std::vector<RGPassResourceDef> inputs_;
    ::std::vector<RGPassResourceDef> outputs_;
    ::std::vector<rhi::ResourceBarrier> barriers_;
    ::std::optional<RGRenderPassDesc> renderPassDesc_;
};

/**
 * @brief 泛型渲染通道
 * @tparam Data Pass 数据结构
 */
template<typename Data>
class RenderGraphPassImpl : public RenderGraphPass {
public:
    using SetupFunc = ::std::function<void(Data&, RenderGraphBuilder&)>;
    using ExecuteFunc = ::std::function<void(const Data&, RenderGraphContext&)>;

    RenderGraphPassImpl(const ::std::string& name, RGPassType type, RGPassCategory category, SetupFunc setup, ExecuteFunc execute)
        : RenderGraphPass(name, type, category), setup_(setup), execute_(execute) {}
    
    // Maintain backward compatibility constructor
    RenderGraphPassImpl(const ::std::string& name, RGPassType type, SetupFunc setup, ExecuteFunc execute)
        : RenderGraphPass(name, type, RGPassCategory::None), setup_(setup), execute_(execute) {}

    void Setup(RenderGraphBuilder& builder) {
        setup_(data_, builder);
    }

    void Execute(RenderGraphContext& context) override {
        execute_(data_, context);
    }

    const Data& GetData() const { return data_; }

private:
    Data data_;
    SetupFunc setup_;
    ExecuteFunc execute_;
};

} // namespace primal::graphics::rendergraph
