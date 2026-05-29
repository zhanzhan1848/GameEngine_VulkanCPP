#include "FusionCompositeModule.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

namespace primal::graphics {

using namespace rhi;

struct DescData {
    u32 binding;
    DescriptorType type;
    ResourceHandle resource;
    u32 count = 1;
};

static void UpdateDesc(RHIDeviceBase* device, DescriptorSetHandle set, const DescData* params, u32 count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);

    for (u32 i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = params[i].count;
        writes[i].descriptorType = params[i].type;
        imageInfos[i].imageView = params[i].resource;
        imageInfos[i].imageLayout = ResourceState::ShaderResource;
        writes[i].imageInfo = &imageInfos[i];
    }
    device->UpdateDescriptorSets(count, writes.data());
}

bool FusionCompositeModule::Initialize(RHIDeviceBase* device,
                                         ShaderHandle indirect_ps, ShaderHandle composite_vs,
                                         ShaderHandle composite_ps,
                                         u32 render_width, u32 render_height) {
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    vertex_shader_ = composite_vs;

    // --- Pass 1: FusionIndirect (half-res, 5 texture reads) ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},  // SSGI
            {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},  // DDGI
            {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},  // SPGI
            {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},  // albedo
            {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},  // SSAO
        };
        indirect_set_layout_ = device->CreateDescriptorSetLayout({5, bindings});
        indirect_layout_ = device->CreatePipelineLayout({1, &indirect_set_layout_});

        for (int i = 0; i < 3; ++i)
            indirect_ds_[i] = device->CreateDescriptorSet({indirect_set_layout_});

        // Half-res indirect output (RGBA16_Float)
        TextureDesc indDesc{};
        indDesc.size = {render_width / 2, render_height / 2, 1};
        indDesc.format = DataFormat::RGBA16_Float;
        indDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
        for (int i = 0; i < 3; ++i)
            indirect_output_[i] = device->CreateTexture(indDesc);

        if (composite_vs != handles::INVALID_SHADER && indirect_ps != handles::INVALID_SHADER) {
            GraphicsPipelineDesc pd{};
            pd.layout = indirect_layout_;
            pd.vertexShader = composite_vs;
            pd.pixelShader = indirect_ps;
            pd.renderTargetFormats[0] = DataFormat::RGBA16_Float;
            pd.renderTargetCount = 1;
            pd.depthStencilFormat = DataFormat::Unknown;
            pd.enableDepthTest = false;
            pd.enableDepthWrite = false;
            pd.cullMode = CullMode::None;
            pd.vertexAttributes.clear();
            pd.vertexBindings.clear();
            indirect_pipeline_ = device->CreateGraphicsPipeline(pd);
        }
    }

    // --- Pass 2: FusionComposite (full-res, 2 texture reads) ---
    {
        DescriptorSetLayoutBinding bindings[] = {
            {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},  // scene
            {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},  // indirect
        };
        composite_set_layout_ = device->CreateDescriptorSetLayout({2, bindings});
        composite_layout_ = device->CreatePipelineLayout({1, &composite_set_layout_});

        for (int i = 0; i < 3; ++i)
            composite_ds_[i] = device->CreateDescriptorSet({composite_set_layout_});

        // Full-res output (BGRA8_UNorm)
        TextureDesc outDesc{};
        outDesc.size = {render_width, render_height, 1};
        outDesc.format = DataFormat::BGRA8_UNorm;
        outDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
        for (int i = 0; i < 3; ++i)
            fusion_output_[i] = device->CreateTexture(outDesc);

        if (composite_vs != handles::INVALID_SHADER && composite_ps != handles::INVALID_SHADER) {
            GraphicsPipelineDesc pd{};
            pd.layout = composite_layout_;
            pd.vertexShader = composite_vs;
            pd.pixelShader = composite_ps;
            pd.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
            pd.renderTargetCount = 1;
            pd.depthStencilFormat = DataFormat::Unknown;
            pd.enableDepthTest = false;
            pd.enableDepthWrite = false;
            pd.cullMode = CullMode::None;
            pd.vertexAttributes.clear();
            pd.vertexBindings.clear();
            composite_pipeline_ = device->CreateGraphicsPipeline(pd);
        }
    }

    return indirect_pipeline_ != handles::INVALID_PIPELINE && composite_pipeline_ != handles::INVALID_PIPELINE;
}

void FusionCompositeModule::Shutdown() {}

FusionOutputs FusionCompositeModule::AddPasses(rendergraph::RenderGraph& graph, const FusionInputs& inputs) {
    FusionOutputs outputs{};
    u32 cbIdx = inputs.current_buffer_index % 3;

    if (indirect_pipeline_ == handles::INVALID_PIPELINE || composite_pipeline_ == handles::INVALID_PIPELINE)
        return outputs;

    // Import textures into render graph
    auto indirectRG = graph.ImportResource("FusionIndirect", indirect_output_[cbIdx]);
    auto fusionOutRG = graph.ImportResource("FusionOutput", fusion_output_[cbIdx]);

    outputs.output_rg = fusionOutRG;
    outputs.output_tex = fusion_output_[cbIdx];

    // Resolve helper
    auto resolve = [&](rendergraph::RGResourceHandle h) -> ResourceHandle {
        if (!h.IsValid()) return inputs.black_texture;
        auto* res = graph.GetResource(h);
        return res ? res->GetPhysicalHandle() : inputs.black_texture;
    };

    // --- Pass 1: FusionIndirect (half-res) ---
    struct IndirectData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<IndirectData>("FusionIndirect",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [inputs, indirectRG](IndirectData& data, rendergraph::RenderGraphBuilder& builder) {
            if (inputs.ssgi_rg.IsValid()) builder.Read(inputs.ssgi_rg, ResourceState::ShaderResource);
            if (inputs.ddgi_rg.IsValid()) builder.Read(inputs.ddgi_rg, ResourceState::ShaderResource);
            if (inputs.spgi_rg.IsValid()) builder.Read(inputs.spgi_rg, ResourceState::ShaderResource);
            if (inputs.ssao_rg.IsValid()) builder.Read(inputs.ssao_rg, ResourceState::ShaderResource);
            data.output = builder.Write(indirectRG, ResourceState::RenderTarget);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({.texture = data.output, .loadOp = LoadAction::DontCare, .storeOp = StoreAction::Store, .clearColor = {math::v4{0, 0, 0, 0}}});
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs, cbIdx, resolve](const IndirectData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            u32 halfW = render_width_ / 2;
            u32 halfH = render_height_ / 2;

            cmd->SetViewport({{0, 0}, {static_cast<float>(halfW), static_cast<float>(halfH)}, 0, 1});
            cmd->SetScissor({{0, 0}, {halfW, halfH}});

            DescData params[5] = {
                {0, DescriptorType::SampledImage, inputs.ssgi_tex != handles::INVALID_RESOURCE ? inputs.ssgi_tex : inputs.black_texture},
                {1, DescriptorType::SampledImage, inputs.ddgi_tex != handles::INVALID_RESOURCE ? inputs.ddgi_tex : inputs.black_texture},
                {2, DescriptorType::SampledImage, inputs.spgi_tex != handles::INVALID_RESOURCE ? inputs.spgi_tex : inputs.black_texture},
                {3, DescriptorType::SampledImage, inputs.gbuffer_albedo != handles::INVALID_RESOURCE ? inputs.gbuffer_albedo : inputs.black_texture},
                {4, DescriptorType::SampledImage, inputs.ssao_tex != handles::INVALID_RESOURCE ? inputs.ssao_tex : inputs.black_texture},
            };
            UpdateDesc(device_, indirect_ds_[cbIdx], params, 5);
            cmd->BindGraphicsPipeline(indirect_pipeline_);
            const DescriptorSetHandle sets[] = { indirect_ds_[cbIdx] };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, indirect_layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        }
    );

    // --- Pass 2: FusionComposite (full-res) ---
    struct ComposeData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<ComposeData>("FusionComposite",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [primaryRG = inputs.primary_input_rg, indirectRG, fusionOutRG](ComposeData& data, rendergraph::RenderGraphBuilder& builder) {
            if (primaryRG.IsValid()) builder.Read(primaryRG, ResourceState::ShaderResource);
            builder.Read(indirectRG, ResourceState::ShaderResource);
            data.output = builder.Write(fusionOutRG, ResourceState::RenderTarget);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({.texture = data.output, .loadOp = LoadAction::DontCare, .storeOp = StoreAction::Store, .clearColor = {math::v4{0, 0, 0, 1}}});
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs, cbIdx, indirectTex = indirect_output_[cbIdx]](const ComposeData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            cmd->SetViewport({{0, 0}, {static_cast<float>(render_width_), static_cast<float>(render_height_)}, 0, 1});
            cmd->SetScissor({{0, 0}, {render_width_, render_height_}});

            DescData params[2] = {
                {0, DescriptorType::SampledImage, inputs.primary_input_tex},
                {1, DescriptorType::SampledImage, indirectTex},
            };
            UpdateDesc(device_, composite_ds_[cbIdx], params, 2);
            cmd->BindGraphicsPipeline(composite_pipeline_);
            const DescriptorSetHandle sets[] = { composite_ds_[cbIdx] };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, composite_layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        }
    );

    return outputs;
}

} // namespace primal::graphics
