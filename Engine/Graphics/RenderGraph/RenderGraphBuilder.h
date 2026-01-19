#pragma once

#include "RenderGraphDefinitions.h"
#include "RenderGraphResource.h"
#include "RenderGraphPass.h"

namespace primal::graphics::rendergraph {

class RenderGraph;

/**
 * @brief 渲染图构建器
 * @details 用于在 Setup 阶段声明 Pass 的资源依赖
 */
class RenderGraphBuilder {
public:
    RenderGraphBuilder(RenderGraph& graph, RenderGraphPass* pass)
        : graph_(graph), pass_(pass) {}

    // 声明读取资源
    RGResourceHandle Read(RGResourceHandle handle, rhi::ResourceState state = rhi::ResourceState::ShaderResource);

    // 声明写入资源
    RGResourceHandle Write(RGResourceHandle handle, rhi::ResourceState state = rhi::ResourceState::RenderTarget);

    // 创建临时纹理
    RGResourceHandle CreateTexture(const std::string& name, const rhi::TextureDesc& desc);

    // 创建临时缓冲区
    RGResourceHandle CreateBuffer(const std::string& name, const rhi::BufferDesc& desc);

    // 标记 Pass 有副作用 (防止被剔除)
    void SideEffect();

private:
    RenderGraph& graph_;
    RenderGraphPass* pass_;
};

} // namespace primal::graphics::rendergraph
