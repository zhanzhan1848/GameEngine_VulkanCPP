#include "DeferredLightingModule.h"
#include "Engine/Common/CoreLogStream.h"
#include "Graphics/Nanite/GPUDrivenDrawPipeline.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHIMath.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/Lumen/SSAO/LumenSSAOPass.h"

namespace primal::graphics {

using namespace rhi;
using namespace rhi::math;

// SceneData struct — MUST match Metal DeferredLighting.metal SceneData layout exactly.
// 2026-09-05：对齐引擎 shader 新布局（字段重排 + time/ambientLight，416B）。
// 旧 384B 布局下 viewPos/jitter/shadowMatrix 全部错位读取——直射、阴影、
// 高光整体漂移（Metal validation: "argument has a length(416)" 抓到）。
struct SceneData {
    math::m4x4 model;           // offset 0
    math::v4 lightPos;          // offset 64
    math::v4 lightColor;        // offset 80
    math::v4 reflectionPlane;   // offset 96 (cascadeSplits)
    math::v4 reflectionPlane2;  // offset 112
    math::v4 reflectionPlane3;  // offset 128
    math::m4x4 previousModel;   // offset 144
    math::v2 jitter;            // offset 208
    math::v2 previousJitter;    // offset 216
    float time;                 // offset 224
    float _timePad;             // offset 228
    math::v4 viewPos;           // offset 232
    math::m4x4 shadowMatrix0;   // offset 240
    math::m4x4 shadowMatrix1;   // offset 304
    math::m4x4 shadowMatrix2;   // offset 368 (P4.2: 级联 2)
    math::m4x4 shadowMatrix3;   // offset 432 (P4.2: 级联 3)
    // offset 496: x = shadow mode (0 = pre-filtered R8, 1 = VSM moments),
    // y = cascade 0 split distance (world units from camera), zw unused.
    math::v4 shadowParams{0.0f, 0.0f, 0.0f, 0.0f};
    // offset 512: rgb = scene ambient color, a = intensity. 0 = 无环境抬升
    // （与旧行为一致；接 ForwardSceneRenderer::ambient_light_ 待接线）。
    math::v4 ambientLight{0.0f, 0.0f, 0.0f, 0.0f};
};
static_assert(sizeof(SceneData) == 544, "SceneData must match DeferredLighting.frag (P4.2: 4 级联 544B)");

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
    // buffer(0)=ViewData, buffer(1)=SceneData, texture(2-6,9), sampler(8).
    // T4.6.5 part 37: bindings 10/11/12 are IBL resources (Tier 5 visual
    // fidelity). 10=irradianceMap (cube), 11=prefilterMap (cube),
    // 12=brdfLUT (2D). Descriptor type is SampledImage (combined with
    // sampler at binding 8 in the shader via samplerCube(...)/sampler2D(...)).
    // VSM: 13/14 = blurred RG32 shadow moments per cascade (Chebyshev
    // sampling in the shader; fall back to the 1x1 white texture on PCSS).
    DescriptorSetLayoutBinding bindings[] = {
        {0, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel | ShaderStage::Vertex, nullptr},
        {1, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr},
        {2, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
        {3, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
        {4, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
        {5, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
        {6, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
        {7, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},  // SSAO
        {8, DescriptorType::Sampler,       1, ShaderStage::Pixel, nullptr},
        {9, DescriptorType::SampledImage,  1, ShaderStage::Pixel, nullptr},
        {10, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},
        {11, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},
        {12, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},
        {13, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},
        {14, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},
        {15, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},  // P4.2 moments2
        {16, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},  // P4.2 moments3
        {17, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr},  // P4.1 translucent
        // sun-audit P2 第二步: DDGI 探针深度可见性（PRIMAL_PROBE_OCC 变体
        // 访问；其余变体不访问——绑定常驻有效：18=dummy SSBO、19=零参数）。
        {18, DescriptorType::StorageBuffer, 1, ShaderStage::Pixel, nullptr},  // 探针深度 SSBO
        {19, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr},  // 探针参数
    };
    set_layout_ = device->CreateDescriptorSetLayout({20, bindings});
    layout_ = device->CreatePipelineLayout({1, &set_layout_});

    // sun-audit P2 第二步: 探针参数 UBO（三缓冲）+ 1-float dummy SSBO
    //（探针不可用时占位——shader 以 counts=0 早退，不产生越界访问）。
    {
        BufferDesc ppDesc{};
        ppDesc.size = 32;  // 2 × vec4
        ppDesc.type = BufferType::Constant;
        ppDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        for (int i = 0; i < 3; ++i) probe_param_cb_[i] = device->CreateBuffer(ppDesc);

        BufferDesc dummyDesc{};
        dummyDesc.size = 4;
        dummyDesc.type = BufferType::Structured;
        dummyDesc.memoryUsage = GPUMemoryUsage::Dynamic;
        dummyDesc.bindFlags = static_cast<u32>(BufferUsageFlags::Storage);
        dummyDesc.structured.elementCount = 1;
        dummyDesc.structured.elementStride = sizeof(float);
        probe_depth_dummy_ = device->CreateBuffer(dummyDesc);
    }

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

void DeferredLightingModule::SetIBLResources(rhi::ResourceHandle irradiance,
                                             rhi::ResourceHandle prefilter,
                                             rhi::ResourceHandle brdfLUT) {
    ibl_irradiance_ = irradiance;
    ibl_prefilter_ = prefilter;
    ibl_brdf_lut_ = brdfLUT;
}

DeferredLightingOutputs DeferredLightingModule::AddPasses(rendergraph::RenderGraph& graph,
                                                           const DeferredLightingInputs& inputs) {
    DeferredLightingOutputs outputs{};

    u32 cbIdx = inputs.current_buffer_index % 3;
    auto currentTex = output_textures_[cbIdx];
    if (pipeline_ == handles::INVALID_PIPELINE || currentTex == handles::INVALID_RESOURCE)
        return outputs;

    // ImportTexture（非 ImportResource）：携带 desc、注册为 Texture 类型，
    // 下游按类型守卫解析 desc 才不会回退空 desc（同 FusionOutput 修复）。
    TextureDesc deferredOutDesc{};
    deferredOutDesc.size = {render_width_, render_height_, 1};
    deferredOutDesc.format = DataFormat::RGBA16_Float;
    deferredOutDesc.type = TextureType::Texture2D;
    deferredOutDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource | TextureUsage::CopySource;
    deferredOutDesc.memoryUsage = GPUMemoryUsage::Static;
    auto deferredRG = graph.ImportTexture("DeferredOutput", currentTex, deferredOutDesc);
    outputs.deferred_output_rg = deferredRG;
    outputs.deferred_output_tex = currentTex;

    struct PassData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<PassData>("DeferredLighting",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::Lighting,
        [deferredRG, visRG = inputs.shadow_visibility_rg,
             mom0RG = inputs.shadow_moments_rg[0],
             mom1RG = inputs.shadow_moments_rg[1],
             albedoRG = inputs.gbuffer_albedo_rg,
             normalRG = inputs.gbuffer_normal_rg,
             ormRG = inputs.gbuffer_orm_rg,
             depthRG = inputs.gbuffer_depth_rg](PassData& data, rendergraph::RenderGraphBuilder& builder) {
            data.output = builder.Write(deferredRG, ResourceState::RenderTarget);

            if (visRG.IsValid())
                builder.Read(visRG, ResourceState::ShaderResource);
            if (mom0RG.IsValid())
                builder.Read(mom0RG, ResourceState::ShaderResource);
            if (mom1RG.IsValid())
                builder.Read(mom1RG, ResourceState::ShaderResource);

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
                    // TEMP-DIAG: deferred 的 vp/invVP 真值
                    if (std::getenv("PRIMAL_DUMP_MATRIX")) {
                        std::fprintf(stderr, "[DEF_VP] %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f\n",
                                     vp.columns[0].x, vp.columns[0].y, vp.columns[0].z, vp.columns[0].w,
                                     vp.columns[1].x, vp.columns[1].y, vp.columns[1].z, vp.columns[1].w,
                                     vp.columns[2].x, vp.columns[2].y, vp.columns[2].z, vp.columns[2].w,
                                     vp.columns[3].x, vp.columns[3].y, vp.columns[3].z, vp.columns[3].w);
                        const math::m4x4& iv = vd->invViewProjection;
                        std::fprintf(stderr, "[DEF_INVP] %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f\n",
                                     iv.columns[0].x, iv.columns[0].y, iv.columns[0].z, iv.columns[0].w,
                                     iv.columns[1].x, iv.columns[1].y, iv.columns[1].z, iv.columns[1].w,
                                     iv.columns[2].x, iv.columns[2].y, iv.columns[2].z, iv.columns[2].w,
                                     iv.columns[3].x, iv.columns[3].y, iv.columns[3].z, iv.columns[3].w);
                    }
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
                    sd->shadowMatrix2 = inputs.shadow_matrix2;   // P4.2
                    sd->shadowMatrix3 = inputs.shadow_matrix3;   // P4.2
                    // PRIMAL_SHADOW_VIS_DEBUG=1: vis-only visualization (0.5).
                    // PRIMAL_GBUFFER_ALBEDO_DEBUG=1: raw albedo (0.25).
                    // Both keep the PCSS branch semantics; see the frag.
                    const bool albedoDebug = std::getenv("PRIMAL_GBUFFER_ALBEDO_DEBUG") != nullptr;
                    const bool visDebug = std::getenv("PRIMAL_SHADOW_VIS_DEBUG") != nullptr;
                    static bool visDebugLogged = false;
                    if ((visDebug || albedoDebug) && !visDebugLogged) {
                        visDebugLogged = true;
                        std::fprintf(stderr, "[VIS_DEBUG] shadowParams.x=%s active\n",
                                     albedoDebug ? "0.25 (albedo)" : "0.5 (vis)");
                    }
                    float translucency = 0.0f;
                    if (const char* tl = std::getenv("PRIMAL_TONE_TRANSLUCENCY"))
                        translucency = static_cast<float>(std::atof(tl));
                    sd->shadowParams = math::v4{
                        albedoDebug ? 0.25f
                            : (visDebug ? 0.5f : (inputs.vsm_enabled ? 1.0f : 0.0f)),
                        inputs.cascade_splits.x,
                        0.0f, translucency};
                    // Raw Vulkan diagnostics keep the selected shadow method.
                    if (const char* debug = std::getenv("PRIMAL_LIGHTING_DEBUG");
                        debug && device_->GetPlatform() == rhi::RHIPlatform::Vulkan) {
                        if (std::strcmp(debug, "normal") == 0) sd->shadowParams.z = 1.0f;
                        if (std::strcmp(debug, "ndotl") == 0) sd->shadowParams.z = 2.0f;
                        if (std::strcmp(debug, "shadow") == 0) sd->shadowParams.z = 3.0f;
                        if (std::strcmp(debug, "albedo") == 0) sd->shadowParams.z = 4.0f;
                    }
                    // sun-audit P2 材质消融（z+10 编码，与 PRIMAL_LIGHTING_DEBUG
                    // 互斥，后写覆盖）：gray_albedo/flat_normal/fixed_roughness。
                    static const float s_materialAblation = [] {
                        const char* e = std::getenv("PRIMAL_MATERIAL_ABLATION");
                        if (!e) return 0.0f;
                        if (std::strcmp(e, "gray_albedo") == 0) return 11.0f;
                        if (std::strcmp(e, "flat_normal") == 0) return 12.0f;
                        if (std::strcmp(e, "fixed_roughness") == 0) return 13.0f;
                        return 0.0f;
                    }();
                    if (s_materialAblation > 0.0f) sd->shadowParams.z = s_materialAblation;
                    device_->UnmapBuffer(scene_cb_[cbIdx]);
                    // TEMP-DIAG: SceneData 上传字节真值（PRIMAL_DUMP_MATRIX=1）
                    if (std::getenv("PRIMAL_DUMP_MATRIX")) {
                        auto* chk = static_cast<const SceneData*>(0);
                        (void)chk;
                        const math::m4x4& mm = inputs.shadow_matrix0;
                        std::fprintf(stderr, "[SCENE_MAT0] %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f\n",
                                     mm.columns[0].x, mm.columns[0].y, mm.columns[0].z, mm.columns[0].w,
                                     mm.columns[1].x, mm.columns[1].y, mm.columns[1].z, mm.columns[1].w,
                                     mm.columns[2].x, mm.columns[2].y, mm.columns[2].z, mm.columns[2].w,
                                     mm.columns[3].x, mm.columns[3].y, mm.columns[3].z, mm.columns[3].w);
                        std::fprintf(stderr, "[SCENE_MISC] viewPos=(%.2f,%.2f,%.2f) shadowParams.x=%.1f\n",
                                     inputs.camera_position.x, inputs.camera_position.y,
                                     inputs.camera_position.z, inputs.vsm_enabled ? 1.0f : 0.0f);
                    }
                }
            }

            // Bind resources
            auto depthSampleable = inputs.gpu_draw_pipeline->GetGBufferDepthSampleable();
            // T4.6.5 part 24.4 (B2 fix): use fallback_tex_ for invalid bindings
            // so descriptor has a valid imageView. SSAO/shadow_visibility may
            // be INVALID when those features aren't enabled.
            // 2026-09-22 加固：0 句柄同样视为无效（ShadowMapOutputs 曾以
            // 零初始化句柄上流，见 ShadowMapModule.h 同日修复注释）。
            auto validOrFallback = [](ResourceHandle h, ResourceHandle fb) {
                return (h != handles::INVALID_RESOURCE && h != rhi::ResourceHandle(0))
                           ? h : fb;
            };

            ResourceHandle albedoTex = validOrFallback(inputs.gpu_draw_pipeline->GetGBufferAlbedo(), fallback_tex_);
            ResourceHandle normalTex = validOrFallback(inputs.gpu_draw_pipeline->GetGBufferNormal(), fallback_tex_);
            ResourceHandle ormTex = validOrFallback(inputs.gpu_draw_pipeline->GetGBufferORM(), fallback_tex_);
            ResourceHandle depthTex = validOrFallback(depthSampleable, fallback_tex_);
            ResourceHandle shadowVisTex = validOrFallback(inputs.shadow_visibility_tex, fallback_tex_);
            // SSAO filter output — binding 7. When SSAO pass is absent, fall back
            // to white (1.0 = unoccluded) so ambient is unchanged.
            ResourceHandle ssaoTex = fallback_tex_;
            if (inputs.ssao_pass && inputs.ssao_pass->IsInitialized()) {
                ResourceHandle ft = inputs.ssao_pass->GetFilterTexture();
                if (ft != handles::INVALID_RESOURCE) ssaoTex = ft;
            }

            DescData params[] = {
                {0, DescriptorType::UniformBuffer, view_cb_[cbIdx]},
                {1, DescriptorType::UniformBuffer, scene_cb_[cbIdx]},
                {2, DescriptorType::SampledImage, albedoTex},
                {3, DescriptorType::SampledImage, normalTex},
                {4, DescriptorType::SampledImage, ormTex},
                {5, DescriptorType::SampledImage, depthTex},
                {6, DescriptorType::SampledImage, shadowVisTex},
                {7, DescriptorType::SampledImage, ssaoTex},
                {8, DescriptorType::Sampler, static_cast<ResourceHandle>(sampler_)},
                {9, DescriptorType::SampledImage, fallback_tex_},
                // T4.6.5 part 37: IBL bindings 10/11/12. Fall back to the 1x1
                // white texture when IBL isn't configured — shader's IBL branch
                // still samples something valid (white = no ambient tint added).
                {10, DescriptorType::SampledImage,
                 validOrFallback(ibl_irradiance_, fallback_tex_)},
                {11, DescriptorType::SampledImage,
                 validOrFallback(ibl_prefilter_, fallback_tex_)},
                {12, DescriptorType::SampledImage,
                 validOrFallback(ibl_brdf_lut_, fallback_tex_)},
                // VSM shadow moments (13/14). White fallback keeps Chebyshev
                // math valid (moments (1,1) → unoccluded) when VSM is off.
                {13, DescriptorType::SampledImage,
                 validOrFallback(inputs.shadow_moments_tex[0], fallback_tex_)},
                {14, DescriptorType::SampledImage,
                 validOrFallback(inputs.shadow_moments_tex[1], fallback_tex_)},
                {15, DescriptorType::SampledImage,
                 validOrFallback(inputs.shadow_moments_tex[2], fallback_tex_)},
                {16, DescriptorType::SampledImage,
                 validOrFallback(inputs.shadow_moments_tex[3], fallback_tex_)},
                {17, DescriptorType::SampledImage,
                 validOrFallback(inputs.shadow_translucent_tex, fallback_tex_)},  // P4.1
                // sun-audit P2 第二步: 探针深度 SSBO + 参数（PRIMAL_PROBE_OCC
                // 变体访问；其余变体不访问——无效时 dummy + counts=0 早退）。
                {18, DescriptorType::StorageBuffer,
                 inputs.probe_depth_buf != handles::INVALID_RESOURCE
                     ? inputs.probe_depth_buf : probe_depth_dummy_},
                {19, DescriptorType::UniformBuffer, probe_param_cb_[cbIdx]},
            };
            UpdateDesc(device_, descriptor_sets_[cbIdx], params, 20);

            // 探针参数上传（origin/spacing/counts；句柄无效时 counts=0）
            if (probe_param_cb_[cbIdx] != handles::INVALID_RESOURCE) {
                struct ProbeParamsGPU {
                    math::v4 origin_spacing;
                    math::v4 counts;
                };
                static_assert(sizeof(ProbeParamsGPU) == 32, "ProbeParams must be 32B");
                const bool probeValid = inputs.probe_depth_buf != handles::INVALID_RESOURCE &&
                                        inputs.probe_counts.w > 0.5f;
                const ProbeParamsGPU pp{
                    inputs.probe_origin_spacing,
                    probeValid ? inputs.probe_counts : math::v4{0.0f, 0.0f, 0.0f, 0.0f}};
                device_->UpdateBufferData(probe_param_cb_[cbIdx], &pp, sizeof(pp), 0);
            }

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
