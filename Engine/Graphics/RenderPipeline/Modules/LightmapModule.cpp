// Lightmap 旁路消费模块（2026-10-19 计划，M3 形态）。
// 隔离契约见 LightmapModule.h 头注释。本文件不 include 任何 Lumen 头，
// 不读写任何 DDGI/SPGI/SSGI/SC 资源；实体几何数据自管缓冲，与
// GPUDrivenDrawPipeline 全局缓冲零耦合。

#include "LightmapModule.h"
#include "Graphics/Baking/LightmapContainer.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RenderScene.h"
#include "Graphics/Utils/ShaderRegistry.h"
#include "Content/ContentToEngine.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <iterator>
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
        } else if (params[i].type == DescriptorType::StorageBuffer) {
            bufferInfos[i].buffer = params[i].resource;
            bufferInfos[i].offset = 0;
            bufferInfos[i].range = 0;  // VK_WHOLE_SIZE
            writes[i].bufferInfo = &bufferInfos[i];
        } else {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

// f32 → f16（位级）。
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
static bool LightmapEnabledByEnv() {
    const char* e = std::getenv("PRIMAL_LIGHTMAP");
    return !(e && e[0] == '0' && e[1] == '\0');
}

// staging 上传 RGBA16F 页（LMAP 页宽为 2 的幂 → 行宽×8B 天然 256 对齐）。
static bool UploadRGBA16F(RHIDeviceBase* device, ResourceHandle tex,
                          const std::vector<u16>& texels, u32 w, u32 h) {
    BufferDesc sbDesc{};
    sbDesc.size = (u64)w * h * 8;
    sbDesc.usage = GPUMemoryUsage::Staging;
    sbDesc.memoryUsage = GPUMemoryUsage::Staging;
    ResourceHandle staging = device->CreateBuffer(sbDesc);
    if (staging == handles::INVALID_RESOURCE) return false;
    void* mapped = device->MapBuffer(staging);
    if (!mapped) {
        device->DestroyBuffer(staging);
        return false;
    }
    std::memcpy(mapped, texels.data(), texels.size() * sizeof(u16));
    device->UnmapBuffer(staging);

    SyncHandle fence = device->CreateSync();
    auto cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
    auto* cmd = GetCommandBuffer(cmdHandle);
    cmd->Begin();
    BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.imageSubresource = {0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};
    cmd->CopyBufferToTexture(staging, tex, &region, 1);
    cmd->End();
    QueueSubmitInfo submit{};
    submit.cmdBuffer = cmdHandle;
    submit.signalFence = fence;
    device->Submit(submit);
    device->WaitForSync(fence, UINT32_MAX);
    device->DestroySync(fence);
    device->DestroyCommandBuffer(cmdHandle);
    device->DestroyBuffer(staging);
    return true;
}

// UV pre-pass push 常量布局（与 LightmapUVPass.vert 一致）。
struct UvPushConstants {
    math::m4x4 view_proj;
    std::uint32_t position_offset;
    std::uint32_t uv_offset;
    float scale_bias[4];
    float calib;  // §13 V 翻转金标定：>0.5 时 FS 输出归一化 gl_FragCoord.xy
};

} // namespace

bool LightmapModule::Initialize(RHIDeviceBase* device, rhi::ShaderHandle full_screen_vs,
                                u32 render_width, u32 render_height) {
    if (!device || full_screen_vs == handles::INVALID_SHADER) return false;
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    full_screen_vs_ = full_screen_vs;

    // --- shader 自装（正确 entry 名；管线 SelfLoad 的 Vulkan 分支不适用）---
    // Vulkan：spv；Metal（M5）：MSL 源文本。Dawn：不支持（Parity Matrix
    // 登记降级边界）——模块不存在。
    const RHIPlatform platform = device->GetPlatform();
    bool shadersOk = false;
    if (platform == RHIPlatform::Vulkan) {
        auto loadSpv = [&](const char* rel, ShaderStage stage, const char* entry,
                           ShaderHandle& out) {
            const std::string path = utils::ShaderRegistry::ResolveShaderPath(rel);
            std::ifstream f(path, std::ios::binary | std::ios::ate);
            if (!f.is_open()) return false;
            const std::streamsize size = f.tellg();
            f.seekg(0);
            std::vector<u8> bytes(static_cast<size_t>(size));
            if (!f.read(reinterpret_cast<char*>(bytes.data()), size)) return false;
            out = device->CreateShader(bytes.data(), bytes.size(), stage, entry);
            return out != handles::INVALID_SHADER;
        };
        shadersOk =
            loadSpv("Engine/Graphics/Vulkan/shaders/Lightmap/LightmapComposite.frag.spv",
                    ShaderStage::Pixel, "lightmap_composite", composite_ps_) &&
            loadSpv("Engine/Graphics/Vulkan/shaders/Lightmap/LightmapUVPass.vert.spv",
                    ShaderStage::Vertex, "main", uv_vs_) &&
            loadSpv("Engine/Graphics/Vulkan/shaders/Lightmap/LightmapUVPass.frag.spv",
                    ShaderStage::Pixel, "main", uv_fs_);
    } else if (platform == RHIPlatform::Metal) {
        auto loadMsl = [&](const char* rel, ShaderStage stage, const char* entry,
                           ShaderHandle& out) {
            const std::string path = utils::ShaderRegistry::ResolveShaderPath(rel);
            std::ifstream f(path);
            if (!f.is_open()) return false;
            std::string src((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());
            if (src.empty()) return false;
            out = device->CreateShader(src.data(), src.size(), stage, entry);
            return out != handles::INVALID_SHADER;
        };
        shadersOk =
            loadMsl("Engine/Graphics/Metal/shaders/Lightmap/LightmapComposite.metal",
                    ShaderStage::Pixel, "lightmap_composite_fragment", composite_ps_) &&
            loadMsl("Engine/Graphics/Metal/shaders/Lightmap/LightmapUVPass.metal",
                    ShaderStage::Vertex, "lightmap_uv_vertex", uv_vs_) &&
            loadMsl("Engine/Graphics/Metal/shaders/Lightmap/LightmapUVPass.metal",
                    ShaderStage::Pixel, "lightmap_uv_fragment", uv_fs_);
    } else {
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Lightmap] platform unsupported (Dawn) — module inert" << std::endl;
        return false;
    }
    if (!shadersOk) {
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Lightmap] spv not deployed (module inert; run build_spv.sh)" << std::endl;
        return false;
    }

    // --- composite 全屏管线（0 indirect / 1 lightmap / 2 uvRT / 3 sampler /
    //     4 gbufferAlbedo——v3 间接辐照度合成的 albedo 调制源；gain/debug
    //     走 push 常量，见 shader 头注释）---
    {
        DescriptorSetLayoutBinding bindings[5];
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};
        bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};
        bindings[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};
        bindings[3] = {3, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr};
        bindings[4] = {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};
        composite_set_layout_ = device->CreateDescriptorSetLayout({5, bindings});

        PipelineLayoutDesc plDesc{};
        PushConstantRange pcRange{};
        pcRange.stageFlags = ShaderStage::Pixel;
        pcRange.offset = 0;
        pcRange.size = 20;  // gain + debugMode + filterMode + frameIndex + darkTint（§21 问题2a）
        plDesc.pushConstantRangeCount = 1;
        plDesc.pushConstantRanges = &pcRange;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &composite_set_layout_;
        composite_layout_ = device->CreatePipelineLayout(plDesc);
        // 三槽轮转（与 fusion cbIdx 语义对齐：帧号 %3——DDGISlotMap 当前恒等）。
        for (int i = 0; i < 3; ++i)
            composite_ds_[i] = device->CreateDescriptorSet({composite_set_layout_});

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

    // --- UV pre-pass 管线（binding 0/1 = 顶点阶段 SSBO；push 常量）---
    {
        DescriptorSetLayoutBinding bindings[2];
        bindings[0] = {0, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex, nullptr};
        bindings[1] = {1, DescriptorType::StorageBuffer, 1, ShaderStage::Vertex, nullptr};
        uv_set_layout_ = device->CreateDescriptorSetLayout({2, bindings});

        PipelineLayoutDesc plDesc{};
        PushConstantRange pcRange{};
        pcRange.stageFlags = ShaderStage::Vertex | ShaderStage::Pixel;
        pcRange.offset = 0;
        pcRange.size = sizeof(UvPushConstants);
        plDesc.pushConstantRangeCount = 1;
        plDesc.pushConstantRanges = &pcRange;
        plDesc.setLayoutCount = 1;
        plDesc.setLayouts = &uv_set_layout_;
        uv_layout_ = device->CreatePipelineLayout(plDesc);
        uv_ds_ = device->CreateDescriptorSet({uv_set_layout_});

        GraphicsPipelineDesc pd{};
        pd.layout = uv_layout_;
        pd.vertexShader = uv_vs_;
        pd.pixelShader = uv_fs_;
        pd.renderTargetFormats[0] = DataFormat::RGBA16_Float;
        pd.renderTargetCount = 1;
        pd.depthStencilFormat = DataFormat::D32_Float;
        pd.enableDepthTest = true;
        pd.enableDepthWrite = false;
        // 重绘匹配（rebinding）深度语义：与 GBuffer 同面等值通过。两条变换链
        // 的浮点结合序差异由 StandardRenderPipeline::LightmapUvPassViewProjection
        // 的 z 行 (1-eps) 收缩吸收（2026-10-20 取证：Vulkan slope-scaled
        // depthBias 对 D32_Float 按 ULP 缩放，0.05~0.2 档全无效——故不走
        // 管线偏差）。PRIMAL_LIGHTMAP_DEPTHALWAYS=1 为取证旋钮：跳过深度比较
        // （遮挡关系错误但覆盖计数不失真），用于分离"深度等值性失配"与
        // "几何绘制缺失"两类覆盖失效。
            pd.depthFunc = std::getenv("PRIMAL_LIGHTMAP_DEPTHALWAYS")
                ? ComparisonFunc::Always : ComparisonFunc::LessEqual;
            // [2026-10-21 远平面黑带轮] 裁剪实验旋钮 PRIMAL_LIGHTMAP_CULL=
            // none/front/back（默认 none = 原形态）。假设：UV pass 双面直绘
            // 在 z-eps 松弛内让背面/次表面过测并在后绘覆盖 marker（远平面
            // 相对深度拥挤处成带）。与 GBuffer 光栅化集对齐的裁剪（考虑
            // Y 翻转导致的绕序反转）应消除该带——由 UVDUMP 覆盖 + 黑带
            // 采集裁决后定默认值。
            pd.cullMode = [] {
                const char* c = std::getenv("PRIMAL_LIGHTMAP_CULL");
                if (c && c[0] == 'f') return CullMode::Front;
                if (c && c[0] == 'b') return CullMode::Back;
                return CullMode::None;               // 烘焙面双面（原形态）
            }();
        pd.topology = PrimitiveTopology::TriangleList;
        pd.vertexAttributes.clear();               // 顶点拉取
        pd.vertexBindings.clear();
        uv_pipeline_ = device->CreateGraphicsPipeline(pd);
    }

    // --- 公共资源 ---
    {
        SamplerDesc sDesc{};
        sDesc.minFilter = FilterMode::Linear;
        sDesc.magFilter = FilterMode::Linear;
        sDesc.addressU = TextureAddressMode::Clamp;
        sDesc.addressV = TextureAddressMode::Clamp;
        sDesc.addressW = TextureAddressMode::Clamp;
        sampler_ = device->CreateSampler(sDesc);
    }
    {
        TextureDesc rtDesc{};
        rtDesc.size = {render_width_, render_height_, 1};
        rtDesc.format = DataFormat::RGBA16_Float;
        rtDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
        output_tex_ = device->CreateTexture(rtDesc);
        uv_rt_ = device->CreateTexture(rtDesc);
    }
    {
        // 1×1 z=0 UV 占位（无实体时 composite 读它 → 全像素走原路径）。
        TextureDesc d{};
        d.size = {1, 1, 1};
        d.format = DataFormat::RGBA16_Float;
        d.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
        uv_black_tex_ = device->CreateTexture(d);
        if (uv_black_tex_ != handles::INVALID_RESOURCE) {
            const std::vector<u16> zero{0, 0, 0, 0};
            UploadRGBA16F(device_, uv_black_tex_, zero, 1, 1);
        }
        // 1×1 白 albedo 占位（binding 4 兜底：gbuffer_albedo 缺失时 albedo=1，
        // 间接项退化为纯 lm 增益——不失活）。
        albedo_white_tex_ = device->CreateTexture(d);
        if (albedo_white_tex_ != handles::INVALID_RESOURCE) {
            const std::vector<u16> white{0x3C00, 0x3C00, 0x3C00, 0x3C00};
            UploadRGBA16F(device_, albedo_white_tex_, white, 1, 1);
        }
    }
    {
        // 编码器切换 flush 哑元对（16B；见 FrameInputs::encoder_flush 注）。
        BufferDesc fb{};
        fb.size = 16;
        fb.usage = GPUMemoryUsage::Dynamic;
        fb.memoryUsage = GPUMemoryUsage::Dynamic;
        flush_src_ = device->CreateBuffer(fb);
        flush_dst_ = device->CreateBuffer(fb);
    }

    const bool ok = composite_pipeline_ != handles::INVALID_PIPELINE &&
                    uv_pipeline_ != handles::INVALID_PIPELINE &&
                    output_tex_ != handles::INVALID_RESOURCE;
    if (ok) {
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Lightmap] module initialized ("
            << (platform == RHIPlatform::Vulkan ? "Vulkan" : "Metal") << ", "
            << render_width_ << "x" << render_height_ << ")" << std::endl;
    }
    if (!ok) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[Lightmap] pipeline/rt init failed" << std::endl;
        Shutdown();
    }
    return ok;
}

void LightmapModule::Shutdown() {
    if (!device_) return;
    if (lightmap_tex_ != handles::INVALID_RESOURCE) device_->DestroyTexture(lightmap_tex_);
    if (output_tex_ != handles::INVALID_RESOURCE) device_->DestroyTexture(output_tex_);
    if (uv_rt_ != handles::INVALID_RESOURCE) device_->DestroyTexture(uv_rt_);
    if (uv_black_tex_ != handles::INVALID_RESOURCE) device_->DestroyTexture(uv_black_tex_);
    if (albedo_white_tex_ != handles::INVALID_RESOURCE) device_->DestroyTexture(albedo_white_tex_);
    if (flush_src_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(flush_src_);
    if (flush_dst_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(flush_dst_);
    if (positions_sb_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(positions_sb_);
    if (uvs_sb_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(uvs_sb_);
    if (indices_ib_ != handles::INVALID_RESOURCE) device_->DestroyBuffer(indices_ib_);
    if (sampler_ != handles::INVALID_SAMPLER) device_->DestroySampler(sampler_);
    for (int i = 0; i < 3; ++i)
        if (composite_ds_[i] != handles::INVALID_DESCRIPTOR_SET)
            device_->DestroyDescriptorSet(composite_ds_[i]);
    if (uv_ds_ != handles::INVALID_DESCRIPTOR_SET) device_->DestroyDescriptorSet(uv_ds_);
    if (composite_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT)
        device_->DestroyDescriptorSetLayout(composite_set_layout_);
    if (uv_set_layout_ != handles::INVALID_DESCRIPTOR_SET_LAYOUT)
        device_->DestroyDescriptorSetLayout(uv_set_layout_);
    if (composite_layout_ != handles::INVALID_PIPELINE_LAYOUT)
        device_->DestroyPipelineLayout(composite_layout_);
    if (uv_layout_ != handles::INVALID_PIPELINE_LAYOUT) device_->DestroyPipelineLayout(uv_layout_);
    if (composite_pipeline_ != handles::INVALID_PIPELINE) device_->DestroyPipeline(composite_pipeline_);
    if (uv_pipeline_ != handles::INVALID_PIPELINE) device_->DestroyPipeline(uv_pipeline_);
    lightmap_tex_ = output_tex_ = uv_rt_ = uv_black_tex_ = albedo_white_tex_ =
        handles::INVALID_RESOURCE;
    page_w_ = page_h_ = 0;
    page_a_.clear();
    positions_sb_ = uvs_sb_ = indices_ib_ = handles::INVALID_RESOURCE;
    sampler_ = handles::INVALID_SAMPLER;
    composite_pipeline_ = uv_pipeline_ = handles::INVALID_PIPELINE;
    entities_.clear();
    entryScaleBias_.clear();
    matchedEntities_.clear();
    device_ = nullptr;
}

rhi::ResourceHandle LightmapModule::CreateGradientLightmapTexture(u32 size) {
    TextureDesc desc{};
    desc.size = {size, size, 1};
    desc.format = DataFormat::RGBA16_Float;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    ResourceHandle tex = device_->CreateTexture(desc);
    if (tex == handles::INVALID_RESOURCE) return tex;

    std::vector<u16> half(static_cast<size_t>(size) * size * 4);
    for (u32 y = 0; y < size; ++y)
        for (u32 x = 0; x < size; ++x) {
            const size_t i = (static_cast<size_t>(y) * size + x) * 4;
            half[i + 0] = FloatToHalf(static_cast<f32>(x) / static_cast<f32>(size - 1));
            half[i + 1] = FloatToHalf(static_cast<f32>(y) / static_cast<f32>(size - 1));
            half[i + 2] = FloatToHalf(0.25f);
            half[i + 3] = FloatToHalf(1.0f);
        }
    if (!UploadRGBA16F(device_, tex, half, size, size)) {
        device_->DestroyTexture(tex);
        return handles::INVALID_RESOURCE;
    }
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
    page_w_ = page_h_ = 256;
    PRIMAL_LOG_STREAM(Info, "Render") << "[Lightmap] debug gradient page registered (256x256)" << std::endl;
}

void LightmapModule::ClearLightmap() {
    if (device_ && lightmap_tex_ != handles::INVALID_RESOURCE)
        device_->DestroyTexture(lightmap_tex_);
    lightmap_tex_ = handles::INVALID_RESOURCE;
    page_w_ = page_h_ = 0;
    page_a_.clear();
    entities_.clear();
    entryScaleBias_.clear();
    matchedEntities_.clear();
}

bool LightmapModule::LoadLightmap(const std::string& path) {
    if (!device_) return false;
    baking::LightmapContainer c;
    if (!baking::LoadLightmapContainer(path, c) || c.pages.empty()) {
        PRIMAL_LOG_STREAM(Error, "Render") << "[Lightmap] load failed: " << path << std::endl;
        return false;
    }
    const auto& page = c.pages[0];

    if (lightmap_tex_ != handles::INVALID_RESOURCE) device_->DestroyTexture(lightmap_tex_);
    TextureDesc desc{};
    desc.size = {page.width, page.height, 1};
    desc.format = DataFormat::RGBA16_Float;
    desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
    lightmap_tex_ = device_->CreateTexture(desc);
    if (lightmap_tex_ == handles::INVALID_RESOURCE) return false;
    if (!UploadRGBA16F(device_, lightmap_tex_, page.texels_rgba16f, page.width, page.height)) {
        device_->DestroyTexture(lightmap_tex_);
        lightmap_tex_ = handles::INVALID_RESOURCE;
        return false;
    }
    page_w_ = page.width;
    page_h_ = page.height;
    // §18：A 通道 CPU 镜像（UVDUMP=2 marker 落空区统计）。f16 位级转 f32
    //（与 DumpUvRT halfToFloat 同式）。
    page_a_.resize((size_t)page.width * page.height);
    {
        const u16* src = page.texels_rgba16f.data() + 3;
        for (size_t i = 0; i < page_a_.size(); ++i, src += 4) {
            const u32 expo = (*src & 0x7C00u) >> 10, man = *src & 0x03FFu;
            u32 bits;
            if (expo == 0) bits = 0;
            else if (expo == 31) bits = 0x7F800000u | (man << 13);
            else bits = ((expo - 15u + 127u) << 23) | (man << 13);
            std::memcpy(&page_a_[i], &bits, 4);
        }
    }

    entryScaleBias_.clear();
    entities_.clear();
    matchedEntities_.clear();
    for (const auto& e : c.entries) {
        entryScaleBias_[e.GetName()] = {e.scale_x, e.scale_y, e.bias_x, e.bias_y};
    }
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[Lightmap] loaded " << path << " (" << page.width << "x" << page.height
        << ", " << c.entries.size() << " entries)" << std::endl;
    return true;
}

void LightmapModule::UpdateFrame(const RenderScene& scene) {
    if (!device_ || entryScaleBias_.empty()) return;

    bool added = false;
    for (const auto& proxy : scene.GetProxies()) {
        const std::uint64_t key =
            ((std::uint64_t)proxy.meshId << 32) | (std::uint32_t)proxy.entityId;
        if (std::find(matchedEntities_.begin(), matchedEntities_.end(), key) !=
            matchedEntities_.end())
            continue;
        // 收集器命名约定（LightmapSceneCollector）：mesh_<contentId>_e<entityId>。
        const std::string name = "mesh_" + std::to_string(proxy.meshId) + "_e" +
                                 std::to_string(proxy.entityId);
        if (entryScaleBias_.find(name) == entryScaleBias_.end()) continue;
        matchedEntities_.push_back(key);
        added = true;
    }
    if (added) {
        std::vector<EntitySpec> specs;
        for (const auto& proxy : scene.GetProxies()) {
            const std::uint64_t key =
                ((std::uint64_t)proxy.meshId << 32) | (std::uint32_t)proxy.entityId;
            if (std::find(matchedEntities_.begin(), matchedEntities_.end(), key) ==
                matchedEntities_.end())
                continue;
            const std::string name = "mesh_" + std::to_string(proxy.meshId) + "_e" +
                                     std::to_string(proxy.entityId);
            const auto it = entryScaleBias_.find(name);
            if (it == entryScaleBias_.end()) continue;
            EntitySpec spec;
            spec.mesh = proxy.meshId;
            spec.entity = (u32)proxy.entityId;
            spec.transform = proxy.transform;
            std::memcpy(spec.scale_bias, it->second.data(), sizeof(spec.scale_bias));
            specs.push_back(spec);
        }
        for (const auto& d : directEntities_) specs.push_back(d);
        RebuildEntityBuffers(specs);
    }
}

bool LightmapModule::RegisterEntityDirect(id::id_type mesh_content_id, u32 entity_id,
                                          const float scale_bias[4]) {
    if (!device_) return false;
    // 幂等替换。
    for (auto& d : directEntities_) {
        if (d.entity == entity_id) {
            d.mesh = mesh_content_id;
            std::memcpy(d.scale_bias, scale_bias, sizeof(d.scale_bias));
            return true;
        }
    }
    EntitySpec spec;
    spec.mesh = mesh_content_id;
    spec.entity = entity_id;
    // 恒等（原实现：零矩阵 + 对角补 1；DirectXMath 分支无聚合零构造，改为显式四列）
    spec.transform = math::make_m4x4(math::v4{1.f, 0.f, 0.f, 0.f},
                                     math::v4{0.f, 1.f, 0.f, 0.f},
                                     math::v4{0.f, 0.f, 1.f, 0.f},
                                     math::v4{0.f, 0.f, 0.f, 1.f});
    std::memcpy(spec.scale_bias, scale_bias, sizeof(spec.scale_bias));
    directEntities_.push_back(spec);

    std::vector<EntitySpec> specs;
    // 场景匹配部分（已有 matchedEntities_ 时重建 specs 的路径与 UpdateFrame 相同）。
    for (const auto& d : directEntities_) specs.push_back(d);
    RebuildEntityBuffers(specs);
    return true;
}

void LightmapModule::RebuildEntityBuffers(const std::vector<EntitySpec>& specs) {
    // 全量重建（实体集变化不频繁；烘焙为离线动作）。
    entities_.clear();
    std::vector<float> cpuPositions;  // vec4
    std::vector<float> cpuUvs;        // vec2
    std::vector<std::uint32_t> cpuIndices;

    for (const auto& spec : specs) {
        // 两步解析（hierarchy → 资产 id，同收集器注）。
        rhi::RHIMeshAsset asset;
        if (!content::get_rhi_mesh_asset(content::get_rhi_mesh_id(spec.mesh), asset))
            continue;
        if (asset.num_vertices == 0 || asset.num_indices < 3) continue;
        if (asset.lightmap_uv_buffer.size() < (size_t)asset.num_vertices * 8) continue;

        EntityRecord rec{};
        rec.index_count = asset.num_indices;
        rec.first_index = (u32)cpuIndices.size();
        rec.position_offset = (u32)(cpuPositions.size() / 4);
        rec.uv_offset = (u32)(cpuUvs.size() / 2);
        rec.scale_bias[0] = spec.scale_bias[0];
        rec.scale_bias[1] = spec.scale_bias[1];
        rec.scale_bias[2] = spec.scale_bias[2];
        rec.scale_bias[3] = spec.scale_bias[3];

        // 世界空间位置（M × 列向量；spec 变换来自代理或直登记恒等）。
        const auto& m = spec.transform;
        const auto* positions = reinterpret_cast<const float*>(asset.position_buffer.data());
        for (u32 v = 0; v < asset.num_vertices; ++v) {
            const float x = positions[v * 3], y = positions[v * 3 + 1], z = positions[v * 3 + 2];
            cpuPositions.push_back(math::elem(m, 0, 0) * x + math::elem(m, 1, 0) * y + math::elem(m, 2, 0) * z + math::elem(m, 3, 0));
            cpuPositions.push_back(math::elem(m, 0, 1) * x + math::elem(m, 1, 1) * y + math::elem(m, 2, 1) * z + math::elem(m, 3, 1));
            cpuPositions.push_back(math::elem(m, 0, 2) * x + math::elem(m, 1, 2) * y + math::elem(m, 2, 2) * z + math::elem(m, 3, 2));
            cpuPositions.push_back(0.0f);
        }
        const auto* uvs = reinterpret_cast<const float*>(asset.lightmap_uv_buffer.data());
        cpuUvs.insert(cpuUvs.end(), uvs, uvs + (size_t)asset.num_vertices * 2);

        if (asset.index_size == 4) {
            const auto* idx = reinterpret_cast<const std::uint32_t*>(asset.index_buffer.data());
            cpuIndices.insert(cpuIndices.end(), idx, idx + asset.num_indices);
        } else {
            const auto* idx = reinterpret_cast<const std::uint16_t*>(asset.index_buffer.data());
            for (u32 i = 0; i < asset.num_indices; ++i) cpuIndices.push_back(idx[i]);
        }
        entities_.push_back(rec);
    }

    // GPU 缓冲重建。
    auto rebuild = [](RHIDeviceBase* dev, ResourceHandle& handle, const void* data,
                      u64 bytes, u32 bindFlags) {
        if (handle != handles::INVALID_RESOURCE) dev->DestroyBuffer(handle);
        BufferDesc desc{};
        desc.size = std::max<u64>(bytes, 16);
        // Static = unmappable in this RHI（见 DebugProbeSphereModule 同注）。
        desc.usage = GPUMemoryUsage::Dynamic;
        desc.memoryUsage = GPUMemoryUsage::Dynamic;
        desc.bindFlags = bindFlags;
        handle = dev->CreateBuffer(desc);
        if (handle == handles::INVALID_RESOURCE) return false;
        void* mapped = dev->MapBuffer(handle);
        if (!mapped) return false;
        std::memcpy(mapped, data, bytes);
        dev->UnmapBuffer(handle);
        return true;
    };
    const u32 storageFlags = static_cast<u32>(BufferUsageFlags::Storage);
    if (!entities_.empty()) {
        if (!rebuild(device_, positions_sb_, cpuPositions.data(),
                     cpuPositions.size() * sizeof(float), storageFlags) ||
            !rebuild(device_, uvs_sb_, cpuUvs.data(), cpuUvs.size() * sizeof(float),
                     storageFlags) ||
            !rebuild(device_, indices_ib_, cpuIndices.data(),
                     cpuIndices.size() * sizeof(std::uint32_t),
                     static_cast<u32>(BufferUsageFlags::Index))) {
            entities_.clear();  // 上传失败 → 本轮无实体（composite 走占位页）
            PRIMAL_LOG_STREAM(Error, "Render") << "[Lightmap] entity buffer upload failed" << std::endl;
            return;
        }
        // SSBO 描述符只在此刻写（帧内只读——避免 in-flight 重写失效）。
        DescData ssbos[2] = {
            {0, DescriptorType::StorageBuffer, positions_sb_},
            {1, DescriptorType::StorageBuffer, uvs_sb_},
        };
        UpdateDesc(device_, uv_ds_, ssbos, 2);
    }
    // [DIAG] PRIMAL_LIGHTMAP_CHECK_ENTITY=<n>：实体 n 的 SSBO 顶点共面性/
    // 索引跨界校验（判别 merged 缓冲错位——共面几何不可能双深度）。
    if (const char* ce = std::getenv("PRIMAL_LIGHTMAP_CHECK_ENTITY")) {
        const int enAll = std::atoi(ce);  // -1 = 全实体
        for (int en = (enAll < 0 ? 0 : enAll);
             en < (enAll < 0 ? (int)entities_.size() : enAll + 1); ++en) {
            const auto& e = entities_[en];
            float yMin = 1e30f, yMax = -1e30f;
            std::uint32_t badIdx = 0;
            for (std::uint32_t k = 0; k < e.index_count; ++k) {
                const std::uint32_t vi = cpuIndices[e.first_index + k];
                if (vi >= (cpuPositions.size() / 4) - e.position_offset) { ++badIdx; continue; }
                const float y = cpuPositions[(e.position_offset + vi) * 4 + 1];
                yMin = std::min(yMin, y); yMax = std::max(yMax, y);
            }
            float xMin = 1e30f, xMax = -1e30f, zMin = 1e30f, zMax = -1e30f;
            for (std::uint32_t k = 0; k < e.index_count; ++k) {
                const std::uint32_t vi = cpuIndices[e.first_index + k];
                if (vi >= (cpuPositions.size() / 4) - e.position_offset) continue;
                const float* P = &cpuPositions[(e.position_offset + vi) * 4];
                xMin = std::min(xMin, P[0]); xMax = std::max(xMax, P[0]);
                zMin = std::min(zMin, P[2]); zMax = std::max(zMax, P[2]);
            }
            PRIMAL_LOG_STREAM(Error, "Render")
                << "[LMCHK] entity=" << en << " idx=" << e.index_count
                << " firstIdx=" << e.first_index << " posOff=" << e.position_offset
                << " yRange=[" << yMin << "," << yMax << "]"
                << " xz=[" << xMin << "," << xMax << "]x[" << zMin << "," << zMax << "]"
                << " oobIdx=" << badIdx
                << " totalPos=" << (cpuPositions.size() / 4) << std::endl;
        }
    }
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[Lightmap] entities rebuilt: " << entities_.size();

}

void LightmapModule::AddUVPrePass(rendergraph::RenderGraph& graph, const FrameInputs& inputs) {
    if (!LightmapEnabledByEnv() || !HasActiveLightmap() || entities_.empty()) return;
    if (!inputs.gbuffer_depth_rg.IsValid()) return;

    // §9 取证：记录本帧 GBuffer 深度的物理句柄（DumpUvRT 与 UV RT .w 的
    // 重绘深度逐像素差分用）。
    if (auto* res = graph.GetResource(inputs.gbuffer_depth_rg)) {
        gbuffer_depth_phys_ = res->GetPhysicalHandle();
    }

    // ImportTexture（非 ImportResource）：携带 desc 注册为 Texture 类型，
    // 避免 RG 瞬态别名池复用本纹理内存（同 DeferredOutput 修复——
    // fusion 抑制模式下图集布局变化时实测被场景色覆写）。
    TextureDesc uvRtDesc{};
    uvRtDesc.size = {render_width_, render_height_, 1};
    uvRtDesc.format = DataFormat::RGBA16_Float;
    uvRtDesc.type = TextureType::Texture2D;
    uvRtDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    uv_rt_rg_ = graph.ImportTexture("LightmapUV_RT", uv_rt_, uvRtDesc);
    auto uvRG = uv_rt_rg_;
    auto depthRG = inputs.gbuffer_depth_rg;

    struct UvData {
        rendergraph::RGResourceHandle output;
    };
    graph.AddPass<UvData>("LightmapUV",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [uvRG, depthRG](UvData& data, rendergraph::RenderGraphBuilder& builder) {
            builder.SideEffect();
            builder.Read(depthRG, ResourceState::DepthStencilReadOnly);
            data.output = builder.Write(uvRG, ResourceState::RenderTarget);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({.texture = data.output, .loadOp = LoadAction::Clear,
                                     .storeOp = StoreAction::Store,
                                     .clearColor = {math::v4{0, 0, 0, 0}}});
            rpDesc.depthStencil.texture = depthRG;
            rpDesc.depthStencil.loadOp = LoadAction::Load;         // 保留场景深度
            rpDesc.depthStencil.storeOp = StoreAction::DontCare;   // 只读——不回写
            rpDesc.depthStencil.depthLoadOp = LoadAction::Load;
            rpDesc.depthStencil.depthStoreOp = StoreAction::DontCare;
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs](const UvData&, rendergraph::RenderGraphContext& context) {
            static std::uint64_t s_execCount = 0;
            if ((s_execCount++ % 600) == 0) {
                PRIMAL_LOG_STREAM(Info, "Render")
                    << "[Lightmap] UV pass running (frame group #" << s_execCount << ")"
                    << std::endl;
            }
            auto cmd = context.cmdBuffer;
            cmd->SetViewport({{0, 0}, {static_cast<float>(render_width_), static_cast<float>(render_height_)}, 0, 1});
            cmd->SetScissor({{0, 0}, {render_width_, render_height_}});

            cmd->BindGraphicsPipeline(uv_pipeline_);
            const DescriptorSetHandle sets[] = {uv_ds_};
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, uv_layout_, 0, 1, sets, 0, nullptr);
            cmd->BindIndexBuffer(indices_ib_, DataFormat::R32_UInt, 0);
            // §9/§10 取证旋钮：PRIMAL_LIGHTMAP_UVONLY=<实体序号> 只绘该实体；
            // "a:b" 区间语法（[a,b)）支持 LessEqual 二分点名——被 GBuffer
            // 深度匹配的实体（位级等值）才会写入，按 bot 带计数对半缩小。
            static const long uvOnlyLo = [] {
                const char* e = std::getenv("PRIMAL_LIGHTMAP_UVONLY");
                long lo = -1, hi = -1;
                if (e && std::sscanf(e, "%ld:%ld", &lo, &hi) == 2) return lo;
                return e ? std::atol(e) : -1L;
            }();
            static const long uvOnlyHi = [] {
                const char* e = std::getenv("PRIMAL_LIGHTMAP_UVONLY");
                long lo = -1, hi = -1;
                if (e && std::sscanf(e, "%ld:%ld", &lo, &hi) == 2) return hi;
                return e ? std::atol(e) + 1 : 0L;
            }();
            for (size_t ei = 0; ei < entities_.size(); ++ei) {
                if (uvOnlyLo >= 0 &&
                    !((long)ei >= uvOnlyLo && (long)ei < uvOnlyHi)) continue;
                const auto& e = entities_[ei];
                UvPushConstants push{};
                push.view_proj = inputs.view_projection;
                push.position_offset = e.position_offset;
                push.uv_offset = e.uv_offset;
                std::memcpy(push.scale_bias, e.scale_bias, sizeof(push.scale_bias));
                // §13 金标定（PRIMAL_LIGHTMAP_UVCALIB=1）：FS 输出归一化
                // gl_FragCoord.xy，读回行序方向一锤定音。
                static const float uvCalib =
                    std::getenv("PRIMAL_LIGHTMAP_UVCALIB") ? 1.0f : 0.0f;
                push.calib = uvCalib;
                cmd->PushConstants(uv_layout_,
                                   ShaderStage::Vertex | ShaderStage::Pixel,
                                   0, sizeof(push), &push);
                cmd->DrawIndexed(e.index_count, e.first_index, 0, 1, 0);
            }
        });

    // 编码器切换 flush（v3.1，2026-10-20 §6.5 二分定罪）：互斥态下 UV
    // pass(render) 与 composite(render) 直连时，uv_rt_ 的附件写入对紧随的
    // 采样不可见（COLOR→SRO 屏障已发出仍失效——MoltenVK tiler 的 render→
    // render 直连缺编码器边界落盘；composite 读到全零）。任何非 render
    // 编码器切入即恢复（二分：refl_specular(Compute) 单块放开即恢复，
    // fusion 两 render pass 放开仍死）。16B buffer→buffer 拷贝是最小代价
    // 的编码器切换；并存态不挂（保持 v2 图形状零影响）。
    if (inputs.encoder_flush &&
        flush_src_ != handles::INVALID_RESOURCE && flush_dst_ != handles::INVALID_RESOURCE) {
        struct FlushData {};
        graph.AddPass<FlushData>("LightmapUvEncoderFlush",
            rendergraph::RGPassType::Copy,
            rendergraph::RGPassCategory::Copy,
            [uvRG](FlushData&, rendergraph::RenderGraphBuilder& builder) {
                builder.SideEffect();
                builder.Read(uvRG, ResourceState::CopySource);
            },
            [this](const FlushData&, rendergraph::RenderGraphContext& context) {
                context.cmdBuffer->CopyBuffer(flush_src_, flush_dst_, 0, 0, 16);
            });
    }
}

bool LightmapModule::IsActive() const {
    return LightmapEnabledByEnv() && HasActiveLightmap() && !entities_.empty();
}

rendergraph::RGResourceHandle LightmapModule::AddCompositePass(
    rendergraph::RenderGraph& graph, const FrameInputs& inputs,
    rendergraph::RGResourceHandle scene_color_rg) {
    if (!LightmapEnabledByEnv() || !HasActiveLightmap()) {
        return rendergraph::RGResourceHandle{};
    }

    // 尺寸变更（窗口 resize / 超采样档切换）→ 重建中间纹理。渲染图每帧
    // 重新导入，无跨帧身份问题。
    if (inputs.render_width != render_width_ || inputs.render_height != render_height_) {
        render_width_ = inputs.render_width;
        render_height_ = inputs.render_height;
        TextureDesc rtDesc{};
        rtDesc.size = {render_width_, render_height_, 1};
        rtDesc.format = DataFormat::RGBA16_Float;
        rtDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
        if (output_tex_ != handles::INVALID_RESOURCE) device_->DestroyTexture(output_tex_);
        if (uv_rt_ != handles::INVALID_RESOURCE) device_->DestroyTexture(uv_rt_);
        output_tex_ = device_->CreateTexture(rtDesc);
        uv_rt_ = device_->CreateTexture(rtDesc);
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Lightmap] rt resized -> " << render_width_ << "x" << render_height_ << std::endl;
    }

    TextureDesc lmOutDesc{};
    lmOutDesc.size = {render_width_, render_height_, 1};
    lmOutDesc.format = DataFormat::RGBA16_Float;
    lmOutDesc.type = TextureType::Texture2D;
    lmOutDesc.usage = TextureUsage::RenderTarget | TextureUsage::ShaderResource;
    auto outRG = graph.ImportTexture("LightmapCompositeOut", output_tex_, lmOutDesc);
    // UV RT 的 RG 依赖：AddUVPrePass 已在本帧导入（写）——这里声明读，
    // RG 才会插 COLOR_ATTACHMENT→SHADER_READ 屏障。无实体的帧 UV RG
    // 无效（pre-pass 未加），composite 绑占位页。
    const rendergraph::RGResourceHandle uvReadRG = uv_rt_rg_;
    const bool uvValid = HasEntities() && uvReadRG.IsValid();
    // GBuffer albedo 的 RG 依赖（v3 albedo 调制）：有效则声明 Read——RG
    // 解析 ShaderResource 屏障（deferred/fusion 已读过，此处收敛状态）。
    const rendergraph::RGResourceHandle albedoReadRG = inputs.gbuffer_albedo_rg;

    struct CompositeData {
        rendergraph::RGResourceHandle output;
    };
    graph.AddPass<CompositeData>("LightmapComposite",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [scene_color_rg, outRG, uvReadRG, uvValid, albedoReadRG](CompositeData& data, rendergraph::RenderGraphBuilder& builder) {
            builder.SideEffect();
            builder.Read(scene_color_rg, ResourceState::ShaderResource);
            if (uvValid) builder.Read(uvReadRG, ResourceState::ShaderResource);
            if (albedoReadRG.IsValid()) builder.Read(albedoReadRG, ResourceState::ShaderResource);
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

            // 间接增益（PRIMAL_LIGHTMAP_GAIN，v3 语义：仅作用于间接叠加项
            // albedo·E_ind/π·gain——直射底图不受影响），不进入任何 Lumen 回路。
            // §19 标定（2026-10-21）：E 全量级由 baker 天空 1× 单位主导
            //（图集 max=π×5.5=17.2 自证），与运行时直射/显示档存在 §18 记录
            // 的 ~12× 单位差——gain=1 时 avgLum 贴适应下限（log2Lum=-6，
            // effExp=2.88 打满）→ 直射区贴白 54.4%。默认 1.0→4.0：E0 实测
            // 贴白 54.4%→0.00%，深暗桶亮度 42.7→56.6（OFF 参考 56.2），
            // darkBR 1.015→0.896（OFF 0.804）——与 OFF 观感对齐。env 可覆盖。
            float gain = 4.0f;
            if (const char* g = std::getenv("PRIMAL_LIGHTMAP_GAIN")) {
                const float v = static_cast<float>(std::atof(g));
                if (v >= 0.0f && v <= 8.0f) gain = v;
            }
            float debugMode = 0.0f;
            if (const char* d = std::getenv("PRIMAL_LIGHTMAP_DEBUG")) {
                if (d[0] == '1') debugMode = 1.0f;
                else if (d[0] == '2') debugMode = 2.0f;
                else if (d[0] == '3') debugMode = 3.0f;
                else if (d[0] == '4') debugMode = 4.0f;
            }
            // [§20 任务4] PRIMAL_LIGHTMAP_FILTER：间接采样柔化（默认 3 =
            // 抖动+4-tap 组合，FFT A/B 定优后转正；0=旧单 tap 线性采样）。
            //   1 = 亚纹素时域抖动（hash(pixel,frame)，TAA 时域累积=对
            //       lightmap 超采样，静止视角柔化 26 tex/m 格感）
            //   2 = 4-tap ±0.25 texel 足迹低通（无时域依赖）
            //   3 = 1+2 组合
            float filterMode = 3.0f;
            if (const char* f = std::getenv("PRIMAL_LIGHTMAP_FILTER")) {
                const float v = (float)std::atoi(f);
                if (v >= 0.0f && v <= 3.0f) filterMode = v;
            }
            // [§21 问题2a] PRIMAL_LIGHTMAP_DARKTINT：无有效间接暗像素暖化
            //（composite 内，不动 Lumen/deferred 共用文件）。默认 1.0：暗桶
            // 80.5% texel E≈0 裸露 deferred fill（B/R 0.968 中性偏蓝，§20.1），
            // tint (1,0.95,0.87) 全强度把暗桶 B/R 拉到 ≈0.84（OFF 参考 0.804，
            // 验收线 ≤0.90）；0=关闭（回 §20 行为）。A/B 定默认见 §21。
            float darkTint = 1.0f;
            if (const char* t = std::getenv("PRIMAL_LIGHTMAP_DARKTINT")) {
                const float v = static_cast<float>(std::atof(t));
                if (v >= 0.0f && v <= 1.0f) darkTint = v;
            }
            // 无实体时绑 z=0 占位页（全像素走原路径/debug 分支）。
            const ResourceHandle uvTex = HasEntities() ? uv_rt_ : uv_black_tex_;
            // [诊断] PRIMAL_LIGHTMAP_SWAPUV=1：交换 b1/b2 纹理。配 DEBUG=3
            // （读 b2）判别"binding 2 槽位失效"vs"uv_rt_ 图像采样内容坏"：
            // 交换后全绿（页的 a=1>0.5）→ 槽位正常，坏在 uv_rt_ 采样；
            // 仍全红 → binding 2 槽位本身失效。
            ResourceHandle b1Tex = lightmap_tex_;
            ResourceHandle b2Tex = uvTex;
            if (std::getenv("PRIMAL_LIGHTMAP_SWAPUV")) std::swap(b1Tex, b2Tex);
            // binding 4 = GBuffer albedo（v3 albedo 调制）；缺失绑 1×1 白
            //（albedo=1，间接项退化为纯 lm 增益——不失活）。
            const ResourceHandle albedoTex =
                inputs.gbuffer_albedo_tex != handles::INVALID_RESOURCE
                    ? inputs.gbuffer_albedo_tex
                    : albedo_white_tex_;
            DescData params[5] = {
                {0, DescriptorType::SampledImage, inputs.scene_color_tex},
                {1, DescriptorType::SampledImage, b1Tex},
                {2, DescriptorType::SampledImage, b2Tex},
                {3, DescriptorType::Sampler, static_cast<ResourceHandle>(sampler_)},
                {4, DescriptorType::SampledImage, albedoTex},
            };
            const u32 slot = inputs.frame_index % 3;
            UpdateDesc(device_, composite_ds_[slot], params, 5);
            cmd->BindGraphicsPipeline(composite_pipeline_);
            const DescriptorSetHandle sets[] = {composite_ds_[slot]};
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, composite_layout_, 0, 1, sets, 0, nullptr);
            const float pushParams[5] = {gain, debugMode, filterMode,
                                         (float)inputs.frame_index, darkTint};
            cmd->PushConstants(composite_layout_, ShaderStage::Pixel, 0, sizeof(pushParams),
                               pushParams);
            cmd->Draw(3, 0, 1, 0);

            // [2026-10-21 §19 后处理消融] PRIMAL_LIGHTMAP_DUMPFRAME：一次性
            // 读回 composite 输出（预 TAA 的 HDR RGBA16F，f16 位级）到
            // /tmp/lm_pp/composite_f<frame>.bin——归因矩阵的"数据侧"证据
            //（B/R、间接/直射量级、内亮 E 均在 HDR 线性域度量，后处理各级
            // 的 LDR 域由 SPONZA_CAPTURE 的 PPM 对照）。时序同 UVDUMP：触发
            // 帧 ≥360，读到的是 ≥3 帧前已 fence 完成的内容（静态场景等价）。
            if (std::getenv("PRIMAL_LIGHTMAP_DUMPFRAME") &&
                output_tex_ != handles::INVALID_RESOURCE && inputs.frame_index >= 360) {
                static bool s_frameDumped = false;
                if (!s_frameDumped) {
                    s_frameDumped = true;
                    const u32 w = inputs.render_width, h = inputs.render_height;
                    BufferDesc bd{};
                    bd.size = (u64)w * h * 8;
                    bd.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
                    bd.memoryUsage = GPUMemoryUsage::Readback;
                    ResourceHandle stage = device_->CreateBuffer(bd);
                    if (stage != handles::INVALID_RESOURCE) {
                        auto ch = device_->CreateCommandBuffer(CommandQueueType::Graphics);
                        auto* c2 = GetCommandBuffer(ch);
                        if (c2) {
                            c2->Begin();
                            BufferTextureCopyRegion region{};
                            region.bufferOffset = 0;
                            region.imageSubresource = {0, 0, 1};
                            region.imageOffset = {0, 0, 0};
                            region.imageExtent = {w, h, 1};
                            c2->CopyTextureToBuffer(output_tex_, stage, &region, 1);
                            c2->End();
                            QueueSubmitInfo sb{};
                            sb.cmdBuffer = ch;
                            SyncHandle sf = device_->CreateSync();
                            sb.signalFence = sf;
                            device_->Submit(sb);
                            device_->WaitForSync(sf, UINT32_MAX);
                            device_->DestroySync(sf);
                            device_->DestroyCommandBuffer(ch);
                            std::system("mkdir -p /tmp/lm_pp");
                            const std::string path =
                                "/tmp/lm_pp/composite_f" +
                                std::to_string(inputs.frame_index) + ".bin";
                            FILE* out = std::fopen(path.c_str(), "wb");
                            if (out) {
                                const void* p = device_->MapBuffer(stage);
                                if (p) {
                                    std::fwrite(p, 8, (size_t)w * h, out);
                                    device_->UnmapBuffer(stage);
                                }
                                std::fclose(out);
                                PRIMAL_LOG_STREAM(Info, "Render")
                                    << "[Lightmap][DUMPFRAME] " << w << "x" << h
                                    << " -> " << path << std::endl;
                            }
                        }
                        device_->DestroyBuffer(stage);
                    }
                }
            }

            // 一次性 UV RT 读回（PRIMAL_LIGHTMAP_UVDUMP[=frame]，取证用）：
            // 把 uv_rt_ 内容读回 CPU，统计 z>0.5 有效像素数并抽样 3 点——
            // 直接回答"UV pre-pass 写入是否存活到消费点"（前轮 §6.3 争议）。
            // 时机：execute lambda 在本帧命令缓冲录制期，此刻本帧尚未提交，
            // 读当前帧会拿到未初始化数据（实测 NaN）——触发帧取 ≥360，
            // 届时 ≥3 帧前的帧已完成 fence，读到的是已提交的 UV 内容
            // （静态场景逐帧等价）。
            if (HasEntities() && uv_rt_ != handles::INVALID_RESOURCE &&
                std::getenv("PRIMAL_LIGHTMAP_UVDUMP")) {
                static bool s_uvDumped = false;
                if (!s_uvDumped && inputs.frame_index >= 360) {
                    s_uvDumped = true;
                    DumpUvRT(inputs);
                }
            }
        });

    return outRG;
}

void LightmapModule::DumpUvRT(const FrameInputs& inputs) {
    const u32 w = inputs.render_width, h = inputs.render_height;
    BufferDesc bd{};
    bd.size = (u64)w * h * 8;
    bd.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
    bd.memoryUsage = GPUMemoryUsage::Readback;
    ResourceHandle stage = device_->CreateBuffer(bd);
    if (stage == handles::INVALID_RESOURCE) return;
    auto ch = device_->CreateCommandBuffer(CommandQueueType::Graphics);
    auto* c2 = GetCommandBuffer(ch);
    if (!c2) {
        device_->DestroyBuffer(stage);
        return;
    }
    c2->Begin();
    BufferTextureCopyRegion region{};
    region.bufferOffset = 0;
    region.imageSubresource = {0, 0, 1};
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {w, h, 1};
    c2->CopyTextureToBuffer(uv_rt_, stage, &region, 1);
    c2->End();
    QueueSubmitInfo submit{};
    submit.cmdBuffer = ch;
    SyncHandle fence = device_->CreateSync();
    submit.signalFence = fence;
    device_->Submit(submit);
    device_->WaitForSync(fence, UINT32_MAX);
    device_->DestroySync(fence);
    device_->DestroyCommandBuffer(ch);

    const u16* p = static_cast<const u16*>(device_->MapBuffer(stage));
    if (!p) {
        device_->DestroyBuffer(stage);
        return;
    }
    auto halfToFloat = [](u16 x) {
        u32 sign = (u32)(x & 0x8000u) << 16, expo = (x & 0x7C00u) >> 10, man = x & 0x03FFu;
        u32 bits;
        if (expo == 0) bits = sign;
        else if (expo == 31) bits = sign | 0x7F800000u | (man << 13);
        else bits = sign | ((expo - 15u + 127u) << 23) | (man << 13);
        f32 f;
        std::memcpy(&f, &bits, 4);
        return f;
    };
    u64 valid = 0;
    const u64 total = (u64)w * h;
    u64 minX = ~0ull, minY = ~0ull, maxX = 0, maxY = 0;
    for (u64 i = 0; i < total; ++i) {
        if (halfToFloat(p[i * 4 + 2]) > 0.5f) {
            ++valid;
            const u64 x = i % w, y = i / w;
            if (x < minX) minX = x;
            if (x > maxX) maxX = x;
            if (y < minY) minY = y;
            if (y > maxY) maxY = y;
        }
    }
    // §18 验收②（UVDUMP=2）：marker 采样落空区统计——每个 valid marker 的
    // atlas UV 双线性采样页 A 通道（与 composite 同采样语义），A≤0.25 计
    // 落空（该像素无间接补光——z=-8 整屏暗的直接成因度量）。
    const int uvDumpLevel = std::atoi(std::getenv("PRIMAL_LIGHTMAP_UVDUMP"));
    // [§20 取证] UVDUMP=3：uv_rt 全帧 RGBA16F 落盘（/tmp/lm_q4/uvrt_f<frame>.bin）
    // ——Python 后处理把屏幕暗桶像素映射回 atlas texel（天空占比统计/热图）。
    if (uvDumpLevel >= 3) {
        std::system("mkdir -p /tmp/lm_q4");
        const std::string path = "/tmp/lm_q4/uvrt_f" + std::to_string(inputs.frame_index) + ".bin";
        if (FILE* mf = std::fopen(path.c_str(), "wb")) {
            std::fwrite(p, 8, (size_t)w * h, mf);
            std::fclose(mf);
            PRIMAL_LOG_STREAM(Info, "Render")
                << "[Lightmap][UVDUMP] full uvrt " << w << "x" << h << " -> " << path
                << std::endl;
        }
    }
    u64 markerVoid = 0, markerN = 0;
    if (uvDumpLevel >= 2 && !page_a_.empty() && page_w_ && page_h_ &&
        page_a_.size() == (size_t)page_w_ * (size_t)page_h_) {
        const auto bilinearA = [&](f32 u, f32 v) {
            const f32 fx = u * (f32)page_w_ - 0.5f, fy = v * (f32)page_h_ - 0.5f;
            const s32 x0 = (s32)std::floor(fx), y0 = (s32)std::floor(fy);
            const f32 tx = fx - (f32)x0, ty = fy - (f32)y0;
            const auto fetch = [&](s32 xx, s32 yy) {
                xx = std::clamp(xx, 0, (s32)page_w_ - 1);
                yy = std::clamp(yy, 0, (s32)page_h_ - 1);
                return page_a_[(u64)yy * page_w_ + xx];
            };
            const f32 a = fetch(x0, y0) * (1 - tx) + fetch(x0 + 1, y0) * tx;
            const f32 b = fetch(x0, y0 + 1) * (1 - tx) + fetch(x0 + 1, y0 + 1) * tx;
            return a * (1 - ty) + b * ty;
        };
        for (u64 i = 0; i < total; ++i) {
            if (halfToFloat(p[i * 4 + 2]) <= 0.5f) continue;
            ++markerN;
            if (bilinearA(halfToFloat(p[i * 4 + 0]), halfToFloat(p[i * 4 + 1])) <= 0.25f)
                ++markerVoid;
        }
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[Lightmap][UVDUMP] markerInVoid=" << markerVoid << "/" << markerN
            << " (" << (markerN ? 100.0 * (double)markerVoid / (double)markerN : 0.0)
            << "%) — bilinear A<=0.25 at marker UV" << std::endl;
    }
    // 定点采样（显示 1280x720 标定 → render 等比）：中心 / 地板池 (431,416)
    // / 墙区 (200,300)。x 按 1280 归一（720 是 y 基准——首轮曾误用致样点
    // 偏移，取到池外）。
    const auto dispIdx = [&](u32 dx, u32 dy) {
        return ((u64)(h * dy / 720) * w + (u64)w * dx / 1280) * 4;
    };
    const u64 sCenter = dispIdx(640, 360);
    const u64 sFloor = dispIdx(431, 416);
    const u64 sWall = dispIdx(200, 300);
    const f32 fU = halfToFloat(p[sFloor]), fV = halfToFloat(p[sFloor + 1]);
    PRIMAL_LOG_STREAM(Info, "Render")
        << "[Lightmap][UVDUMP] frame=" << inputs.frame_index << " " << w << "x" << h
        << " valid(z>0.5)=" << valid << "/" << total
        << " bbox x[" << minX << "," << maxX << "] y[" << minY << "," << maxY << "]"
        << " center=(" << halfToFloat(p[sCenter]) << "," << halfToFloat(p[sCenter + 1]) << ")"
        << " floorUV=(" << fU << "," << fV << ")"
        << " wall=(" << halfToFloat(p[sWall]) << "," << halfToFloat(p[sWall + 1]) << ")"
        << std::endl;

    // §9 取证：GBuffer 深度读回 + 与 UV RT .w（重绘深度）逐像素差分分桶。
    // delta = redraw_z - gb_z：>0 即 LessEqual 拒绝量。分桶维度：幅度（相对
    // gb_z）× 屏幕纵向三带（近场/中场/远场代理）。DEPTHALWAYS=1 时全部
    // 重绘落盘（含被拒像素），差分图完整。
    if (gbuffer_depth_phys_ != handles::INVALID_RESOURCE) {
        BufferDesc dbd{};
        dbd.size = (u64)w * h * 4;
        dbd.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
        dbd.memoryUsage = GPUMemoryUsage::Readback;
        ResourceHandle dstage = device_->CreateBuffer(dbd);
        if (dstage != handles::INVALID_RESOURCE) {
            auto dch = device_->CreateCommandBuffer(CommandQueueType::Graphics);
            auto* dc = GetCommandBuffer(dch);
            if (dc) {
                dc->Begin();
                BufferTextureCopyRegion dr{};
                dr.bufferOffset = 0;
                dr.imageSubresource = {0, 0, 1};
                dr.imageOffset = {0, 0, 0};
                dr.imageExtent = {w, h, 1};
                dc->CopyTextureToBuffer(gbuffer_depth_phys_, dstage, &dr, 1);
                dc->End();
                QueueSubmitInfo ds{};
                ds.cmdBuffer = dch;
                SyncHandle dfence = device_->CreateSync();
                ds.signalFence = dfence;
                device_->Submit(ds);
                device_->WaitForSync(dfence, UINT32_MAX);
                device_->DestroySync(dfence);
            }
            device_->DestroyCommandBuffer(dch);
            const f32* dp = static_cast<const f32*>(device_->MapBuffer(dstage));
            if (dp) {
                // 幅度桶（相对深度差）× 纵向三带。
                constexpr int kBands = 3, kBuckets = 7;
                u64 hist[kBands][kBuckets] = {};
                u64 bandCount[kBands] = {};
                f32 bandMaxDelta[kBands] = {};
                const f32 edges[kBuckets] = {1e-7f, 1e-6f, 1e-5f, 1e-4f, 1e-3f, 1e-2f};
                for (u64 y = 0; y < h; ++y) {
                    const int band = (int)(y * kBands / h);
                    for (u64 x = 0; x < w; ++x) {
                        const u64 i = y * w + x;
                        if (halfToFloat(p[i * 4 + 2]) <= 0.5f) continue;
                        const f32 rz = halfToFloat(p[i * 4 + 3]);  // 重绘深度
                        const f32 gz = dp[i];
                        if (gz <= 0.f || gz >= 1.f || rz <= 0.f || rz >= 1.f) continue;
                        const f32 rel = (rz - gz) / gz;
                        int b = 0;
                        while (b < kBuckets - 1 && rel > edges[b]) ++b;
                        ++hist[band][b];
                        ++bandCount[band];
                        if (rel > bandMaxDelta[band]) bandMaxDelta[band] = rel;
                    }
                }
                const char* bandNames[kBands] = {"top", "mid", "bot"};
                const char* bNames[kBuckets] = {"<=1e-7", "1e-6", "1e-5", "1e-4",
                                                "1e-3", "1e-2", ">1e-2"};
                for (int band = 0; band < kBands; ++band) {
                    std::string rows;
                    for (int b = 0; b < kBuckets; ++b) {
                        if (b) rows += ",";
                        rows += std::to_string(hist[band][b]);
                    }
                    PRIMAL_LOG_STREAM(Info, "Render")
                        << "[Lightmap][UVDIFF] " << bandNames[band]
                        << " n=" << bandCount[band]
                        << " maxRel=" << bandMaxDelta[band]
                        << " buckets(<=1e-7,1e-6,1e-5,1e-4,1e-3,1e-2,>1e-2)=" << rows
                        << std::endl;
                }
                // 探针像素的绝对深度对照。
                auto probeZ = [&](const char* tag, u64 idx) {
                    PRIMAL_LOG_STREAM(Info, "Render")
                        << "[Lightmap][UVDIFF] " << tag
                        << " redraw=" << halfToFloat(p[idx + 3])
                        << " gb=" << dp[idx / 4]
                        << std::endl;
                };
                probeZ("center", sCenter);
                probeZ("floorProbe", sFloor);
                probeZ("wall", sWall);
                // §10 下带点名探针（ON 图黑带：display y 446/560/680 @ x 640）。
                // §11 对账：同步打印纹理整数像素坐标（display→texture 换算
                // 自证——x 按 1280、y 按 720 基准等比）。
                auto probeZXY = [&](const char* tag, u64 idx) {
                    const u64 px = (idx / 4) % w, py = (idx / 4) / w;
                    PRIMAL_LOG_STREAM(Info, "Render")
                        << "[Lightmap][UVDIFF] " << tag
                        << " px=(" << px << "," << py << ")"
                        << " redraw=" << halfToFloat(p[idx + 3])
                        << " gb=" << dp[idx / 4]
                        << std::endl;
                };
                probeZXY("bot1", dispIdx(640, 446));
                probeZXY("bot2", dispIdx(640, 560));
                probeZXY("bot3", dispIdx(640, 680));
                // [2026-10-21 远平面黑带轮] 上缘探针（display y 4/12/24/40
                // @ x 640）：远面 marker 的重绘深度与 GBuffer 是否同面——
                // 同面 delta≈-eps_rel，异面（slack 内远表面）更负。
                probeZXY("top1", dispIdx(640, 4));
                probeZXY("top2", dispIdx(640, 12));
                probeZXY("top3", dispIdx(640, 24));
                probeZXY("top4", dispIdx(640, 40));
                // [DIAG] PRIMAL_LIGHTMAP_DUMPPAIR：.w 与 gb 深度场各导出
                // 灰度 PPM（/tmp/lm_mirror/{uvw,gbw}.ppm）供视觉对照。
                if (std::getenv("PRIMAL_LIGHTMAP_DUMPPAIR")) {
                    auto writeDepthPpm = [&](const char* path, const float* src) {
                        FILE* f = std::fopen(path, "wb");
                        if (!f) return;
                        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
                        std::vector<u8> row(w * 3);
                        for (u32 y = 0; y < h; ++y) {
                            for (u32 x = 0; x < w; ++x) {
                                const float z = src[y * w + x];
                                const u8 v = (z <= 0.f || z >= 1.f)
                                                 ? 0
                                                 : (u8)std::lround((z - 0.85f) / 0.15f * 255.f);
                                row[x * 3] = row[x * 3 + 1] = row[x * 3 + 2] = v;
                            }
                            std::fwrite(row.data(), 1, row.size(), f);
                        }
                        std::fclose(f);
                    };
                    std::vector<float> wbuf((u64)w * h);
                    for (u64 i = 0; i < (u64)w * h; ++i) wbuf[i] = halfToFloat(p[i * 4 + 3]);
                    writeDepthPpm("/tmp/lm_mirror/uvw.ppm", wbuf.data());
                    writeDepthPpm("/tmp/lm_mirror/gbw.ppm", dp);
                    PRIMAL_LOG_STREAM(Info, "Render")
                        << "[Lightmap][DUMPPAIR] written /tmp/lm_mirror/{uvw,gbw}.ppm"
                        << std::endl;
                }
                // [DIAG] 镜像相关性判别：.w（重绘深度，ALWAYS 全落盘）与 gb
                // 的直接 vs 垂直镜像平均绝对差——裁决读回/写入坐标错位
                // （MoltenVK tiler 一族）vs 光栅化深度真错。纯统计，零开销。
                {
                    double sumDir = 0, sumMir = 0;
                    u64 nDir = 0, nMir = 0;
                    for (u64 y = 0; y + 1 < (u64)h; ++y) {
                        const u64 ym = (u64)h - 1 - y;
                        for (u64 x = 0; x < (u64)w; x += 4) {
                            const u64 iA = y * w + x, iB = ym * w + x;
                            const f32 rzA = halfToFloat(p[iA * 4 + 3]);
                            const f32 gzA = dp[iA];
                            const f32 gzB = dp[iB];
                            if (gzA > 0.f && gzA < 1.f && rzA > 0.f && rzA < 1.f) {
                                sumDir += std::fabs(rzA - gzA); ++nDir;
                            }
                            if (gzB > 0.f && gzB < 1.f && rzA > 0.f && rzA < 1.f) {
                                sumMir += std::fabs(rzA - gzB); ++nMir;
                            }
                        }
                    }
                    PRIMAL_LOG_STREAM(Info, "Render")
                        << "[Lightmap][UVMIRROR] direct MAD=" << (nDir ? sumDir / nDir : -1.0)
                        << " (n=" << nDir << ")  mirrored MAD="
                        << (nMir ? sumMir / nMir : -1.0) << " (n=" << nMir << ")"
                        << std::endl;
                }
                device_->UnmapBuffer(dstage);
            }
            device_->DestroyBuffer(dstage);
        }
    }
    device_->UnmapBuffer(stage);
    device_->DestroyBuffer(stage);

    // 追加：烘焙页在 floorUV 处的值（回答"composite 对地板池会替换成什么"）。
    if (fU > 0.f && fV > 0.f && lightmap_tex_ != handles::INVALID_RESOURCE) {
        BufferDesc pb2{};
        pb2.size = 8;
        pb2.bindFlags = static_cast<u32>(BufferUsageFlags::TransferDst);
        pb2.memoryUsage = GPUMemoryUsage::Readback;
        ResourceHandle stage2 = device_->CreateBuffer(pb2);
        if (stage2 != handles::INVALID_RESOURCE) {
            auto ch2 = device_->CreateCommandBuffer(CommandQueueType::Graphics);
            auto* c3 = GetCommandBuffer(ch2);
            if (c3) {
                c3->Begin();
                BufferTextureCopyRegion pr{};
                pr.bufferOffset = 0;
                pr.imageSubresource = {0, 0, 1};
                pr.imageOffset = {(s32)(fU * (f32)(page_w_ - 1)),
                                  (s32)(fV * (f32)(page_h_ - 1)), 0};
                pr.imageExtent = {1, 1, 1};
                c3->CopyTextureToBuffer(lightmap_tex_, stage2, &pr, 1);
                c3->End();
                QueueSubmitInfo s2{};
                s2.cmdBuffer = ch2;
                SyncHandle f2 = device_->CreateSync();
                s2.signalFence = f2;
                device_->Submit(s2);
                device_->WaitForSync(f2, UINT32_MAX);
                device_->DestroySync(f2);
                device_->DestroyCommandBuffer(ch2);
                if (const u16* q = static_cast<const u16*>(device_->MapBuffer(stage2))) {
                    PRIMAL_LOG_STREAM(Info, "Render")
                        << "[Lightmap][PAGEDUMP] page(" << (u32)(fU * (page_w_ - 1))
                        << "," << (u32)(fV * (page_h_ - 1))
                        << ") rgb=(" << halfToFloat(q[0]) << "," << halfToFloat(q[1])
                        << "," << halfToFloat(q[2]) << ")" << std::endl;
                    device_->UnmapBuffer(stage2);
                }
            }
            device_->DestroyBuffer(stage2);
        }
    }
}

} // namespace primal::graphics
