#include "ForwardPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/ForwardRenderer.h"
#include "Graphics/MaterialInstance.h"
#include "Graphics/RHI/Core/RHICommand.h"

namespace primal::graphics::ForwardPass {

struct ForwardPassData {
    ForwardPassOutput output;
    ForwardRenderer* renderer;
    RenderScene* scene;
    RenderView* view;
    const std::unordered_map<id::id_type, std::shared_ptr<MaterialInstance>>* materials;
    u32 frameIndex;
    u32 width;
    u32 height;
};

const ForwardPassOutput& AddPass(
    RenderGraph& graph,
    RenderScene& scene,
    RenderView& view,
    ForwardRenderer& renderer,
    const std::unordered_map<id::id_type, std::shared_ptr<MaterialInstance>>& materials,
    u32 frameIndex,
    u32 width,
    u32 height) {

    return graph.AddPass<ForwardPassData>("ForwardPass", RGPassType::Graphics, RGPassCategory::Main,
        [&](ForwardPassData& data, RenderGraphBuilder& builder) {
            data.renderer = &renderer;
            data.scene = &scene;
            data.view = &view;
            data.materials = &materials;
            data.frameIndex = frameIndex;
            data.width = width;
            data.height = height;

            // Create HDR render target (RGBA16_Float for HDR pipeline)
            rhi::TextureDesc hdrDesc;
            hdrDesc.size = {width, height, 1};
            hdrDesc.format = rhi::DataFormat::RGBA16_Float;
            hdrDesc.type = rhi::TextureType::Texture2D;
            hdrDesc.mipLevels = 1;
            hdrDesc.usage = rhi::TextureUsage::RenderTarget | rhi::TextureUsage::ShaderResource;
            data.output.hdrTexture = builder.CreateTexture("HDR_Scene", hdrDesc, rhi::ResourceState::RenderTarget);

            // Create depth buffer
            rhi::TextureDesc depthDesc;
            depthDesc.size = {width, height, 1};
            depthDesc.format = rhi::DataFormat::D32_Float;
            depthDesc.type = rhi::TextureType::Texture2D;
            depthDesc.mipLevels = 1;
            depthDesc.usage = rhi::TextureUsage::DepthStencil;
            data.output.depthTexture = builder.CreateTexture("ForwardDepth", depthDesc, rhi::ResourceState::DepthStencil);
        },
        [](const ForwardPassData& data, RenderGraphContext& context) {
            if (!data.renderer || !data.scene || !data.view) return;

            auto* hdrRes = context.graph->GetResource(data.output.hdrTexture);
            auto* depthRes = context.graph->GetResource(data.output.depthTexture);
            if (!hdrRes || !depthRes) return;

            data.renderer->Render(
                context.cmdBuffer,
                *data.scene,
                *data.view,
                hdrRes->GetPhysicalHandle(),
                depthRes->GetPhysicalHandle(),
                *data.materials,
                data.frameIndex,
                data.width,
                data.height
            );
        }
    ).output;
}

} // namespace primal::graphics::ForwardPass
