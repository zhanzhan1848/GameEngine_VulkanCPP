#include "FinalBlitModule.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

namespace primal::graphics {

using namespace rhi;

struct DescData {
    u32 binding;
    DescriptorType type;
    ResourceHandle resource;
    SamplerHandle sampler;
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
        // T4.6.5 part 24.10: Sampler descriptors only populate the sampler
        // field; SampledImage descriptors populate imageView + imageLayout.
        if (params[i].type == DescriptorType::Sampler) {
            imageInfos[i].sampler = params[i].sampler;
            imageInfos[i].imageView = handles::INVALID_RESOURCE;
            imageInfos[i].imageLayout = ResourceState::Unknown;
        } else {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
        }
        writes[i].imageInfo = &imageInfos[i];
    }
    device->UpdateDescriptorSets(count, writes.data());
}

bool FinalBlitModule::Initialize(RHIDeviceBase* device,
                                  ShaderHandle vertex_shader, ShaderHandle pixel_shader) {
    device_ = device;

    // T4.6.5 part 24.10: Vulkan needs both SampledImage + Sampler descriptors
    // to satisfy Blit.frag's combined sampler2D(inputTex, inputSamp) idiom.
    // Metal has implicit default samplers, so the extra binding is unused there.
    DescriptorSetLayoutBinding bindings[] = {
        {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},
        {1, DescriptorType::Sampler,      1, ShaderStage::Pixel, nullptr}
    };
    set_layout_ = device->CreateDescriptorSetLayout({2, bindings});
    layout_ = device->CreatePipelineLayout({1, &set_layout_});

    for (int i = 0; i < 3; ++i)
        descriptor_sets_[i] = device->CreateDescriptorSet({set_layout_});

    SamplerDesc samplerDesc{};
    samplerDesc.minFilter = FilterMode::Linear;
    samplerDesc.magFilter = FilterMode::Linear;
    samplerDesc.mipFilter = FilterMode::Linear;
    samplerDesc.addressU = TextureAddressMode::Clamp;
    samplerDesc.addressV = TextureAddressMode::Clamp;
    samplerDesc.addressW = TextureAddressMode::Clamp;
    samplerDesc.comparisonFunc = ComparisonFunc::Never;
    default_sampler_ = device->CreateSampler(samplerDesc);

    if (vertex_shader != handles::INVALID_SHADER && pixel_shader != handles::INVALID_SHADER) {
        GraphicsPipelineDesc pd{};
        pd.layout = layout_;
        pd.vertexShader = vertex_shader;
        pd.pixelShader = pixel_shader;
        pd.renderTargetFormats[0] = DataFormat::BGRA8_UNorm;
        pd.renderTargetCount = 1;
        pd.depthStencilFormat = DataFormat::Unknown;
        pd.enableDepthTest = false;
        pd.enableDepthWrite = false;
        pd.cullMode = CullMode::None;
        pd.vertexAttributes.clear();
        pd.vertexBindings.clear();
        pipeline_ = device->CreateGraphicsPipeline(pd);
    }

    return pipeline_ != handles::INVALID_PIPELINE;
}

void FinalBlitModule::Shutdown() {
    if (device_ == nullptr) return;
    if (default_sampler_ != handles::INVALID_SAMPLER) {
        device_->DestroySampler(default_sampler_);
        default_sampler_ = handles::INVALID_SAMPLER;
    }
}

void FinalBlitModule::AddPass(rendergraph::RenderGraph& graph, const FinalBlitInputs& inputs) {
    if (pipeline_ == handles::INVALID_PIPELINE) return;

    struct BlitData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<BlitData>("FinalBlit",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [inputs](BlitData& data, rendergraph::RenderGraphBuilder& builder) {
            if (inputs.input_rg.IsValid())
                builder.Read(inputs.input_rg, ResourceState::ShaderResource);
            data.output = builder.Write(inputs.backbuffer_rg, ResourceState::RenderTarget);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({
                .texture = data.output,
                .loadOp = LoadAction::DontCare,
                .storeOp = StoreAction::Store,
                .clearColor = { math::v4{0, 0, 0, 1} }
            });
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs](const BlitData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            u32 cbIdx = inputs.current_buffer_index % 3;

            DescData params[] = {
                {0, DescriptorType::SampledImage, inputs.input_tex, handles::INVALID_SAMPLER},
                {1, DescriptorType::Sampler,      handles::INVALID_RESOURCE, default_sampler_},
            };
            UpdateDesc(device_, descriptor_sets_[cbIdx], params, 2);

            cmd->SetViewport({{0, 0}, {static_cast<float>(inputs.render_width), static_cast<float>(inputs.render_height)}, 0, 1});
            cmd->SetScissor({{0, 0}, {inputs.render_width, inputs.render_height}});

            cmd->BindGraphicsPipeline(pipeline_);
            const DescriptorSetHandle sets[] = { descriptor_sets_[cbIdx] };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        }
    );
}

} // namespace primal::graphics
