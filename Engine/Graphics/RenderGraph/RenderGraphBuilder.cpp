#include "RenderGraphBuilder.h"
#include "RenderGraph.h"

namespace primal::graphics::rendergraph {

RGResourceHandle RenderGraphBuilder::Read(RGResourceHandle handle, rhi::ResourceState state) {
    if (handle.IsValid()) {
        graph_.RegisterResourceRead(pass_, handle, state);
    }
    return handle;
}

RGResourceHandle RenderGraphBuilder::Write(RGResourceHandle handle, rhi::ResourceState state) {
    if (handle.IsValid()) {
        graph_.RegisterResourceWrite(pass_, handle, state);
        
        // 写入操作会产生一个新的版本 (简化起见，这里先不处理多版本，假设 Handle 唯一标识资源)
        // 在更复杂的实现中，这里可能返回一个新的 Handle 代表新版本的资源
    }
    return handle;
}

RGResourceHandle RenderGraphBuilder::CreateTexture(const std::string& name, const rhi::TextureDesc& desc, rhi::ResourceState state) {
    RGResourceHandle handle = graph_.CreateTexture(name, desc);
    // 创建的资源通常会被当前 Pass 写入
    graph_.RegisterResourceWrite(pass_, handle, state);
    return handle;
}

RGResourceHandle RenderGraphBuilder::CreateBuffer(const std::string& name, const rhi::BufferDesc& desc, rhi::ResourceState state) {
    RGResourceHandle handle = graph_.CreateBuffer(name, desc);
    // TODO: Determine default state for buffer (e.g. UnorderedAccess or CopyDest)
    graph_.RegisterResourceWrite(pass_, handle, state); 
    return handle;
}

void RenderGraphBuilder::DeclareRenderPass(const RGRenderPassDesc& desc) {
    pass_->SetRenderPassDesc(desc);
    
    // 自动注册 RenderTarget 写入依赖
    for (const auto& att : desc.colors) {
        if (att.texture.IsValid()) {
            graph_.RegisterResourceWrite(pass_, att.texture, rhi::ResourceState::RenderTarget);
        }
    }
    
    if (desc.depthStencil.texture.IsValid()) {
        graph_.RegisterResourceWrite(pass_, desc.depthStencil.texture, rhi::ResourceState::DepthStencil);
    }
}

void RenderGraphBuilder::SideEffect() {
    pass_->SetSideEffect(true);
}

} // namespace primal::graphics::rendergraph
