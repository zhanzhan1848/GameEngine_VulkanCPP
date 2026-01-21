#include "VisibilityPass.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHIDevice.h"

namespace primal::graphics::Visibility {

using namespace rendergraph;
using namespace rhi;

static PipelineHandle s_VisibilityPipeline = handles::INVALID_PIPELINE;

const VisibilityPassData& AddVisibilityPass(RenderGraph& graph) {
    // 1. 初始化 Pipeline (如果尚未初始化)
    if (s_VisibilityPipeline == handles::INVALID_PIPELINE) {
        // 使用 graph 获取 Device
        // 注意：这里假设 s_VisibilityPipeline 只会被初始化一次，且 RenderGraph 的 Device 在整个生命周期内有效
        // 在多线程或多 RenderGraph 实例情况下，可能需要更完善的 PipelineCache
        auto& device = graph.GetDevice();
        
        GraphicsPipelineDesc desc;
        // TODO: Load shader properly
        // desc.vs = { "visibility_vs", "main" }; 
        // desc.fs = { "visibility_fs", "main" };
        
        // Visibility Buffer Format: R32_UINT
        desc.renderTargetFormats[0] = DataFormat::R32_UInt;
        desc.depthStencilFormat = DataFormat::D32_Float;
        desc.enableDepthWrite = true;
        desc.depthFunc = ComparisonFunc::Less;
        desc.renderTargetCount = 1;
        
        // s_VisibilityPipeline = device.CreateGraphicsPipeline(desc);
        // Temporary: keep nullptr until shader loading is implemented
    }

    // 2. 添加 Pass
    return graph.AddPass<VisibilityPassData>("VisibilityPass", RGPassType::Graphics, RGPassCategory::Visibility,
        [&](VisibilityPassData& data, RenderGraphBuilder& builder) {
            // 创建 Visibility Buffer (R32_UINT)
            TextureDesc visDesc;
            visDesc.size.x = 0; // 0 表示跟随 SwapChain 大小
            visDesc.size.y = 0;
            visDesc.size.z = 1;
            visDesc.format = DataFormat::R32_UInt;
            visDesc.type = TextureType::Texture2D;
            visDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
            
            data.visibilityBuffer = builder.CreateTexture("VisibilityBuffer", visDesc);
            
            // 创建 Depth Buffer
            TextureDesc depthDesc;
            depthDesc.size.x = 0;
            depthDesc.size.y = 0;
            depthDesc.size.z = 1;
            depthDesc.format = DataFormat::D32_Float;
            depthDesc.type = TextureType::Texture2D;
            depthDesc.usage = TextureUsage::DepthStencil | TextureUsage::ShaderResource;
            
            RGResourceHandle depthBuffer = builder.CreateTexture("SceneDepth", depthDesc);
            
            // 设置 Render Target
            RGRenderPassDesc passDesc;
            passDesc.colors.resize(1);
            passDesc.colors[0].texture = data.visibilityBuffer;
            passDesc.colors[0].loadOp = LoadAction::Clear;
            passDesc.colors[0].storeOp = StoreAction::Store;
            passDesc.colors[0].clearColor = { 0.0f, 0.0f, 0.0f, 0.0f }; // Clear ID to 0
            
            passDesc.depthStencil.texture = depthBuffer;
            passDesc.depthStencil.depthLoadOp = LoadAction::Clear;
            passDesc.depthStencil.depthStoreOp = StoreAction::Store;
            passDesc.depthStencil.clearDepth = 1.0f;
            
            builder.DeclareRenderPass(passDesc);
        },
        [&](const VisibilityPassData& data, RenderGraphContext& context) {
            if (s_VisibilityPipeline != handles::INVALID_PIPELINE) {
                context.cmdBuffer->BindGraphicsPipeline(s_VisibilityPipeline);
                
                // TODO: 绑定场景数据 (Camera, Instances)
                // TODO: 执行 Draw Call (DrawIndirect 或遍历 Scene)
                // context.cmdBuffer->DrawIndexed(...)
            }
        }
    );
}

} // namespace primal::graphics::Visibility
