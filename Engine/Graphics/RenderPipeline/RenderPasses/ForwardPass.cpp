#include "ForwardPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RenderScene.h"
#include "Graphics/RenderView.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include <iostream>

namespace primal::graphics::ForwardPass {

struct ForwardPassData {
    RGResourceHandle renderTarget;
};

RGResourceHandle AddPass(RenderGraph& graph, RenderScene& scene, RenderView& view, RGResourceHandle renderTarget) {
    return graph.AddPass<ForwardPassData>("ForwardPass", RGPassType::Graphics,
        [&](ForwardPassData& data, RenderGraphBuilder& builder) {
            data.renderTarget = renderTarget;
            
            // Write to RenderTarget
            builder.Write(data.renderTarget, rhi::ResourceState::RenderTarget);
            
            // Since we are writing to an imported backbuffer (usually), 
            // we need to make sure this pass is not culled if it's the final output.
            // If the resource is marked as Output, the pass writing to it is kept.
            // If not, we might need SideEffect.
            // For safety in this simple implementation, we mark SideEffect.
            builder.SideEffect(); 
        },
        [&](const ForwardPassData& data, RenderGraphContext& context) {
            auto* cmd = context.cmdBuffer;
            auto* rtResource = context.graph->GetResource(data.renderTarget);
            
            if (!rtResource) return;
            
            // Setup RenderPass Descriptor
            rhi::RenderPassDesc passDesc;
            passDesc.colorAttachments.resize(1);
            passDesc.colorAttachments[0].texture = rtResource->GetPhysicalHandle();
            passDesc.colorAttachments[0].loadOp = rhi::LoadAction::Clear;
            passDesc.colorAttachments[0].storeOp = rhi::StoreAction::Store;
            passDesc.colorAttachments[0].clearValue = {0.1f, 0.1f, 0.1f, 1.0f}; // Dark gray background
            
            passDesc.viewport = view.GetViewport();
            passDesc.scissor = view.GetScissor();
            
            // Begin Pass
            // std::cout << "BeginRenderPass..." << std::endl;
            cmd->BeginRenderPass(passDesc);
            
            // Render Scene
            // For this iteration, we just clear the screen (which BeginRenderPass does).
            // Actual object drawing requires Material/Pipeline binding which is complex 
            // and depends on the Material System integration which is separate.
            // We will iterate proxies just to show where it happens.
            
            const auto& visibleProxies = view.GetVisibleProxies();
            if (!visibleProxies.empty()) {
                // Issue a dummy draw call to verify stats
                // std::cout << "Drawing dummy..." << std::endl;
                // cmd->Draw(3, 0, 1, 0); // Crashes without pipeline state
            }
            for (const auto* proxy : visibleProxies) {
                // Draw Proxy
                (void)proxy;
            }
            
            // std::cout << "EndRenderPass..." << std::endl;
            cmd->EndRenderPass();
        }
    ).renderTarget;
}

} // namespace primal::graphics::ForwardPass
