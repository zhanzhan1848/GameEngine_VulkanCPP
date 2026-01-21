#include "DebugPass.h"
#include "Graphics/RenderGraph/RenderGraphDebug.h"

namespace primal::graphics {

static std::unique_ptr<rendergraph::RenderGraphDebug> s_DebugRenderer;
static rhi::RHIDeviceBase* s_Device = nullptr;

// Ensure we cleanup static resources before device shutdown
struct DebugRendererCleaner {
    ~DebugRendererCleaner() {
        if (s_DebugRenderer) {
            s_DebugRenderer.reset();
        }
    }
};
static DebugRendererCleaner s_Cleaner;

const DebugPassData& AddDebugPass(rendergraph::RenderGraph& graph, rendergraph::RGResourceHandle target, const std::vector<DebugResource>& debugResources) {
    if (s_Device != &graph.GetDevice()) {
        s_DebugRenderer.reset(); // Reset if device changes
        s_Device = &graph.GetDevice();
    }

    if (!s_DebugRenderer) {
        s_DebugRenderer = std::make_unique<rendergraph::RenderGraphDebug>(graph.GetDevice());
        s_DebugRenderer->ToggleEnabled(); // Enable by default for demo
    }

    // Input system needs explicit update call from main loop, but we can't do it here easily.
    // However, RenderGraphDebug::Update is called in execute lambda.
    
    return graph.AddPass<DebugPassData>("DebugOverlay", rendergraph::RGPassType::Graphics, rendergraph::RGPassCategory::UI,
        [&](DebugPassData& data, rendergraph::RenderGraphBuilder& builder) {
            data.target = target;
            data.debugResources = debugResources;
            
            rendergraph::RGRenderPassDesc desc;
            desc.colors.resize(1);
            desc.colors[0].texture = target;
            desc.colors[0].loadOp = rhi::LoadAction::Load;
            desc.colors[0].storeOp = rhi::StoreAction::Store;
            
            // Assume target is already transitioned to RenderTarget by previous pass if we just Load?
            // No, builder needs to know we use it.
            builder.Write(target); // Register write access (even if we just Load, we are outputting to it)
            
            // Register debug resources
            for (const auto& res : debugResources) {
                builder.Read(res.handle, rhi::ResourceState::ShaderResource);
            }

            builder.DeclareRenderPass(desc);
        },
        [&](const DebugPassData& data, rendergraph::RenderGraphContext& context) {
            s_DebugRenderer->Update(0.016f); // Mock delta time
            
            uint32_t width = 1920;
            uint32_t height = 1080;
            
            auto* resource = context.graph->GetResource(data.target);
            if (resource) {
                // Try to cast to texture to get size
                if (resource->GetType() == rendergraph::RGResourceType::Texture) {
                    auto* tex = static_cast<rendergraph::RenderGraphTexture*>(resource);
                    width = tex->GetDesc().size.x;
                    height = tex->GetDesc().size.y;
                }
            }

            // Prepare debug resources
            std::vector<std::pair<std::string, rendergraph::RenderGraphResource*>> resolvedResources;
            for (const auto& res : data.debugResources) {
                resolvedResources.push_back({res.name, context.graph->GetResource(res.handle)});
            }
            s_DebugRenderer->SetDebugResources(resolvedResources);

            s_DebugRenderer->Draw(context.cmdBuffer, *context.graph, width, height);
        }
    );
}

void ShutdownDebugPass() {
    if (s_DebugRenderer) {
        s_DebugRenderer.reset();
    }
    s_Device = nullptr;
}

}
