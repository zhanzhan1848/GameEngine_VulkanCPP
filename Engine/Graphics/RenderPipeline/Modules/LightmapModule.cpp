// Lightmap 旁路消费模块（2026-10-19 计划，M0 骨架形态）。
// 隔离契约见 LightmapModule.h 头注释。本文件不 include 任何 Lumen 头，
// 不读写任何 DDGI/SPGI/SSGI/SC 资源。

#include "LightmapModule.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include <vector>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include "Engine/Common/CoreLogStream.h"

namespace primal::graphics {

using namespace rhi;

namespace {

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
        if (params[i].type == DescriptorType::Sampler) {
            imageInfos[i].sampler = static_cast<SamplerHandle>(params[i].resource);
            writes[i].imageInfo = &imageInfos[i];
        } else if (params[i].type == DescriptorType::UniformBuffer) {
            bufferInfos[i].buffer = params[i].resource;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = 16;
            writes[i].bufferInfo = &bufferInfos[i];
        } else {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

// f32 → f16 位级转换（与 TestVulkanSponzaRenderGraph.cpp 的 f32_to_f16 同式）。
static u16 FloatToHalf(f32 f) {
    u32 x;
    std::memcpy(&x, &f, sizeof(x));
    const u32 sign = (x >> 16) & 0x8000u;
    const s32 exp = static_cast<s32>((x >> 23) & 0xFFu) - 127 + 15;
    const u32 mant = x & 0x007FFFFFu;
    if (exp <= 0) return static_cast<u16>(sign);
    if (exp >= 31) return static_cast<u16>(sign | 0x7C00u);
    return static_cast<u16>(sign | (static_cast<u32>(exp) << 10) | (mant >> 13));
}

// 激活开关：PRIMAL_LIGHTMAP=0 强制关闭（零影响回归的 A/B 旋钮）。
static bool LightmapEnabledByEnvChecked() {
    const char* e = std::getenv("PRIMAL_LIGHTMAP");
    return !(e && e[0] == '0' && e[1] == '\0');
}

} // namespace

bool LightmapModule::Initialize(RHIDeviceBase* device, rhi::ShaderHandle full_screen_vs,
                                u32 render_width, u32 render_height) {
    if (!device || full_screen_vs == handles::INVALID_SHADER) return false;
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    full_screen_vs_ = full_screen_vs;

    // --- shader 自装（正确 entry 名；管线 SelfLoad 的 Vulkan 分支不适用）---
    if (device->GetPlatform() == RHIPlatform::Vulkan) {
        const std::string path = utils::ShaderRegistry::ResolveShaderPath(
            "Engine/Graphics/Vulkan/shaders/Lightmap/LightmapComposite.frag.spv");
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f.is_open()) {
            // 未部署 spv（未跑 build_spv.sh）——按"模块不存在"处理。
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[Lightmap] composite spv not found (module inert): " << path << std::endl;
            return false;
        }
        const std::streamsize size = f.tellg();
        f.seekg(0);
        std::vector<u8> bytes(static_cast<size_t>(size));
        if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) return false;
        composite_ps_ = device->CreateShader(bytes.data(), bytes.size(),
                                             ShaderStage::Pixel, "lightmap_composite");
        if (composite_ps_ == handles::INVALID_SHADER) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[Lightmap] CreateShader failed" << std::endl;
            return false;
        }
    } else {
        // M5 Metal 移植前的降级：非 Vulkan 平台模块不存在（Dawn 边界已在
        // Parity Matrix 登记为不支持 lightmap 消费）。
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Lightmap] platform not Vulkan — module inert (M5 pending)" << std::endl;
        return false;
    }

    // --- 描述符/管线/资源（镜像 FusionCompositeModule 全屏 pass 形态）---
    const u32 bindingCount = 4;
    DescriptorSetLayoutBinding bindings[4];
    bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // indirectIn
    bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // lightmapTex
    bindings[2] = {2, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr};       // sampler
    bindings[3] = {3, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr}; // params
    composite_set_layout_ = device->CreateDescriptorSetLayout({bindingCount, bindings});
    composite_layout_ = device->CreatePipelineLayout({1, &composite_set_layout_});
    composite_ds_ = device->CreateDescriptorSet({composite_set_layout_});

    {
        BufferDesc cb{};
        cb.size = 16;
        cb.type = BufferType::Constant;
        cb.memoryUsage = GPUMemoryUsage::Dynamic;
        params_cb_ = device->CreateBuffer(cb);
    }
    {
        SamplerDesc sDesc{};  // 线性过滤 + clamp——烘焙页采样语义
        sDesc.minFilter = FilterMode::Linear;
        sDesc.magFilter = FilterMode::Linear;
        sDesc.addressU = TextureAddressMode::Clamp;
        sDesc.addressV = TextureAddressMode::Clamp;
        sDesc.addressW = TextureAddressMode::Clamp;
        sampler_ = device->CreateSampler(sDesc);
    }
    {
        TextureDesc outDesc{};
        outDesc.size = {render_width_, render_height_, 1};
        outDesc.format = DataFormat::RGBA16_Float;
        outDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
        output_tex_ = device->CreateTexture(outDesc);
    }
    {
        GraphicsPipelineDesc pd{};
        pd.layout = composite_layout_;
        pd.vertexShader = full_screen_vs_;
        pd.pixelShader = composite_ps_;
        pd.renderTargetFormats[0] = DataFormat::RGBA16_Float;
        pd.renderTargetCount = 1;
        pd.depthStencilFormat = DataFormat::Unknown;
        pd.enableDepthTest = false;
        pd.enableDepthWrite = false;
        pd.cullMode = CullMode::None;
        pd.vertexAttributes.clear();
        pd.vertexBindings.clear();
        composite_pipeline_ = device->CreateGraphicsPipeline(pd);
    }

    const bool ok = composite_pipeline_ != handles::INVALID_PIPELINE &&
                    output_tex_ != handles::INVALID_RESOURCE;
    if (!ok) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[Lightmap] pipeline/output init failed" << std::endl;
        Shutdown();
    }
    return ok;
}

void LightmapModule::Shutdown() {
    if (!device_) return;
    if (lightmap_tex_ != handles::INVALID_RESOURCE) device_->DestroyTexture(lightmap_tex_);
    if (output_tex_ != handles::INVALID_RESOURCE) device_->DestroyTexture(output_tex_);
    if (params_cb_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(params_cb_);
    if (sampler_ != handles::INVALID_SAMPLER) device_->DestroySampler(sampler_);
    if (composite_ds_ != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(composite_ds_);
    if (composite_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT)
        device_->DestroyDescriptorSetLayout(composite_set_layout_);
    if (composite_layout_ != handles::INVALID_PIPELINE_LAYOUT)
        device_->DestroyPipelineLayout(composite_layout_);
    if (composite_pipeline_ != handles::INVALID_PIPELINE) device_->DestroyPipeline(composite_pipeline_);
    lightmap_tex_ = output_tex_ = params_cb_ = handles::INVALID_RESOURCE;
    sampler_ = handles::INVALID_SAMPLER;
    composite_pipeline_ = handles::INVALID_PIPELINE;
    device_ = nullptr;
}

rhi::ResourceHandle LightmapModule::CreateGradientLightmapTexture(u32 size) {
    // 过程生成渐变页：R=x, G=y, B=0.25, A=1（RGBA16F）。
    TextureDesc desc{};
    desc.size = {size, size, 1};
    desc.format = DataFormat::RGBA16_Float;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    ResourceHandle tex = device_->CreateTexture(desc);
    if (tex == handles::INVALID_RESOURCE) return tex;

    const u32 rowBytes = size * 8;  // RGBA16F = 8B/px；256px 行 = 2048B，天然 256 对齐
    std::vector<u16> half(static_cast<size_t>(size) * size * 4);
    for (u32 y = 0; y < size; ++y) {
        for (u32 x = 0; x < size; ++x) {
            const size_t i = (static_cast<size_t>(y) * size + x) * 4;
            half[i + 0] = FloatToHalf(static_cast<f32>(x) / static_cast<f32>(size - 1));
            half[i + 1] = FloatToHalf(static_cast<f32>(y) / static_cast<f32>(size - 1));
            half[i + 2] = FloatToHalf(0.25f);
            half[i + 3] = FloatToHalf(1.0f);
        }
    }

    BufferDesc sbDesc{};
    sbDesc.size = static_cast<u64>(rowBytes) * size;
    sbDesc.usage = GPUMemoryUsage::Staging;
    sbDesc.memoryUsage = GPUMemoryUsage::Staging;
    ResourceHandle staging = device_->CreateBuffer(sbDesc);
    if (staging == handles::INVALID_RESOURCE) {
        device_->DestroyTexture(tex);
        return handles::INVALID_RESOURCE;
    }
    void* mapped = device_->MapBuffer(staging);
    if (mapped) {
        std::memcpy(mapped, half.data(), half.size() * sizeof(u16));
        device_->UnmapBuffer(staging);
    }

    SyncHandle fence = device_->CreateSync();
    auto cmdHandle = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    auto* cmd = GetCommandBuffer(cmdHandle);
    cmd->Begin();
    BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.imageSubresource = {0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {size, size, 1};
    cmd->CopyBufferToTexture(staging, tex, &region, 1);
    cmd->End();
    QueueSubmitInfo submit{};
    submit.cmdBuffer = cmdHandle;
    submit.signalFence = fence;
    device_->Submit(submit);
    device_->WaitForSync(fence, UINT32_MAX);
    device_->DestroySync(fence);
    device_->DestroyCommandBuffer(cmdHandle);
    device_->DestroyBuffer(staging);
    return tex;
}

void LightmapModule::RegisterDebugLightmap() {
    if (!device_) return;
    if (lightmap_tex_ != handles::INVALID_RESOURCE) return;  // 幂等
    lightmap_tex_ = CreateGradientLightmapTexture(256);
    if (lightmap_tex_ == handles::INVALID_RESOURCE) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[Lightmap] debug gradient texture failed" << std::endl;
        return;
    }
    PRIMAL_LOG_STREAM(Info, "Render") << "[Lightmap] debug gradient page registered (256x256)" << std::endl;
}

void LightmapModule::ClearLightmap() {
    if (device_ && lightmap_tex_ != handles::INVALID_RESOURCE)
        device_->DestroyTexture(lightmap_tex_);
    lightmap_tex_ = handles::INVALID_RESOURCE;
}

rendergraph::RGResourceHandle LightmapModule::AddCompositePass(
    rendergraph::RenderGraph& graph, const FrameInputs& inputs,
    rendergraph::RGResourceHandle indirect_rg) {
    using rendergraph::RenderGraphBuilder;

    if (!LightmapEnabledByEnvChecked()) return rendergraph::RGResourceHandle{};

    auto outRG = graph.ImportResource("LightmapCompositeOut", output_tex_);

    struct CompositeData {
        rendergraph::RGResourceHandle output;
    };
    graph.AddPass<CompositeData>("LightmapComposite",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [indirect_rg, outRG](CompositeData& data, RenderGraphBuilder& builder) {
            builder.SideEffect();
            builder.Read(indirect_rg, ResourceState::ShaderResource);
            data.output = builder.Write(outRG, ResourceState::RenderTarget);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({.texture = data.output, .loadOp = LoadAction::DontCare,
                                     .storeOp = StoreAction::Store,
                                     .clearColor = {math::v4{0, 0, 0, 0}}});
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs](const CompositeData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            cmd->SetViewport({{0, 0}, {static_cast<float>(render_width_), static_cast<float>(render_height_)}, 0, 1});
            cmd->SetScissor({{0, 0}, {render_width_, render_height_}});

            // 与 FusionIndirect 同源增益（默认 1.0，物理恒等）。
            float gain = 1.0f;
            if (const char* g = std::getenv("PRIMAL_GI_INDIRECT_GAIN")) {
                const float v = static_cast<float>(std::atof(g));
                if (v >= 0.0f && v <= 8.0f) gain = v;
            }
            // M0 debug 开关：PRIMAL_LIGHTMAP_DEBUG=1 时全屏替换为渐变页采样。
            float debugMode = 0.0f;
            if (const char* d = std::getenv("PRIMAL_LIGHTMAP_DEBUG")) {
                if (d[0] == '1') debugMode = 1.0f;
            }
            if (params_cb_ != handles::INVALID_RESOURCE) {
                const float ubo[2] = {gain, debugMode};
                device_->UpdateBufferData(params_cb_, ubo, sizeof(ubo), 0);
            }

            DescData params[4] = {
                {0, DescriptorType::SampledImage, inputs.indirect_tex},
                {1, DescriptorType::SampledImage, lightmap_tex_},
                {2, DescriptorType::Sampler, static_cast<ResourceHandle>(sampler_)},
                {3, DescriptorType::UniformBuffer, params_cb_},
            };
            UpdateDesc(device_, composite_ds_, params, 4);
            cmd->BindGraphicsPipeline(composite_pipeline_);
            const DescriptorSetHandle sets[] = {composite_ds_};
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, composite_layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        });

    return outRG;
}

} // namespace primal::graphics
