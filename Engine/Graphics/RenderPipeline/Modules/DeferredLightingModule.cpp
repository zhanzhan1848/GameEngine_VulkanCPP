#include "DeferredLightingModule.h"
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"

namespace primal::graphics {

using namespace rhi;
using namespace rhi::math;

// SceneData struct — MUST match Metal DeferredLighting.metal SceneData layout exactly
struct SceneData {
    math::m4x4 model;           // offset 0
    math::v4 lightPos;          // offset 64
    math::v4 lightColor;        // offset 80
    math::v4 reflectionPlane;   // offset 96 (cascadeSplits)
    math::v4 reflectionPlane2;  // offset 112
    math::v4 reflectionPlane3;  // offset 128
    math::m4x4 previousModel;   // offset 144
    math::v4 viewPos;           // offset 208
    math::m4x4 shadowMatrix0;   // offset 224
    math::m4x4 shadowMatrix1;   // offset 288
    math::v2 jitter;            // offset 352
    math::v2 previousJitter;    // offset 360
    math::v2 padding;           // offset 368
};

// ViewData struct
struct ViewData {
    math::m4x4 viewProjection;
    math::m4x4 invViewProjection;
};

// Descriptor update helper
struct DescData {
    u32 binding;
    DescriptorType type;
    ResourceHandle resource;
    u32 count = 1;
};

static void UpdateDesc(RHIDeviceBase* device, DescriptorSetHandle set, const DescData* params, u32 count) {
    std::vector<WriteDescriptorSet> writes(count);
    std::vector<DescriptorImageInfo> imageInfos(count);
    std::vector<DescriptorBufferInfo> bufferInfos(count);

    for (u32 i = 0; i < count; ++i) {
        writes[i].dstSet = set;
        writes[i].dstBinding = params[i].binding;
        writes[i].descriptorCount = params[i].count;
        writes[i].descriptorType = params[i].type;

        if (params[i].type == DescriptorType::UniformBuffer || params[i].type == DescriptorType::StorageBuffer) {
            bufferInfos[i].buffer = params[i].resource;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = ~0ull;
            writes[i].bufferInfo = &bufferInfos[i];
        } else if (params[i].type == DescriptorType::SampledImage || params[i].type == DescriptorType::StorageImage) {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        } else if (params[i].type == DescriptorType::Sampler) {
            imageInfos[i].sampler = static_cast<SamplerHandle>(params[i].resource);
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

bool DeferredLightingModule::Initialize(RHIDeviceBase* device,
                                         ShaderHandle vertex_shader, ShaderHandle pixel_shader,
                                         u32 render_width, u32 render_height) {
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;

    // Descriptor set layout: matches fragmentLighting_gpuDriven shader signature
    // buffer(0)=ViewData, buffer(1)=SceneData, texture(2-6,9), sampler(8)
    DescriptorSetLayoutBinding bindings[] = {
        {0, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel | ShaderStage::Vertex, nullptr},
        {1, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr},
        {2, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
        {3, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
        {4, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
        {5, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
        {6, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
        {8, DescriptorType::Sampler,       1, ShaderStage::Pixel, nullptr},
        {9, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
    };
    set_layout_ = device->CreateDescriptorSetLayout({9, bindings});
    layout_ = device->CreatePipelineLayout({1, &set_layout_});

    // Triple-buffered output textures (RGBA16_Float for HDR)
    // T4.6.5 part 24.3 (B5 fix): CopySource required because the output is
    // blitted FROM into ColorHistoryManager each frame
    // (ColorHistoryManager::CopyColorTexture at line 225). Without
    // TRANSFER_SRC_BIT, vkCmdBlitImage triggers
    // VUID-vkCmdBlitImage-srcImage-00219.
    TextureDesc outputDesc{};
    outputDesc.size = {render_width, render_height, 1};
    outputDesc.format = DataFormat::RGBA16_Float;
    outputDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::CopySource;
    outputDesc.memoryUsage = GPUMemoryUsage::Static;
    for (int i = 0; i < 3; ++i)
        output_textures_[i] = device->CreateTexture(outputDesc);

    // Triple-buffered constant buffers
    // T4.6.5 part 24.4 (B1 fix): type=Constant required so VulkanBuffer
    // translates to VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT. Without it, the
    // default BufferType falls through the switch in BufferDescToVkUsage
    // and the buffer only gets TRANSFER_SRC|DST_BIT — vkUpdateDescriptorSets
    // rejects it for UNIFORM_BUFFER descriptor (VUID-...-00330).
    for (int i = 0; i < 3; ++i) {
        BufferDesc viewCbDesc{};
        viewCbDesc.size = sizeof(ViewData);
        viewCbDesc.type = BufferType::Constant;
        viewCbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        view_cb_[i] = device->CreateBuffer(viewCbDesc);

        BufferDesc sceneCbDesc{};
        sceneCbDesc.size = sizeof(SceneData);
        sceneCbDesc.type = BufferType::Constant;
        sceneCbDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        scene_cb_[i] = device->CreateBuffer(sceneCbDesc);

        descriptor_sets_[i] = device->CreateDescriptorSet({set_layout_});
    }

    // Sampler
    {
        SamplerDesc desc{};
        desc.minFilter = FilterMode::Linear;
        desc.magFilter = FilterMode::Linear;
        desc.mipFilter = FilterMode::Linear;
        desc.addressU = TextureAddressMode::Clamp;
        desc.addressV = TextureAddressMode::Clamp;
        desc.addressW = TextureAddressMode::Clamp;
        sampler_ = device->CreateSampler(desc);
    }

    // T4.6.5 part 24.4 (B2 fix): 1x1 white fallback texture.
    // T4.6.5 part 24.9 (B7 fix): on Vulkan, initialize the texture with a
    // 4-byte staging copy + barrier to ShaderResource. Without this, the
    // texture stays in UNDEFINED layout and descriptor writes (which hardcode
    // SHADER_READ_ONLY_OPTIMAL) trigger VUID-vkCmdDraw-None-09600 when the
    // bound descriptor is read at the deferred lighting draw.
    TextureDesc fallbackDesc{};
    fallbackDesc.size = {1, 1, 1};
    fallbackDesc.format = DataFormat::RGBA8_UNorm;
    fallbackDesc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    fallbackDesc.memoryUsage = GPUMemoryUsage::Static;
    fallback_tex_ = device->CreateTexture(fallbackDesc);

    if (fallback_tex_ != handles::INVALID_RESOURCE &&
        device->GetPlatform() == RHIPlatform::Vulkan) {
        u8 white_pixel[4] = { 255, 255, 255, 255 };
        BufferDesc staging{};
        staging.size = 4;
        staging.memoryUsage = GPUMemoryUsage::Dynamic;
        ResourceHandle stagingHandle = device->CreateBuffer(staging);
        if (stagingHandle != handles::INVALID_RESOURCE) {
            void* mapped = device->MapBuffer(stagingHandle);
            if (mapped) {
                std::memcpy(mapped, white_pixel, 4);
                device->UnmapBuffer(stagingHandle);
            }
            SyncHandle fence = device->CreateSync();
            auto cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
            auto* cmd = GetCommandBuffer(cmdHandle);
            if (cmd && cmd->Begin()) {
                BufferTextureCopyRegion region{};
                region.bufferOffset = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = 1;
                region.imageOffset = {0, 0, 0};
                region.imageExtent = {1, 1, 1};
                cmd->CopyBufferToTexture(stagingHandle, fallback_tex_, &region, 1);

                ResourceBarrier toSRV;
                toSRV.resource = fallback_tex_;
                toSRV.beforeState = ResourceState::CopyDest;
                toSRV.afterState = ResourceState::ShaderResource;
                toSRV.subresource = RHI_ALL_SUBRESOURCES;
                toSRV.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&toSRV, 1);

                cmd->End();
                QueueSubmitInfo submitInfo{};
                submitInfo.cmdBuffer = cmdHandle;
                submitInfo.signalFence = fence;
                device->Submit(submitInfo);
                device->WaitForSync(fence, UINT32_MAX);
            }
            device->DestroySync(fence);
            device->DestroyCommandBuffer(cmdHandle);
            device->DestroyBuffer(stagingHandle);
        }
    }

    // Graphics pipeline
    if (vertex_shader != handles::INVALID_SHADER && pixel_shader != handles::INVALID_SHADER) {
        GraphicsPipelineDesc pd{};
        pd.layout = layout_;
        pd.vertexShader = vertex_shader;
        pd.pixelShader = pixel_shader;
        pd.renderTargetFormats[0] = DataFormat::RGBA16_Float;
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

void DeferredLightingModule::Shutdown() {
    // Resources are owned by the device; nothing else to clean up
}

ResourceHandle DeferredLightingModule::GetOutputTexture(u32 buffer_index) const {
    return (buffer_index < 3) ? output_textures_[buffer_index] : handles::INVALID_RESOURCE;
}

DeferredLightingOutputs DeferredLightingModule::AddPasses(rendergraph::RenderGraph& graph,
                                                           const DeferredLightingInputs& inputs) {
    DeferredLightingOutputs outputs{};

    u32 cbIdx = inputs.current_buffer_index % 3;
    auto currentTex = output_textures_[cbIdx];
    if (pipeline_ == handles::INVALID_PIPELINE || currentTex == handles::INVALID_RESOURCE)
        return outputs;

    auto deferredRG = graph.ImportResource("DeferredOutput", currentTex);
    outputs.deferred_output_rg = deferredRG;
    outputs.deferred_output_tex = currentTex;

    struct PassData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<PassData>("DeferredLighting",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::Lighting,
        [deferredRG, visRG = inputs.shadow_visibility_rg,
             albedoRG = inputs.gbuffer_albedo_rg,
             normalRG = inputs.gbuffer_normal_rg,
             ormRG = inputs.gbuffer_orm_rg,
             depthRG = inputs.gbuffer_depth_rg](PassData& data, rendergraph::RenderGraphBuilder& builder) {
            data.output = builder.Write(deferredRG, ResourceState::RenderTarget);

            if (visRG.IsValid())
                builder.Read(visRG, ResourceState::ShaderResource);

            // Declare GBuffer texture reads so the render graph inserts
            // RenderTarget → ShaderResource barriers before this pass
            if (albedoRG.IsValid())
                builder.Read(albedoRG, ResourceState::ShaderResource);
            if (normalRG.IsValid())
                builder.Read(normalRG, ResourceState::ShaderResource);
            if (ormRG.IsValid())
                builder.Read(ormRG, ResourceState::ShaderResource);
            if (depthRG.IsValid())
                builder.Read(depthRG, ResourceState::ShaderResource);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({
                .texture = data.output,
                .loadOp = LoadAction::DontCare,
                .storeOp = StoreAction::Store,
                .clearColor = { math::v4{0, 0, 0, 1} }
            });
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs, cbIdx](const PassData& data, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;

            // Upload ViewData
            {
                math::m4x4 vp = inputs.proj_matrix * inputs.view_matrix;
                auto* vd = static_cast<ViewData*>(device_->MapBuffer(view_cb_[cbIdx]));
                if (vd) {
                    vd->viewProjection = vp;
                    vd->invViewProjection = Inverse(vp);
                    device_->UnmapBuffer(view_cb_[cbIdx]);
                }
            }

            // Upload SceneData
            {
                auto* sd = static_cast<SceneData*>(device_->MapBuffer(scene_cb_[cbIdx]));
                if (sd) {
                    memset(sd, 0, sizeof(SceneData));
                    sd->lightPos = inputs.light_pos;
                    sd->lightColor = inputs.light_color;
                    sd->reflectionPlane = inputs.cascade_splits;
                    sd->viewPos = math::v4{inputs.camera_position.x, inputs.camera_position.y, inputs.camera_position.z, 1.0f};
                    sd->shadowMatrix0 = inputs.shadow_matrix0;
                    sd->shadowMatrix1 = inputs.shadow_matrix1;
                    device_->UnmapBuffer(scene_cb_[cbIdx]);
                }
            }

            // Bind resources
            auto depthSampleable = inputs.gpu_draw_pipeline->GetGBufferDepthSampleable();
            // T4.6.5 part 24.4 (B2 fix): use fallback_tex_ for invalid bindings
            // so descriptor has a valid imageView. SSAO/shadow_visibility may
            // be INVALID when those features aren't enabled.
            auto validOrFallback = [](ResourceHandle h, ResourceHandle fb) {
                return h != handles::INVALID_RESOURCE ? h : fb;
            };

            ResourceHandle albedoTex = validOrFallback(inputs.gpu_draw_pipeline->GetGBufferAlbedo(), fallback_tex_);
            ResourceHandle normalTex = validOrFallback(inputs.gpu_draw_pipeline->GetGBufferNormal(), fallback_tex_);
            ResourceHandle ormTex = validOrFallback(inputs.gpu_draw_pipeline->GetGBufferORM(), fallback_tex_);
            ResourceHandle depthTex = validOrFallback(depthSampleable, fallback_tex_);
            ResourceHandle shadowVisTex = validOrFallback(inputs.shadow_visibility_tex, fallback_tex_);

            DescData params[] = {
                {0, DescriptorType::UniformBuffer, view_cb_[cbIdx]},
                {1, DescriptorType::UniformBuffer, scene_cb_[cbIdx]},
                {2, DescriptorType::SampledImage, albedoTex},
                {3, DescriptorType::SampledImage, normalTex},
                {4, DescriptorType::SampledImage, ormTex},
                {5, DescriptorType::SampledImage, depthTex},
                {6, DescriptorType::SampledImage, shadowVisTex},
                {8, DescriptorType::Sampler, static_cast<ResourceHandle>(sampler_)},
                {9, DescriptorType::SampledImage, fallback_tex_},
            };
            UpdateDesc(device_, descriptor_sets_[cbIdx], params, 9);

            cmd->SetViewport({{0, 0}, {static_cast<float>(render_width_), static_cast<float>(render_height_)}, 0, 1});
            cmd->SetScissor({{0, 0}, {render_width_, render_height_}});

            cmd->BindGraphicsPipeline(pipeline_);
            const DescriptorSetHandle sets[] = { descriptor_sets_[cbIdx] };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        }
    );

    return outputs;
}

} // namespace primal::graphics
