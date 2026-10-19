#include "FusionCompositeModule.h"
#include "LightmapModule.h"
#include "Graphics/RenderGraph/RenderGraph.h"
#include "Graphics/Lumen/ScreenProbes/ScreenProbeGIPass.h"
#include "Graphics/Lumen/DDGISlotMap.h"
#include "Graphics/RenderGraph/RenderGraphBuilder.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include <iostream>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <utility>
#include <vector>
#include <cstdio>
#include <string>
#include "Engine/Common/CoreLogStream.h"

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
    // sun-audit P2: UniformBuffer 描述符走 bufferInfo（imageInfo 为 NULL 时
    // vkUpdateDescriptorSets 报 VUID-00324）。
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
            bufferInfos[i].range = 16;  // GI 融合增益 UBO（float）
            writes[i].bufferInfo = &bufferInfos[i];
        } else {
            imageInfos[i].imageView = params[i].resource;
            imageInfos[i].imageLayout = ResourceState::ShaderResource;
            writes[i].imageInfo = &imageInfos[i];
        }
    }
    device->UpdateDescriptorSets(count, writes.data());
}

// GI_ROI_AUDIT is intentionally strict about texture layout.  The readback
// size is derived from the live RHI format; an unknown/unsupported format is
// rejected instead of being treated as a guessed RGBA16F/BGRA8 source.
static const char* RoiAuditFormatName(DataFormat format) {
    switch (format) {
        case DataFormat::R16_Float:    return "r16f";
        case DataFormat::RGBA16_Float: return "rgba16f";
        case DataFormat::BGRA8_UNorm:  return "bgra8";
        case DataFormat::RGBA8_UNorm:  return "rgba8";
        default:                       return "unknown";
    }
}

static u32 RoiAuditFormatBytes(DataFormat format) {
    switch (format) {
        case DataFormat::R16_Float:    return 2;
        case DataFormat::RGBA16_Float: return 8;
        case DataFormat::BGRA8_UNorm:  return 4;
        case DataFormat::RGBA8_UNorm:  return 4;
        default:                       return 0;
    }
}

static u32 RoiAuditRequestedFrame() {
    const char* frame = std::getenv("GI_ROI_FRAME");
    return frame ? u32(std::max(0, std::atoi(frame))) : 350u;
}

// RGBA16F 输出按四竖条统计亮度 mean/max + RGB 分量 mean（颜色对齐用），与
// SC_DUMP_CARDS 的 IND_STATS 同一数据面（ddgi|ssgi|spgi|full），供异步路径复用。
static void PrintIndirectStripStats(const u16* p, u32 w, u32 h, const char* prefix, u32 tick) {
    auto f16 = [](u16 h16) -> float {
        u32 sign = (h16 >> 15) & 1u, exp = (h16 >> 10) & 0x1Fu, mant = h16 & 0x3FFu;
        if (exp == 0u) return 0.0f;
        u32 bits = (sign << 31) | ((exp + 112u) << 23) | (mant << 13);
        float f; std::memcpy(&f, &bits, 4); return f;
    };
    const char* names[4] = { "ddgi-strip", "ssgi-strip", "spgi-strip", "full-strip" };
    for (int q = 0; q < 4; ++q) {
        u32 x0 = w * q / 4, x1 = w * (q + 1) / 4;
        double sum = 0.0, sr = 0.0, sg = 0.0, sb = 0.0, sa = 0.0;
        float mx = 0.0f; u64 n = 0;
        for (u32 y = 0; y < h; y += 4)
            for (u32 x = x0; x < x1; x += 4) {
                const u16* t = p + ((size_t)y * w + x) * 4;
                float r = f16(t[0]), g = f16(t[1]), b = f16(t[2]);
                if (std::isnan(r)) continue;
                sum += 0.2126f * r + 0.7152f * g + 0.0722f * b;
                sr += r; sg += g; sb += b; sa += f16(t[3]);
                mx = std::max(mx, std::max(r, std::max(g, b)));
                ++n;
            }
        if (n) PRIMAL_LOG_STREAM(Error, "Render") << "[" << prefix << "] tick=" << tick << " " << names[q]
                         << " mean=" << (sum / n)
                         << " rgb=(" << (sr / n) << "," << (sg / n) << "," << (sb / n) << ")"
                         << " a=" << (sa / n)
                         << " max=" << mx << std::endl;
    }
}

bool FusionCompositeModule::Initialize(RHIDeviceBase* device,
                                         ShaderHandle indirect_ps, ShaderHandle composite_vs,
                                         ShaderHandle composite_ps,
                                         u32 render_width, u32 render_height) {
    device_ = device;
    render_width_ = render_width;
    render_height_ = render_height;
    vertex_shader_ = composite_vs;

    // Vulkan needs an explicit Sampler descriptor (Metal uses implicit samplers).
    // Create a linear-clamp sampler for the Fusion passes.
    const bool isVk = device->GetPlatform() == RHIPlatform::Vulkan;
    if (isVk) {
        SamplerDesc sDesc{};
        sDesc.minFilter = FilterMode::Linear;
        sDesc.magFilter = FilterMode::Linear;
        sDesc.mipFilter = FilterMode::Linear;
        sDesc.addressU = TextureAddressMode::Clamp;
        sDesc.addressV = TextureAddressMode::Clamp;
        sDesc.addressW = TextureAddressMode::Clamp;
        sDesc.comparisonFunc = ComparisonFunc::Never;
        default_sampler_ = device->CreateSampler(sDesc);
        if (default_sampler_ == handles::INVALID_SAMPLER) {
            PRIMAL_LOG_STREAM(Error, "Render") << "[FusionComposite] Failed to create default_sampler_" << std::endl;
        }
    }

    // --- Pass 1: FusionIndirect (full-res, 5 texture reads + 1 sampler on Vulkan) ---
    {
        const u32 indirectBindingCount = isVk ? 7 : 5;
        DescriptorSetLayoutBinding bindings[7];
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // SSGI
        bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // DDGI
        bindings[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // SPGI
        bindings[3] = {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // albedo
        bindings[4] = {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // SSAO
        if (isVk) {
            bindings[5] = {5, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr};   // sampler
            // sun-audit P2 用户反馈（间接光过弱）：GI 融合增益运行时旋钮
            bindings[6] = {6, DescriptorType::UniformBuffer, 1, ShaderStage::Pixel, nullptr};
        }
        indirect_set_layout_ = device->CreateDescriptorSetLayout({indirectBindingCount, bindings});
        indirect_layout_ = device->CreatePipelineLayout({1, &indirect_set_layout_});

        for (int i = 0; i < 3; ++i) {
            indirect_ds_[i] = device->CreateDescriptorSet({indirect_set_layout_});
            // sun-audit P2: GI 融合增益 UBO（16B float；PRIMAL_GI_INDIRECT_GAIN）
            if (isVk) {
                BufferDesc gcb{};
                gcb.size = 16;
                gcb.type = BufferType::Constant;
                gcb.memoryUsage = GPUMemoryUsage::Dynamic;
                indirect_gain_cb_[i] = device->CreateBuffer(gcb);
            }
        }

        // Full-res indirect output.  SPGI and material data contain thin
        // receiver geometry that cannot survive an unguided half-res resolve.
        // DDGI itself remains half-res and is linearly sampled as low-frequency
        // lighting by the fragment shader.
        TextureDesc indDesc{};
        indDesc.size = {render_width, render_height, 1};
        indDesc.format = DataFormat::RGBA16_Float;
        indDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget |
                        TextureUsage::CopySource;  // TEMP: IND_STATS readback
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

    // --- Pass 2: FusionComposite (full-res, 8 texture reads + 1 sampler on Vulkan) ---
    {
        // bindings: 0=scene, 1=indirect, 2=volume_scatter, 3=ssr, 4=orm,
        //           5=albedo, 6=ddgi_specular (T4.3), [7=sampler(Vk)],
        //           8=refl_specular (R4/R5 offscreen specular)
        const u32 compositeBindingCount = isVk ? 9 : 7;
        DescriptorSetLayoutBinding bindings[9];
        bindings[0] = {0, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // scene
        bindings[1] = {1, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // indirect
        bindings[2] = {2, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // volume_scatter
        bindings[3] = {3, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // ssr
        bindings[4] = {4, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // orm
        bindings[5] = {5, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // albedo (T4.3)
        bindings[6] = {6, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr};  // ddgi_specular (T4.3)
        if (isVk) {
            bindings[7] = {7, DescriptorType::Sampler, 1, ShaderStage::Pixel, nullptr};   // sampler
            bindings[8] = {8, DescriptorType::SampledImage, 1, ShaderStage::Pixel, nullptr}; // refl_specular
        }
        composite_set_layout_ = device->CreateDescriptorSetLayout({compositeBindingCount, bindings});
        composite_layout_ = device->CreatePipelineLayout({1, &composite_set_layout_});

        for (int i = 0; i < 3; ++i)
            composite_ds_[i] = device->CreateDescriptorSet({composite_set_layout_});

        // Full-res output (BGRA8_UNorm)
        TextureDesc outDesc{};
        outDesc.size = {render_width, render_height, 1};
        outDesc.format = DataFormat::RGBA16_Float;  // HDR — FinalBlit does tonemap
        outDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
        for (int i = 0; i < 3; ++i)
            fusion_output_[i] = device->CreateTexture(outDesc);

        // Volume identity texture: 1x1 RGBA16_Float = (0,0,0,1)
        // transmittance=1 means scene passes through unchanged when no volume effect
        {
            TextureDesc desc{};
            desc.size = {1, 1, 1};
            desc.format = DataFormat::RGBA16_Float;
            desc.usage = TextureUsage::ShaderResource | TextureUsage::CopyDest;
            volume_identity_tex_ = device->CreateTexture(desc);

            if (volume_identity_tex_ != handles::INVALID_RESOURCE) {
                // Upload (0,0,0,1) via staging buffer + CopyBufferToTexture
                BufferDesc sbDesc{};
                sbDesc.size = 256; // Metal requires 256-byte row alignment
                sbDesc.usage = GPUMemoryUsage::Staging;
                sbDesc.memoryUsage = GPUMemoryUsage::Staging;
                auto staging = device->CreateBuffer(sbDesc);

                if (staging != handles::INVALID_RESOURCE) {
                    auto* mapped = static_cast<float*>(device->MapBuffer(staging));
                    if (mapped) {
                        memset(mapped, 0, 256);
                        // RGBA16_Float: each channel is 2 bytes (half float)
                        auto* halfData = reinterpret_cast<u16*>(mapped);
                        halfData[3] = 0x3C00; // A = 1.0 (half-float encoding)
                        device->UnmapBuffer(staging);

                        auto cmdHandle = device->CreateCommandBuffer(CommandQueueType::Graphics);
                        if (cmdHandle != handles::INVALID_COMMAND_BUFFER) {
                            auto* cmd = GetCommandBuffer(cmdHandle);
                            cmd->Begin();
                            BufferTextureCopyRegion region{};
                            region.bufferOffset = 0;
                            region.bufferRowLength = 256 / 4; // aligned row in pixels (RGBA16 = 4 bytes/pixel for 2 channels... actually 8 bytes for RGBA16)
                            region.imageSubresource = {0, 0, 1};
                            region.imageExtent = {1, 1, 1};
                            cmd->CopyBufferToTexture(staging, volume_identity_tex_, &region, 1);
                            // P0-B: 上传后回置 SRO——该 1×1 纹理被 ToneMapping
                            // binding 2 作为 SampledImage 采样（承诺 SRO），
                            // CopyBufferToTexture 把它停在 TRANSFER_DST，
                            // 帧 1-7 每帧 submit 报 "expects SRO, got
                            // TRANSFER_DST"。truth-based，恰好发一次转换。
                            {
                                ResourceBarrier restoreBar{};
                                restoreBar.resource = volume_identity_tex_;
                                restoreBar.beforeState = ResourceState::CopyDest;
                                restoreBar.afterState  = ResourceState::ShaderResource;
                                restoreBar.subresource = 0xFFFFFFFF;
                                restoreBar.queueFamily = 0xFFFFFFFF;
                                cmd->InsertBarrier(&restoreBar, 1);
                            }
                            cmd->End();
                            cmd->Submit();
                            cmd->WaitForCompletion();
                            device->DestroyCommandBuffer(cmdHandle);
                        }
                    }
                    device->DestroyBuffer(staging);
                }
            }
        }

        if (composite_vs != handles::INVALID_SHADER && composite_ps != handles::INVALID_SHADER) {
            GraphicsPipelineDesc pd{};
            pd.layout = composite_layout_;
            pd.vertexShader = composite_vs;
            pd.pixelShader = composite_ps;
            pd.renderTargetFormats[0] = DataFormat::RGBA16_Float;  // HDR — FinalBlit does tonemap
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

void FusionCompositeModule::Shutdown() {
    for (auto& source : roi_audit_sources_) {
        if (device_ && source.staging!=handles::INVALID_RESOURCE)
            device_->DestroyBuffer(source.staging);
        source={};
    }
    for (u32 slot = 0; slot < 3; ++slot) {
        for (u32 binding = 0; binding < kRoiAuditCompositeBindingCount; ++binding)
            roi_audit_composite_bound_[slot][binding] = handles::INVALID_RESOURCE;
        roi_audit_composite_bound_frame_[slot] = 0;
        roi_audit_composite_bound_valid_[slot] = false;
    }
    roi_audit_queued_=roi_audit_dumped_=false;
    for (auto& valid : roi_audit_indirect_bound_valid_) valid=false;
    if (device_ && volume_identity_tex_ != handles::INVALID_RESOURCE) {
        device_->DestroyTexture(volume_identity_tex_);
        volume_identity_tex_ = handles::INVALID_RESOURCE;
    }
    for (auto& staging : async_stats_staging_) {
        if (device_ && staging != handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(staging);
            staging = handles::INVALID_RESOURCE;
        }
    }
    for (auto* ring : {&primary_stats_staging_, &fusion_stats_staging_, &ddgi_stats_staging_}) {
        for (auto& staging : *ring) {
            if (device_ && staging != handles::INVALID_RESOURCE) {
                device_->DestroyBuffer(staging);
                staging = handles::INVALID_RESOURCE;
            }
        }
    }
}

// ============================================================================
// sun-audit P1（暗地面 GI 逐段查，§7）: GI_LADDER_DUMP 能量台账。
//
// 对固定屏幕 ROI 逐像素读回数值链：
//   vis（直射可见性，ShadowFilter 半分辨率 R8）→ direct（屏幕直射 HDR）
//   → ddgi / spgi（探针积分阶段）→ indirect（FusionIndirect 输出=探针融合），
// 每行附世界射线（invVP 反投影；锁定机位下 ROI 像素 ⇔ 固定世界位置）。
// ROI 级判定（审计分段口径）：
//   A trace_or_sc_source_dark —— 参考带供给在而目标 ROI 探针段≈0：
//       查 SC texel/卡片深度/SDF 覆盖/太阳方向（SC atlas 本体另用
//       SC_CARDVIEW / SC_DUMP_CARDS 检查）；
//   B fusion_darkening —— 目标 ROI 探针段有光而融合输出衰减：
//       查 SH 卷积/π/albedo 重复乘/置信度归一；
//   C indirect_ok —— HDR 间接光存在：显示仍暗则查曝光/色调映射。
// 读回沿用 SC_DUMP_CARDS 的同步单发惯例：读上一完成帧的三缓冲输出
// （BeginFrame 的 per-frame fence 已保证完成）；vis 为单缓冲，读到的是
// 上一帧内容——锁定机位+静态场景下逐帧一致。
// 环境：GI_LADDER_DUMP=1、GI_LADDER_FRAME（默认 360）、GI_LADDER_ROI /
// GI_LADDER_ROI2（归一 x0,y0,x1,y1；默认 = 审计中央地面 / 右立面参考带）、
// GI_LADDER_STRIDE（默认 8）。
// ============================================================================
namespace {

float GiLadderH16(u16 h) {
    const u32 sign = (h >> 15) & 1u, ex = (h >> 10) & 0x1Fu, mant = h & 0x3FFu;
    if (ex == 0u) return 0.0f;
    u32 bits = (sign << 31) | ((ex + 112u) << 23) | (mant << 13);
    float f; std::memcpy(&f, &bits, 4); return f;
}

struct GiLadderRoi { u32 x0, y0, x1, y1; };

GiLadderRoi GiLadderParseRoi(const char* envName, const char* def, u32 w, u32 h) {
    const char* s = std::getenv(envName);
    float v[4] = {0, 0, 0, 0};
    if (!s || std::sscanf(s, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) != 4) {
        std::sscanf(def, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
    }
    for (float& e : v) e = std::min(std::max(e, 0.0f), 1.0f);
    if (v[2] < v[0]) std::swap(v[0], v[2]);
    if (v[3] < v[1]) std::swap(v[1], v[3]);
    GiLadderRoi r{};
    r.x0 = std::min(u32(v[0] * w), w - 1);  r.x1 = std::max(u32(v[2] * w), r.x0 + 1);
    r.y0 = std::min(u32(v[1] * h), h - 1);  r.y1 = std::max(u32(v[3] * h), r.y0 + 1);
    r.x1 = std::min(r.x1, w); r.y1 = std::min(r.y1, h);
    return r;
}

// 列主序 m4x4 × (x,y,z,1)，返回 float[4]。
void GiLadderXform(const math::m4x4& m, float x, float y, float z, float out[4]) {
    const float xs[4] = {x, y, z, 1.0f};
    for (int rr = 0; rr < 4; ++rr) out[rr] = 0.0f;
    for (int cc = 0; cc < 4; ++cc)
        for (int rr = 0; rr < 4; ++rr)
            out[rr] += m.columns[cc][rr] * xs[cc];
}

void RunGiLadderDump(RHIDeviceBase* device, const FusionInputs& in,
                     ResourceHandle spgiTex, ResourceHandle indirectTex) {
    static bool dumped = false;
    if (dumped) return;
    static const u32 ladderFrame = [] {
        const char* e = std::getenv("GI_LADDER_FRAME");
        return e ? (u32)std::atoi(e) : 360u;
    }();
    static u32 tick = 0;
    ++tick;
    if (tick < ladderFrame) return;
    dumped = true;

    const u32 w = in.render_width, h = in.render_height;
    u32 stride = 8;
    if (const char* e = std::getenv("GI_LADDER_STRIDE")) stride = std::clamp((u32)std::atoi(e), 1u, 64u);
    const GiLadderRoi rois[2] = {
        GiLadderParseRoi("GI_LADDER_ROI", "0.4414,0.8583,0.5039,0.975", w, h),
        GiLadderParseRoi("GI_LADDER_ROI2", "0.766,0.25,0.996,0.667", w, h),
    };
    const char* roiNames[2] = {"target", "reference"};

    struct Source { const char* name; ResourceHandle tex; float scale; u32 bytesPx; };
    // ddgi/spgi（探针 gather 输出）与 vis（ShadowFilter）/ssgi 按设计为半分辨率
    // ——ROI 全分辨率坐标按 scale 缩放到各自纹素空间后拷贝/取样。
    // 2026-09-17 P0（probe-selection-density-audit §8.1）：新增 ssgi 与 albedo
    // 两个源——台账期望值改为逐像素镜像 FusionIndirect.frag 的真实融合式
    // （mix+SSGI 项+真实 albedo+gain），取代旧 (ddgi+spgi)×0.25/π 启发式。
    const Source sources[8] = {
        {"direct", in.primary_input_tex != handles::INVALID_RESOURCE ? in.primary_input_tex : in.black_texture, 1.0f, 8},
        {"ddgi", in.ddgi_tex != handles::INVALID_RESOURCE ? in.ddgi_tex : in.black_texture, 0.5f, 8},
        {"spgi", spgiTex != handles::INVALID_RESOURCE ? spgiTex : in.black_texture, 0.5f, 8},
        {"indirect", indirectTex != handles::INVALID_RESOURCE ? indirectTex : in.black_texture, 1.0f, 8},
        {"vis", in.shadow_vis_tex != handles::INVALID_RESOURCE ? in.shadow_vis_tex : in.black_texture, 0.5f, 1},
        {"ssgi", in.ssgi_tex != handles::INVALID_RESOURCE ? in.ssgi_tex : in.black_texture, 0.5f, 8},
        // GBuffer albedo 是 BGRA8_UNorm（4B/px）——按 8B 半浮点读会得到垃圾。
        {"albedo", in.gbuffer_albedo != handles::INVALID_RESOURCE ? in.gbuffer_albedo : in.black_texture, 1.0f, 4},
        // [R5 2026-09-20] spec 列（追加在 CSV 尾部，不挪既有列位）：屏幕外
        // specular 兜底输出（RGB=Fresnel 加权辐射 A=权重，全分辨率）。
        {"spec", in.refl_specular_tex != handles::INVALID_RESOURCE ? in.refl_specular_tex : in.black_texture, 1.0f, 8},
    };
    constexpr int kSpecSrc = 7;
    // 与 FusionIndirect pass 同源的增益（PRIMAL_GI_INDIRECT_GAIN，
    // Phase 1.1 起默认 1.0）。
    const float ladderGain = [] {
        float g = 1.0f;
        if (const char* e = std::getenv("PRIMAL_GI_INDIRECT_GAIN")) {
            const float v = static_cast<float>(std::atof(e));
            if (v >= 0.0f && v <= 8.0f) g = v;
        }
        return g;
    }();

    PRIMAL_LOG_STREAM(Error, "Render")
        << "[GILadderMeta] frame=" << tick << " render=" << w << "x" << h
        << " stride=" << stride
        << " vis_tex=" << (in.shadow_vis_tex != handles::INVALID_RESOURCE ? "bound" : "MISSING")
        << " ddgi_tex=" << (in.ddgi_tex != handles::INVALID_RESOURCE ? "bound" : "black")
        << " spgi_tex=" << (spgiTex != handles::INVALID_RESOURCE ? "bound" : "black")
        << " ssgi_tex=" << (in.ssgi_tex != handles::INVALID_RESOURCE ? "bound" : "black")
        << " albedo_tex=" << (in.gbuffer_albedo != handles::INVALID_RESOURCE ? "bound" : "black")
        << " gain=" << ladderGain
        << " (CSV: px,py,rayx,rayy,rayz,vis,direct_rgb,ddgi_rgb,spgi_rgb,indirect_rgb"
           ",ssgi_rgb,albedo_rgb,spgi_a,expected_lum,residual_lum,spec_rgb)"
        << std::endl;

    // ROI 级亮度收集（判定用）：[roi][stage]
    std::vector<float> lums[2][4] = {};
    // 目标带（roi 0）模型期望/残差聚合——判定块引用。
    float floorExpected = 0.0f, resMedianAll = 0.0f, resP95All = 0.0f, resMaxAll = 0.0f;

    for (int ri = 0; ri < 2; ++ri) {
        const GiLadderRoi r = rois[ri];
        // 每源一段（256 对齐）；段尺寸按源缩放后的纹素数。
        u64 secAlign[8];
        u32 secW[8], secH[8];
        u64 total = 0;
        for (int k = 0; k < 8; ++k) {
            secW[k] = std::max(1u, u32((r.x1 - r.x0) * sources[k].scale));
            secH[k] = std::max(1u, u32((r.y1 - r.y0) * sources[k].scale));
            const u64 bytes = (u64)secW[k] * secH[k] * sources[k].bytesPx;
            secAlign[k] = (total + 255u) & ~255u;
            total = secAlign[k] + bytes;
        }

        BufferDesc bd{}; bd.size = total;
        bd.bindFlags = (u32)BufferUsageFlags::TransferDst;
        bd.memoryUsage = GPUMemoryUsage::Readback;
        auto stage = device->CreateBuffer(bd);
        if (stage == handles::INVALID_RESOURCE) continue;

        bool copied = false;
        bool live[8] = {};
        {
            auto ch = device->CreateCommandBuffer(CommandQueueType::Graphics);
            auto* c2 = rhi::GetCommandBuffer(ch);
            if (c2) {
                c2->Begin();
                for (int k = 0; k < 8; ++k) {
                    if (sources[k].tex == in.black_texture) continue; // 1×1 占位不可拷 ROI
                    live[k] = true;
                    BufferTextureCopyRegion rr{};
                    rr.bufferOffset = secAlign[k];
                    rr.imageSubresource.layerCount = 1;
                    rr.imageOffset = {(int)(r.x0 * sources[k].scale),
                                      (int)(r.y0 * sources[k].scale), 0};
                    rr.imageExtent = {secW[k], secH[k], 1};
                    c2->CopyTextureToBuffer(sources[k].tex, stage, &rr, 1);
                }
                c2->End(); c2->Submit(); c2->WaitForCompletion();
                copied = true;
            }
            device->DestroyCommandBuffer(ch);
        }
        if (!copied) { device->DestroyBuffer(stage); continue; }

        const auto* base = static_cast<const u8*>(device->MapBuffer(stage, 0, total));
        if (!base) { device->DestroyBuffer(stage); continue; }

        std::vector<float> visL, dirL, ddgiL, spgiL, indL, expL, resL;
        for (u32 y = r.y0; y < r.y1; y += stride) {
            for (u32 x = r.x0; x < r.x1; x += stride) {
                // 世界射线：NDC 近/远两点反投影差向量（锁定机位 → 固定世界位置）
                const float u = (x + 0.5f) / w, v = (y + 0.5f) / h;
                float n0[4], n1[4];
                GiLadderXform(in.inv_view_projection, u * 2.0f - 1.0f, 1.0f - 2.0f * v, -1.0f, n0);
                GiLadderXform(in.inv_view_projection, u * 2.0f - 1.0f, 1.0f - 2.0f * v, 1.0f, n1);
                float dir[3];
                for (int k = 0; k < 3; ++k) {
                    const float fz = n1[k] / std::max(n1[3], 1e-6f);
                    const float nz = n0[k] / std::max(n0[3], 1e-6f);
                    dir[k] = fz - nz;
                }
                const float dl = std::sqrt(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
                float rgb[8][3] = {};   // 清零：未绑定源保持 0（模型 ssgi 项随之归零）
                float lum[8] = {0, 0, 0, 0, 0, 0, 0, 0};
                float spgiA = 0.0f, ssgiA = 0.0f;
                for (int k = 0; k < 8; ++k) {
                    if (!live[k]) continue; // 静音/缺资源段：保持 0
                    // 全分辨率采样点 → 该源缩放空间最近纹素
                    const u32 sx = std::min(secW[k] - 1u, u32((x - r.x0) * sources[k].scale));
                    const u32 sy = std::min(secH[k] - 1u, u32((y - r.y0) * sources[k].scale));
                    if (sources[k].bytesPx == 8) {
                        const u16* t = reinterpret_cast<const u16*>(base + secAlign[k]) +
                                       ((size_t)sy * secW[k] + sx) * 4;
                        for (int cch = 0; cch < 3; ++cch) rgb[k][cch] = GiLadderH16(t[cch]);
                        // alpha：spgi→融合权重 w；ssgi→命中距离（→置信度）。
                        if (k == 2) spgiA = GiLadderH16(t[3]);
                        if (k == 5) ssgiA = GiLadderH16(t[3]);
                    } else if (sources[k].bytesPx == 4) {
                        // BGRA8_UNorm：内存序 [B,G,R,A]。
                        const u8* t = base + secAlign[k] + ((size_t)sy * secW[k] + sx) * 4;
                        rgb[k][0] = t[2] / 255.0f;
                        rgb[k][1] = t[1] / 255.0f;
                        rgb[k][2] = t[0] / 255.0f;
                    } else {
                        const u8 g = *(base + secAlign[k] + (size_t)sy * secW[k] + sx);
                        rgb[k][0] = rgb[k][1] = rgb[k][2] = (float)g / 255.0f;
                    }
                    lum[k] = 0.2126f * rgb[k][0] + 0.7152f * rgb[k][1] + 0.0722f * rgb[k][2];
                }
                // 2026-09-17 P0：逐像素镜像 FusionIndirect.frag 的真实融合式
                //   w      = clamp(spgi.a, 0, 0.9)
                //   ssgi_c = clamp(1 - clamp(ssgi.a,0,30)/30*0.7, 0.3, 1)
                //   E      = mix(ddgi, spgi, w) + ssgi * ssgi_c * mix(.25,.15,w)
                //   Lo     = albedo * E * gain / π
                // 旧判定器 (ddgi+spgi)*0.25/π 与其差了加法/混合、实际 albedo、
                // alpha、SSGI 与 gain——C 档因此只证明"非零输出"。
                float wFusion = std::clamp(spgiA, 0.0f, 0.9f);
                float ssgiConf = std::clamp(1.0f - std::clamp(ssgiA, 0.0f, 30.0f) / 30.0f * 0.7f,
                                            0.3f, 1.0f);
                float expR = 0.0f, expG = 0.0f, expB = 0.0f;
                for (int cch = 0; cch < 3; ++cch) {
                    float mixTerm = rgb[1][cch] * (1.0f - wFusion) + rgb[2][cch] * wFusion;
                    float ssgiTerm = rgb[5][cch] * ssgiConf * (0.25f + (0.15f - 0.25f) * wFusion);
                    float e = (mixTerm + ssgiTerm) * rgb[6][cch] * ladderGain * 0.31830988f;
                    expR = cch == 0 ? e : expR;
                    expG = cch == 1 ? e : expG;
                    expB = cch == 2 ? e : expB;
                }
                float expectedLum = 0.2126f * expR + 0.7152f * expG + 0.0722f * expB;
                float residual = std::fabs(lum[3] - expectedLum);
                PRIMAL_LOG_STREAM(Error, "Render")
                    << "[GILadder] " << roiNames[ri]
                    << "," << x << "," << y
                    << "," << dir[0] / dl << "," << dir[1] / dl << "," << dir[2] / dl
                    << "," << rgb[4][0]
                    << "," << rgb[0][0] << "," << rgb[0][1] << "," << rgb[0][2]
                    << "," << rgb[1][0] << "," << rgb[1][1] << "," << rgb[1][2]
                    << "," << rgb[2][0] << "," << rgb[2][1] << "," << rgb[2][2]
                    << "," << rgb[3][0] << "," << rgb[3][1] << "," << rgb[3][2]
                    << "," << rgb[5][0] << "," << rgb[5][1] << "," << rgb[5][2]
                    << "," << rgb[6][0] << "," << rgb[6][1] << "," << rgb[6][2]
                    << "," << spgiA
                    << "," << expectedLum << "," << residual
                    << "," << rgb[kSpecSrc][0] << "," << rgb[kSpecSrc][1] << "," << rgb[kSpecSrc][2]
                    << std::endl;
                visL.push_back(rgb[4][0]);
                dirL.push_back(lum[0]); ddgiL.push_back(lum[1]);
                spgiL.push_back(lum[2]); indL.push_back(lum[3]);
                expL.push_back(expectedLum); resL.push_back(residual);
            }
        }
        device->UnmapBuffer(stage);
        device->DestroyBuffer(stage);

        auto med = [](std::vector<float> v) -> float {
            if (v.empty()) return 0.0f;
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            return v[v.size() / 2];
        };
        // P95/Max（残差分布用；med 会破坏排序，副本单独算）。
        auto p95 = [](std::vector<float> v) -> float {
            if (v.empty()) return 0.0f;
            std::sort(v.begin(), v.end());
            return v[std::min(v.size() - 1, (size_t)(v.size() * 95 / 100))];
        };
        auto vmax = [](std::vector<float> v) -> float {
            float m = 0.0f;
            for (float x : v) m = std::max(m, x);
            return m;
        };
        lums[ri][0] = {med(visL)}; lums[ri][1] = {med(dirL)};
        lums[ri][2] = {med(ddgiL), med(spgiL)}; lums[ri][3] = {med(indL)};
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[GILadderRoi] " << roiNames[ri]
            << " px=(" << r.x0 << "," << r.y0 << ")-(" << r.x1 << "," << r.y1 << ")"
            << " median vis=" << med(visL) << " direct=" << med(dirL)
            << " ddgi=" << med(ddgiL) << " spgi=" << med(spgiL)
            << " indirect=" << med(indL) << " expected(model)=" << med(expL)
            << std::endl;
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[GILadderResidual] " << roiNames[ri]
            << " median=" << med(resL) << " P95=" << p95(resL)
            << " max=" << vmax(resL)
            << " (逐像素 |indirect - model|；模型=融合式逐项镜像，"
               "输入为同一完成帧读回，锁定机位下混帧误差可忽略)"
            << std::endl;
        if (ri == 0) { // 目标带聚合供判定块使用
            floorExpected = med(expL);
            resMedianAll = med(resL);
            resP95All = p95(resL);
            resMaxAll = vmax(resL);
        }
    }

    // 审计分段判定（第一层分支；阈值是趋势判断的启发式，非验收线）。
    // 2026-09-17 P0（probe-selection-density-audit §8.1）：期望值改为逐像素
    // 镜像 FusionIndirect.frag 的真实融合式（mix + SSGI 项 + 实际 albedo +
    // spgi.a 权重 + PRIMAL_GI_INDIRECT_GAIN），不再用 (ddgi+spgi)×0.25/π。
    // 语义边界（必须随判定一起读）：
    //   A/B 档：供给为零 / 融合显著低于模型——有明确源可查。
    //   C 档：融合输出与模型一致——只证明"合成公式按预期工作"，
    //         不证明命中正确/能量守恒/只剩曝光问题（探针供给本身错误时
    //         模型会同样一致地复现错误）。
    const float supplyProbe = std::max(lums[1][2][0] + lums[1][2][1],
                                       lums[0][2][0] + lums[0][2][1]); // 对照带/目标带 ddgi+spgi 上界
    const float floorProbe = lums[0][2][0] + lums[0][2][1];
    const float floorFused = lums[0][3][0];
    const float floorModel = floorExpected; // 逐像素模型期望的中位数（目标带）
    const char* verdict = "C_fused_matches_model";
    if (floorProbe < std::max(0.02f * supplyProbe, 1e-3f))
        verdict = "A_trace_or_sc_source_dark";
    else if (floorFused < 0.5f * floorModel)
        verdict = "B_fusion_darkening";
    PRIMAL_LOG_STREAM(Error, "Render")
        << "[GILadderVerdict] " << verdict
        << " supply_probe=" << supplyProbe << " floor_probe=" << floorProbe
        << " floor_fused=" << floorFused
        << " floor_model(mix+ssgi+albedo+gain/pi)=" << floorModel
        << " residual(med/P95/max)=" << resMedianAll << "/" << resP95All << "/" << resMaxAll
        << " | A: 查SC texel/卡片深度/SDF/太阳方向(用SC_CARDVIEW/SC_DUMP_CARDS)"
        << " B: 查融合式输入(混帧/置信度/gain) C: 合成一致≠供给正确——"
           "仍需 GILadderResidual 与分支统计定位探针侧问题"
        << std::endl;
}

// ============================================================================
// GI 能量根治方案 Phase 2.4: 逐回路 DC 增益仪表（GI_DCGAIN=1 启用，默认零开销）。
// 每 GI_DCGAIN_STRIDE 帧读回上一帧 GI 供给（indirect）中心 ROI 的均值能量，
// 维护样本窗口；回路 DC 增益 ≈ 相邻帧能量增量之比的中位数（误差衰减率，
// 几何收敛回路应单调衰减 → <1，断言线 0.5）。张弛振荡表现为比值 >1 且
// 正负交替——一并打印符号序列供人工判读（TemporalAlpha/hysteresis 扫描）。
// ============================================================================
void RunGiLoopDcGainMeter(RHIDeviceBase* device, ResourceHandle indirectTex,
                          u32 renderW, u32 renderH) {
    static const bool enabled = [] {
        const char* e = std::getenv("GI_DCGAIN");
        return e && e[0] != '0';
    }();
    if (!enabled) return;
    static const u32 strideFrames = [] {
        const char* e = std::getenv("GI_DCGAIN_STRIDE");
        // 缺省 9 = 3 的倍数（indirect 三缓冲深度）——保证每次仪表采样落在
        // 同一物理槽位；否则槽间收敛残余差异会伪装成周期性"振荡"。
        return e ? std::clamp((u32)std::atoi(e), 1u, 128u) : 9u;
    }();
    static u32 tick = 0;
    ++tick;
    if (tick % strideFrames != 0) return;

    constexpr u32 kRoiW = 48, kRoiH = 48;
    const u32 x0 = renderW / 2 - kRoiW / 2, y0 = renderH / 2 - kRoiH / 2;
    const u64 bytes = (u64)kRoiW * kRoiH * 8;   // RGBA16F

    BufferDesc bd{}; bd.size = bytes;
    bd.bindFlags = (u32)BufferUsageFlags::TransferDst;
    bd.memoryUsage = GPUMemoryUsage::Readback;
    auto stage = device->CreateBuffer(bd);
    if (stage == handles::INVALID_RESOURCE) return;
    float energy = -1.0f;
    {
        auto ch = device->CreateCommandBuffer(CommandQueueType::Graphics);
        auto* cmd = rhi::GetCommandBuffer(ch);
        if (cmd) {
            cmd->Begin();
            BufferTextureCopyRegion rr{};
            rr.bufferOffset = 0;
            rr.imageSubresource.layerCount = 1;
            rr.imageOffset = {(int)x0, (int)y0, 0};
            rr.imageExtent = {kRoiW, kRoiH, 1};
            cmd->CopyTextureToBuffer(indirectTex, stage, &rr, 1);
            cmd->End(); cmd->Submit(); cmd->WaitForCompletion();
            if (const auto* base = static_cast<const u16*>(device->MapBuffer(stage, 0, bytes))) {
                double sum = 0.0;
                const size_t px = (size_t)kRoiW * kRoiH;
                for (size_t i = 0; i < px; ++i) {
                    const float r = GiLadderH16(base[i * 4 + 0]);
                    const float g = GiLadderH16(base[i * 4 + 1]);
                    const float b = GiLadderH16(base[i * 4 + 2]);
                    sum += 0.2126 * r + 0.7152 * g + 0.0722 * b;
                }
                energy = (float)(sum / (double)px);
            }
        }
        device->DestroyCommandBuffer(ch);
    }
    device->DestroyBuffer(stage);
    if (energy < 0.0f) return;

    constexpr u32 kWindow = 16;
    static float samples[kWindow] = {};
    static u32 count = 0;
    samples[count++ % kWindow] = energy;

    if (count < 4) return;
    // 相邻增量比（|Δ_n| / |Δ_{n-1}|，跳过近零增量），取中位数 = DC 增益估计。
    const u32 n = std::min(count, kWindow);
    std::vector<float> ratios;
    std::string seq;
    float prevDelta = 0.0f;
    bool havePrev = false;
    for (u32 i = 1; i < n; ++i) {
        const float ePrev = samples[(count - n + i - 1) % kWindow];
        const float eCurr = samples[(count - n + i) % kWindow];
        const float d = eCurr - ePrev;
        seq += d >= 0.0f ? "+" : "-";
        if (havePrev && std::abs(prevDelta) > 1e-4f && std::abs(d) > 1e-4f) {
            ratios.push_back(std::abs(d) / std::abs(prevDelta));
        }
        prevDelta = d;
        havePrev = true;
    }
    float dcGain = 0.0f;
    if (!ratios.empty()) {
        std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
        dcGain = ratios[ratios.size() / 2];
    }
    // 判定（Phase 2.4 / Phase 5 门②）：断言对象是"无张弛振荡/双稳态"。
    //   - 摆动幅度 amp = max|E - mean| / mean > 5% → FAIL（收敛后能量不应
    //     有大幅极限环；双稳态表现为幅度可观的分支跳变）。
    //   - dc_gain（相邻 |Δ| 比中位数）在稳态被读数噪声托底到 ~1，不能直接
    //     作断言线（计划稿的 "<0.5" 语义适用于收敛瞬态的衰减率）——保留为
    //     诊断输出，配合 seq 符号序列人工判读。
    double sum2 = 0.0;
    float emax = 0.0f, emin = 1e9f;
    for (u32 i = 0; i < n; ++i) {
        const float e = samples[i];
        sum2 += e; emax = std::max(emax, e); emin = std::min(emin, e);
    }
    const float emean = static_cast<float>(sum2 / n);
    const float amp = emean > 1e-6f ? (emax - emin) / emean : 0.0f;
    if (amp > 0.05f) {
        PRIMAL_LOG_STREAM(Error, "Render")
            << "[GIDCGain] E=" << energy << " mean=" << emean
            << " amp=" << amp << " (assert < 0.05) dc_gain=" << dcGain
            << " seq=" << seq
            << "  << 张弛振荡/双稳态警报：查注入端能量与 TemporalAlpha/hysteresis"
            << std::endl;
    } else {
        PRIMAL_LOG_STREAM(Info, "Render")
            << "[GIDCGain] E=" << energy << " mean=" << emean
            << " amp=" << amp << " (assert < 0.05) dc_gain=" << dcGain
            << " seq=" << seq << "  OK" << std::endl;
    }
}

} // namespace

FusionOutputs FusionCompositeModule::AddPasses(rendergraph::RenderGraph& graph, const FusionInputs& inputs) {
    FusionOutputs outputs{};
    // Phase 1d（时钟统一，审计 §2.4 新发现①）: 三缓冲槽位从 cbIdx（swapchain
    // 槽）改为绝对帧号——cbIdx 与 frameCount%3 之间无同步不变量（resize/多
    // 视图/未来异步会错位），错位时读侧会拿到本帧正在写的槽。现随 DDGI 体系
    // 统一走 DDGISlotMap.h；今天两者数值恒等（调用端 cbIdx=frameCount_%3），
    // 行为零变化，不变量显式化。
    u32 cbIdx = lumen::DDGISlotsForFrame(inputs.frame_index).writeIndex;
    // 同一帧号的上一完成槽（诊断读回统一取此槽，Phase 1d 起禁裸算）。
    const u32 prevIdx = lumen::DDGISlotsForFrame(inputs.frame_index).previousIndex;

    if (indirect_pipeline_ == handles::INVALID_PIPELINE || composite_pipeline_ == handles::INVALID_PIPELINE)
        return outputs;

    // Import textures into render graph
    auto indirectRG = graph.ImportResource("FusionIndirect", indirect_output_[cbIdx]);
    // FusionOutput 必须用 ImportTexture（携带 desc、RGResourceType::Texture）：
    // ImportResource 注册的是 Unknown 类型，下游（ToneMapping 的输出 desc
    // 拷贝、FinalBlit 等）按类型守卫解析会失败并回退空 desc——曾致
    // ToneMapping_Output 以 0 尺寸/pixelFormat 0 创建，Metal validation
    // 直接 assert（MTLTextureDescriptor validateWithDevice）。
    TextureDesc fusionOutDesc{};
    fusionOutDesc.size = {inputs.render_width, inputs.render_height, 1};
    fusionOutDesc.format = DataFormat::RGBA16_Float;
    fusionOutDesc.type = TextureType::Texture2D;
    fusionOutDesc.usage = TextureUsage::ShaderResource | TextureUsage::RenderTarget;
    auto fusionOutRG = graph.ImportTexture("FusionOutput", fusion_output_[cbIdx], fusionOutDesc);

    outputs.output_rg = fusionOutRG;
    outputs.output_tex = fusion_output_[cbIdx];

    // Resolve helper
    auto resolve = [&](rendergraph::RGResourceHandle h) -> ResourceHandle {
        if (!h.IsValid()) return inputs.black_texture;
        auto* res = graph.GetResource(h);
        return res ? res->GetPhysicalHandle() : inputs.black_texture;
    };

    // --- Pass 1: FusionIndirect (full-res) ---
    struct IndirectData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<IndirectData>("FusionIndirect",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [inputs, indirectRG](IndirectData& data, rendergraph::RenderGraphBuilder& builder) {
            builder.SideEffect(); // prevent RG culling
            if (inputs.ssgi_rg.IsValid()) builder.Read(inputs.ssgi_rg, ResourceState::ShaderResource);
            if (inputs.ddgi_rg.IsValid()) builder.Read(inputs.ddgi_rg, ResourceState::ShaderResource);
            if (inputs.spgi_rg.IsValid()) builder.Read(inputs.spgi_rg, ResourceState::ShaderResource);
            if (inputs.ssao_rg.IsValid()) builder.Read(inputs.ssao_rg, ResourceState::ShaderResource);
            data.output = builder.Write(indirectRG, ResourceState::RenderTarget);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({.texture = data.output, .loadOp = LoadAction::DontCare, .storeOp = StoreAction::Store, .clearColor = {math::v4{0, 0, 0, 0}}});
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs, cbIdx, prevIdx, resolve](const IndirectData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            static int s_fusion_diag = 0;
            if (s_fusion_diag < 3) { PRIMAL_LOG_STREAM(Error, "Render") << "[FUSION_INDIRECT] exec " << s_fusion_diag << std::endl; s_fusion_diag++; }
            cmd->SetViewport({{0, 0}, {static_cast<float>(render_width_), static_cast<float>(render_height_)}, 0, 1});
            cmd->SetScissor({{0, 0}, {render_width_, render_height_}});

            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
            u32 paramCount = isVk ? 6 : 5;

            // Resolve SSGI/DDGI/SPGI from RG handles when physical tex is INVALID.
            ResourceHandle ssgiTex = inputs.ssgi_tex;
            if (ssgiTex == handles::INVALID_RESOURCE && inputs.ssgi_rg.IsValid())
                ssgiTex = resolve(inputs.ssgi_rg);
            if (ssgiTex == handles::INVALID_RESOURCE) ssgiTex = inputs.black_texture;

            ResourceHandle spgiTex = inputs.spgi_tex;
            if (spgiTex == handles::INVALID_RESOURCE && inputs.spgi_rg.IsValid())
                spgiTex = resolve(inputs.spgi_rg);
            if (spgiTex == handles::INVALID_RESOURCE) spgiTex = inputs.black_texture;


            static int s_ssgi_resolve = 0;
            if (s_ssgi_resolve < 5) {
                PRIMAL_LOG_STREAM(Error, "Render") << "[FUSION_SSGI] ssgi_tex=" << inputs.ssgi_tex
                          << " ssgi_rg_valid=" << inputs.ssgi_rg.IsValid()
                          << " resolved=" << ssgiTex
                          << " black=" << inputs.black_texture << std::endl;
                s_ssgi_resolve++;
            }

            // Force SSGI texture to SRV layout — it may be left in TRANSFER_DST
            // by SSGI's internal blit or layout tracking issues.
            if (ssgiTex != inputs.black_texture) {
                ResourceBarrier ssgiBar{};
                ssgiBar.resource = ssgiTex;
                ssgiBar.beforeState = ResourceState::ShaderResource;
                ssgiBar.afterState = ResourceState::ShaderResource;
                ssgiBar.subresource = 0xFFFFFFFF;
                ssgiBar.queueFamily = 0xFFFFFFFF;
                cmd->InsertBarrier(&ssgiBar, 1);
            }

            // P0-B（queue-submit 09600 收尾）: 对其余全部 sampled 输入做同样
            // 的 ensure-SRO。SRO→SRO 声明是通用收敛原语：后端按 tracked 解析
            // ——tracked 停在 XFER_*/GENERAL（上游 blit/storage 写遗留）时发
            // 一次精确转换，已 SRO 时 no-op。ddgi/spgi 槽承载的纹理有 blit
            // 上游（probe radiance fused supply / SPGI blit），首帧绑定早于
            // 其首次转换时正是 "expects SRO, actual TRANSFER_*/UNDEFINED"
            // 预热错误的来源。black 占位纹理除外（常驻 SRO）。
            {
                ResourceHandle ensureList[4] = {
                    inputs.ddgi_tex != handles::INVALID_RESOURCE ? inputs.ddgi_tex : inputs.black_texture,
                    spgiTex != inputs.black_texture ? spgiTex : inputs.black_texture,
                    inputs.gbuffer_albedo != handles::INVALID_RESOURCE ? inputs.gbuffer_albedo : inputs.black_texture,
                    inputs.ssao_tex != handles::INVALID_RESOURCE ? inputs.ssao_tex : inputs.black_texture,
                };
                ResourceBarrier ensureBars[4]{};
                u32 ensureCount = 0;
                for (u32 i = 0; i < 4; ++i) {
                    if (ensureList[i] == inputs.black_texture ||
                        ensureList[i] == handles::INVALID_RESOURCE ||
                        ensureList[i] == ssgiTex) continue;  // ssgi 已单独处理
                    ensureBars[ensureCount].resource = ensureList[i];
                    ensureBars[ensureCount].beforeState = ResourceState::ShaderResource;
                    ensureBars[ensureCount].afterState  = ResourceState::ShaderResource;
                    ensureBars[ensureCount].subresource = 0xFFFFFFFF;
                    ensureBars[ensureCount].queueFamily = 0xFFFFFFFF;
                    ++ensureCount;
                }
                if (ensureCount > 0) cmd->InsertBarrier(ensureBars, ensureCount);
            }

            // GI 能量根治方案 Phase 1.1（Docs/2026-09-18-gi-energy-root-cure-plan.md）：
            // 融合端全局增益默认回归 **1.0**（物理恒等）。旧默认 3.0 是死探针
            // 网格时代扫频调出的均衡点（"gain3.0 全图统计落参考区间"），scene-fit
            // 后探针场变活，3.0 即整体过驱（mean 108.3 vs 参考 42.7，gain 只平移
            // 不恢复层次）。FusionIndirect.frag 的 gain 仅为 debug 旋钮，显示增益
            // 永不进入反馈回路。PRIMAL_GI_INDIRECT_GAIN 运行时可调（0..8；
            // 设 3.0 可临时回滚旧行为）。
            float giIndirectGain = 1.0f;
            if (const char* g = std::getenv("PRIMAL_GI_INDIRECT_GAIN")) {
                const float v = static_cast<float>(std::atof(g));
                if (v >= 0.0f && v <= 8.0f) giIndirectGain = v;
            }
            // WRC 计划 Stream C1（Fusion 档位化）：像素级 GI 来源 mode switch。
            // 档位结构单一事实源 = axes.use_screen_probes（决定 SPGI 是否
            // 运行）；此处只决定 fusion 像素读哪路：High/Ultra → SPGI-only，
            // Medium → DDGI-only。PRIMAL_FUSION_PERPIXEL=1 回滚旧逐像素
            // 置信度混合（A/B 证据保留）。
            float fusionMode = inputs.pixel_gi_spgi_only ? 1.0f : 2.0f;
            if (const char* p = std::getenv("PRIMAL_FUSION_PERPIXEL")) {
                if (p[0] == '1') fusionMode = 0.0f;
            }
            if (indirect_gain_cb_[cbIdx] != handles::INVALID_RESOURCE) {
                const float gainUbo[2] = { giIndirectGain, fusionMode };
                device_->UpdateBufferData(indirect_gain_cb_[cbIdx], gainUbo, sizeof(gainUbo), 0);
            }
            DescData params[7] = {
                {0, DescriptorType::SampledImage, ssgiTex},
                {1, DescriptorType::SampledImage, inputs.ddgi_tex != handles::INVALID_RESOURCE ? inputs.ddgi_tex : inputs.black_texture},
                {2, DescriptorType::SampledImage, spgiTex},
                {3, DescriptorType::SampledImage, inputs.gbuffer_albedo != handles::INVALID_RESOURCE ? inputs.gbuffer_albedo : inputs.black_texture},
                {4, DescriptorType::SampledImage, inputs.ssao_tex != handles::INVALID_RESOURCE ? inputs.ssao_tex : inputs.black_texture},
                {5, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
            };
            if (isVk && indirect_gain_cb_[cbIdx] != handles::INVALID_RESOURCE) {
                params[6] = {6, DescriptorType::UniformBuffer, indirect_gain_cb_[cbIdx]};
                paramCount = 7;
            }
            // Diagnostic source identity comes from the actual selected descriptor
            // array, including ScreenProbeGI's filtered-output fallback.
            if (isVk) {
                const char* auditPrefix=std::getenv("GI_ROI_AUDIT");
                if (auditPrefix && auditPrefix[0] && inputs.frame_index==RoiAuditRequestedFrame()) {
                    for (u32 i=0;i<5;++i) roi_audit_indirect_bound_[cbIdx][i]=params[i].resource;
                    roi_audit_indirect_bound_frame_[cbIdx]=inputs.frame_index;
                    roi_audit_indirect_bound_valid_[cbIdx]=true;
                }
            }
            // TEMP diagnosis: what really lands in each GI texture slot.
            static int s_fusion_slots = 0;
            if (s_fusion_slots < 3) {
                ++s_fusion_slots;
                PRIMAL_LOG_STREAM(Error, "Render") << "[FUSION_SLOTS] black=" << inputs.black_texture
                          << " ssgi=" << ssgiTex
                          << " ddgi=" << (inputs.ddgi_tex != handles::INVALID_RESOURCE ? inputs.ddgi_tex : inputs.black_texture)
                          << " spgi_in=" << inputs.spgi_tex
                          << " spgi_rg_valid=" << inputs.spgi_rg.IsValid()
                          << " spgi_bound=" << spgiTex
                          << " albedo=" << inputs.gbuffer_albedo
                          << " ssao=" << inputs.ssao_tex << std::endl;
            }
            UpdateDesc(device_, indirect_ds_[cbIdx], params, paramCount);
            cmd->BindGraphicsPipeline(indirect_pipeline_);
            const DescriptorSetHandle sets[] = { indirect_ds_[cbIdx] };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, indirect_layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);

            // sun-audit P1: GI_LADDER_DUMP 能量台账（一次性同步读回，见函数头注释）
            RunGiLadderDump(device_, inputs, spgiTex, indirect_output_[prevIdx]);
            // GI 能量根治方案 Phase 2.4: 逐回路 DC 增益仪表（GI_DCGAIN=1 启用）
            RunGiLoopDcGainMeter(device_, indirect_output_[prevIdx],
                                 inputs.render_width, inputs.render_height);

            // TEMP diagnosis: one-shot per-strip stats of the indirect output
            // (RGBA16F full-res). Strips when GI_DEBUG_STRIP=1: ddgi|ssgi|spgi|full.
            static const bool s_stripDump = std::getenv("SC_DUMP_CARDS") != nullptr;  // TEMP: decouple from strip env
            static u32 s_stripTick = 0;
            static bool s_stripDumped = false;
            const u32 stripTickNow = ++s_stripTick;
            if (s_stripDump && !s_stripDumped && stripTickNow > 120) {
                s_stripDumped = true;
                const u32 w = render_width_, h = render_height_;
                // Read LAST frame's output (triple-buffered, GPU-complete —
                // this frame's [cbIdx] hasn't been submitted yet).
                const u32 readIdx = prevIdx;
                BufferDesc bd{}; bd.size = (u64)w * h * 8;
                bd.bindFlags = (u32)BufferUsageFlags::TransferDst;
                bd.memoryUsage = GPUMemoryUsage::Readback;
                auto stage = device_->CreateBuffer(bd);
                if (stage != handles::INVALID_RESOURCE) {
                    auto ch = device_->CreateCommandBuffer(CommandQueueType::Graphics);
                    auto* c2 = rhi::GetCommandBuffer(ch);
                    if (c2) {
                        c2->Begin();
                        BufferTextureCopyRegion rr{};
                        rr.imageSubresource.mipLevel = 0;
                        rr.imageSubresource.layerCount = 1;
                        rr.imageExtent = {w, h, 1};
                        c2->CopyTextureToBuffer(indirect_output_[readIdx], stage, &rr, 1);
                        c2->End(); c2->Submit(); c2->WaitForCompletion();
                        device_->DestroyCommandBuffer(ch);
                        if (auto* p = static_cast<const u16*>(device_->MapBuffer(stage, 0, bd.size))) {
                            auto f16 = [](u16 h16) -> float {
                                u32 sign = (h16 >> 15) & 1u, exp = (h16 >> 10) & 0x1Fu, mant = h16 & 0x3FFu;
                                if (exp == 0u) return 0.0f;
                                u32 bits = (sign << 31) | ((exp + 112u) << 23) | (mant << 13);
                                float f; std::memcpy(&f, &bits, 4); return f;
                            };
                            const char* names[4] = { "ddgi-strip", "ssgi-strip", "spgi-strip", "full-strip" };
                            for (int q = 0; q < 4; ++q) {
                                u32 x0 = w * q / 4, x1 = w * (q + 1) / 4;
                                double sum = 0.0; float mx = 0.0f; u64 n = 0;
                                for (u32 y = 0; y < h; y += 4)
                                    for (u32 x = x0; x < x1; x += 4) {
                                        const u16* t = p + ((size_t)y * w + x) * 4;
                                        float r = f16(t[0]), g = f16(t[1]), b = f16(t[2]);
                                        if (std::isnan(r)) continue;
                                        float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
                                        sum += lum; mx = std::max(mx, lum); ++n;
                                    }
                                if (n) PRIMAL_LOG_STREAM(Error, "Render") << "[IND_STATS] " << names[q]
                                                 << " mean=" << (sum / n) << " max=" << mx << std::endl;
                            }
                            device_->UnmapBuffer(stage);
                        }
                    }
                    device_->DestroyBuffer(stage);
                }
            }

            // TEMP diagnosis: readback the "black" texture — if the heap-
            // recycled contents are non-zero, the SPGI/AO fallback slots
            // sample recycled scene data instead of black.
            static bool s_blackDumped = false;
            if (!s_blackDumped && inputs.black_texture != handles::INVALID_RESOURCE) {
                s_blackDumped = true;
                auto ch2 = device_->CreateCommandBuffer(CommandQueueType::Graphics);
                auto* c3 = rhi::GetCommandBuffer(ch2);
                if (c3) {
                    c3->Begin();
                    BufferTextureCopyRegion br{};
                    br.imageSubresource.mipLevel = 0;
                    br.imageSubresource.layerCount = 1;
                    br.imageExtent = {1, 1, 1};
                    auto bsDesc = BufferDesc{};
                    bsDesc.size = 512;
                    bsDesc.bindFlags = (u32)BufferUsageFlags::TransferDst;
                    bsDesc.memoryUsage = GPUMemoryUsage::Readback;
                    auto bs = device_->CreateBuffer(bsDesc);
                    if (bs != handles::INVALID_RESOURCE) {
                        c3->CopyTextureToBuffer(inputs.black_texture, bs, &br, 1);
                        c3->End(); c3->Submit(); c3->WaitForCompletion();
                        device_->DestroyCommandBuffer(ch2);
                        if (auto* q = static_cast<const u16*>(device_->MapBuffer(bs, 0, 512))) {
                            PRIMAL_LOG_STREAM(Error, "Render") << "[BLACK_TEX] first 8 texels rgba16:";
                            for (int k = 0; k < 32; ++k) PRIMAL_LOG_STREAM(Error, "Render") << " " << q[k];
                            PRIMAL_LOG_STREAM(Error, "Render") << std::endl;
                            device_->UnmapBuffer(bs);
                        }
                    } else {
                        device_->DestroyCommandBuffer(ch2);
                    }
                    device_->DestroyBuffer(bs);
                }
            }
        }
    );

    // --- Lightmap 旁路（2026-10-19 计划）---
    // 场景注册了 lightmap 页时，独立模块在两个 fusion pass 之间追加改写
    // pass（读 FusionIndirect 输出、写自己的替换纹理），Pass 2 的 indirect
    // 绑定随之切换。未注册时零额外 pass、绑定原样——既有路径零差异。
    rendergraph::RGResourceHandle effectiveIndirectRG = indirectRG;
    ResourceHandle effectiveIndirectTex = indirect_output_[cbIdx];
    if (lightmap_hook_ && lightmap_hook_->HasActiveLightmap()) {
        LightmapModule::FrameInputs lmIn;
        lmIn.frame_index = inputs.frame_index;
        lmIn.render_width = inputs.render_width;
        lmIn.render_height = inputs.render_height;
        lmIn.indirect_tex = indirect_output_[cbIdx];
        const rendergraph::RGResourceHandle lmOut =
            lightmap_hook_->AddCompositePass(graph, lmIn, indirectRG);
        if (lmOut.IsValid()) {
            effectiveIndirectRG = lmOut;
            effectiveIndirectTex = lightmap_hook_->GetOutputTexture();
        }
    }

    // --- Pass 2: FusionComposite (full-res) ---
    struct ComposeData {
        rendergraph::RGResourceHandle output;
    };

    graph.AddPass<ComposeData>("FusionComposite",
        rendergraph::RGPassType::Graphics,
        rendergraph::RGPassCategory::PostProcess,
        [primaryRG = inputs.primary_input_rg, volumeRG = inputs.volume_scatter_rg, ssrRG = inputs.ssr_rg,
         specRG = inputs.ddgi_specular_rg, reflRG = inputs.refl_specular_rg,
         indirectRG = effectiveIndirectRG, fusionOutRG](ComposeData& data, rendergraph::RenderGraphBuilder& builder) {
            builder.SideEffect(); // prevent RG culling
            if (primaryRG.IsValid()) builder.Read(primaryRG, ResourceState::ShaderResource);
            builder.Read(indirectRG, ResourceState::ShaderResource);
            if (volumeRG.IsValid()) builder.Read(volumeRG, ResourceState::ShaderResource);
            if (ssrRG.IsValid()) builder.Read(ssrRG, ResourceState::ShaderResource);
            if (specRG.IsValid()) builder.Read(specRG, ResourceState::ShaderResource);
            if (reflRG.IsValid()) builder.Read(reflRG, ResourceState::ShaderResource);
            data.output = builder.Write(fusionOutRG, ResourceState::RenderTarget);

            rendergraph::RGRenderPassDesc rpDesc;
            rpDesc.colors.push_back({.texture = data.output, .loadOp = LoadAction::DontCare, .storeOp = StoreAction::Store, .clearColor = {math::v4{0, 0, 0, 1}}});
            builder.DeclareRenderPass(rpDesc);
        },
        [this, inputs, cbIdx, indirectTex = effectiveIndirectTex, resolve](const ComposeData&, rendergraph::RenderGraphContext& context) {
            auto cmd = context.cmdBuffer;
            static int s_composite_diag = 0;
            if (s_composite_diag < 3) { PRIMAL_LOG_STREAM(Error, "Render") << "[FUSION_COMPOSITE] exec " << s_composite_diag << std::endl; s_composite_diag++; }
            cmd->SetViewport({{0, 0}, {static_cast<float>(render_width_), static_cast<float>(render_height_)}, 0, 1});
            cmd->SetScissor({{0, 0}, {render_width_, render_height_}});

            // Resolve volume scatter from RG, or fall back to identity texture
            // (transmittance=1). P1（sponza-gap-reaudit §4.4）: 旧 workaround 在
            // Vulkan 无条件绑 identity（当年 RG 解析出错误格式的 VolumePass 纹理
            // 触发 R32_UINT validation）——这使 FROXEL 开启后合成始终不消费雾，
            // 全图 RGB MAE 仅 0.118/255。Froxel 输出现在是合法 RGBA16F 半分辨率
            // 纹理，两平台统一恢复 RG 消费；无有效输入时仍回 identity。
            ResourceHandle volTex = volume_identity_tex_;
            if (inputs.volume_scatter_rg.IsValid()) {
                ResourceHandle rgVol = resolve(inputs.volume_scatter_rg);
                if (rgVol != inputs.black_texture && rgVol != handles::INVALID_RESOURCE)
                    volTex = rgVol;
            }

            // Resolve SSR from RG when physical tex is INVALID; fall back to black.
            ResourceHandle ssrTex = inputs.ssr_tex;
            if (ssrTex == handles::INVALID_RESOURCE && inputs.ssr_rg.IsValid())
                ssrTex = resolve(inputs.ssr_rg);
            if (ssrTex == handles::INVALID_RESOURCE) ssrTex = inputs.black_texture;

            // ORM texture (roughness/metallic) — fall back to white (roughness=1) when absent,
            // so SSR contribution is fully attenuated (smoothstep at roughness=1 → weight 0).
            ResourceHandle ormTex = inputs.gbuffer_orm;
            if (ormTex == handles::INVALID_RESOURCE) ormTex = inputs.black_texture;

            // T4.3: DDGI glossy specular + albedo (metallic F0). Black fallbacks
            // degrade the glossy relay to SSR-only.
            ResourceHandle specTex = inputs.ddgi_specular_tex;
            if (specTex == handles::INVALID_RESOURCE && inputs.ddgi_specular_rg.IsValid())
                specTex = resolve(inputs.ddgi_specular_rg);
            if (specTex == handles::INVALID_RESOURCE || specTex == inputs.black_texture)
                specTex = inputs.black_texture;
            ResourceHandle albedoTex = inputs.gbuffer_albedo;
            if (albedoTex == handles::INVALID_RESOURCE) albedoTex = inputs.black_texture;

            const bool isVk = device_->GetPlatform() == RHIPlatform::Vulkan;
            const u32 paramCount = isVk ? 9 : 7;
            // [R4/R5] 屏幕外 specular 输入（ReflSpecularTrace）；缺席 → black
            //（合成式退化为 v1：SSR + glossy relay）。
            ResourceHandle reflTex = inputs.refl_specular_tex;
            if (reflTex == handles::INVALID_RESOURCE && inputs.refl_specular_rg.IsValid())
                reflTex = resolve(inputs.refl_specular_rg);
            if (reflTex == handles::INVALID_RESOURCE) reflTex = inputs.black_texture;
            DescData params[9] = {
                {0, DescriptorType::SampledImage, inputs.primary_input_tex},
                {1, DescriptorType::SampledImage, indirectTex},
                {2, DescriptorType::SampledImage, volTex},
                {3, DescriptorType::SampledImage, ssrTex},
                {4, DescriptorType::SampledImage, ormTex},
                {5, DescriptorType::SampledImage, albedoTex},
                {6, DescriptorType::SampledImage, specTex},
                {7, DescriptorType::Sampler, static_cast<ResourceHandle>(default_sampler_)},
                {8, DescriptorType::SampledImage, reflTex},
            };
            // Capture the exact handles that reach the live FusionComposite
            // descriptor set.  The later GI_ROI_AUDIT pass consumes this
            // snapshot; it must not reconstruct a fallback from FusionInputs
            // after the render-graph callback has run.  This is diagnostics
            // only and is gated by the existing Vulkan audit environment.
            if (isVk) {
                const char* auditPrefix = std::getenv("GI_ROI_AUDIT");
                if (auditPrefix && auditPrefix[0] &&
                    inputs.frame_index == RoiAuditRequestedFrame()) {
                    for (u32 binding = 0; binding < kRoiAuditCompositeBindingCount; ++binding)
                        roi_audit_composite_bound_[cbIdx][binding] = params[binding].resource;
                    roi_audit_composite_bound_frame_[cbIdx] = inputs.frame_index;
                    roi_audit_composite_bound_valid_[cbIdx] = true;
                }
            }
            UpdateDesc(device_, composite_ds_[cbIdx], params, paramCount);
            cmd->BindGraphicsPipeline(composite_pipeline_);
            const DescriptorSetHandle sets[] = { composite_ds_[cbIdx] };
            cmd->BindDescriptorSets(PipelineBindPoint::Graphics, composite_layout_, 0, 1, sets, 0, nullptr);
            cmd->Draw(3, 0, 1, 0);
        }
    );

    // --- Pass 3: GI 可见性 flush（生产修复）+ PRIMAL_IND_ASYNC 多路条带统计（诊断）---
    //
    // 生产修复（Metal 白爆，2026-09-02）：DDGI gather 的 compute 写纹理对后续
    // render encoder 的采样不可见（fragmentFusion 读到堆回收垃圾，条带 mean 高达
    // 199/1724 → 直射区+GI 区全屏爆白+噪点）。UAV 纹理已是 HazardTrackingModeTracked、
    // RG 也已声明 ShaderResource 读边，但只有对本纹理的一次全幅 blit 读能强制
    // 可见化（部分区域拷贝无效、仅声明 RG 边无效——已按位掩码逐项实测）。
    // 故 Metal 平台默认执行 ddgi 全幅 CopyTextureToBuffer（半res RGBA16F ≈1.4MB/帧，
    // Apple Silicon 带宽下可忽略）。Vulkan 无此问题，保持零改动。
    //
// 诊断门 PRIMAL_IND_ASYNC（位掩码，覆盖默认行为，用于回归定位）：
//   1 = 全部边（ddgi|primary|indirect|fusion）  2 = 仅 ddgi（=生产默认）
//   4 = 仅 primary   8 = 仅 indirect           16 = 仅 fusion
//  32 = 仅 ddgi 边声明、不发拷贝命令           64 = 仅 ddgi 拷 256×256 角落
// 128 = 仅 spgi（ScreenProbeGI 输出，能量标定与 DDGI 槽位对比）
    // 设了该环境变量时同时打印 [*_ASYNC] 条带统计（读回环：拷贝挂本帧 CB，
    // 3 帧后同槽映射，零额外同步——SC_DUMP_CARDS 的帧中 WaitForCompletion 会
    // 掩盖时序问题，不可用于此类定位）。
    {
        static const u32 s_envMask = [] {
            const char* e = std::getenv("PRIMAL_IND_ASYNC");
            if (!e) return 0u;
            u32 m = (u32)std::atoi(e);
            return m == 1u ? 15u : m;
        }();
        static const bool s_printStats = [] {
            return std::getenv("PRIMAL_IND_ASYNC") != nullptr;
        }();
        const bool isMetal = device_->GetPlatform() == RHIPlatform::Metal;
        // Vulkan 的 DDGI gather 是半分辨率纹理，全幅 extent 拷贝会越界——
        // 非 Metal 平台强制清掉 ddgi 位（间接/合成输出都是全分辨率，安全）。
        u32 maskEff = s_envMask ? s_envMask : (isMetal ? 2u : 0u);
        if (!isMetal) maskEff &= ~(2u | 64u);
        const u32 s_asyncMask = maskEff;
        static u32 s_asyncTick = 0;
        if (s_asyncMask) {
            struct AsyncStatsData {
                rendergraph::RGResourceHandle source;
                rendergraph::RGResourceHandle primary;
            };
            graph.AddPass<AsyncStatsData>("FusionIndirectAsyncStats",
                rendergraph::RGPassType::Compute,
                rendergraph::RGPassCategory::PostProcess,
                [inputs, indirectRG, fusionOutRG, s_asyncMask](AsyncStatsData& data, rendergraph::RenderGraphBuilder& builder) {
                    builder.SideEffect(); // 诊断 pass，禁止被 RG 裁剪
                    if (s_asyncMask & 8u) data.source = builder.Read(indirectRG, ResourceState::CopySource);
                    if ((s_asyncMask & 4u) && inputs.primary_input_rg.IsValid())
                        data.primary = builder.Read(inputs.primary_input_rg, ResourceState::CopySource);
                    // fusion 输出在 composite pass 里写入；ddgi 来自 GIGather。
                    // 声明 CopySource 读依赖，保证拷贝排在写入之后。
                    if ((s_asyncMask & 16u) && fusionOutRG.IsValid())
                        builder.Read(fusionOutRG, ResourceState::CopySource);
                    if ((s_asyncMask & 2u) && inputs.ddgi_rg.IsValid())
                        builder.Read(inputs.ddgi_rg, ResourceState::CopySource);
                    if ((s_asyncMask & 128u) && inputs.spgi_rg.IsValid())
                        builder.Read(inputs.spgi_rg, ResourceState::CopySource);
                },
                [this, inputs, cbIdx, resolve,
                 indirectTex = indirect_output_[cbIdx],
                 fusionTex = fusion_output_[cbIdx], s_asyncMask,
                 printStats = s_printStats](const AsyncStatsData&, rendergraph::RenderGraphContext& context) {
                    auto cmd = context.cmdBuffer;
                    const u32 tick = ++s_asyncTick;
                    const u32 w = render_width_, h = render_height_;
                    if (w == 0 || h == 0) return;

                    struct StatTarget {
                        const char* tag;
                        rhi::ResourceHandle tex;
                        rhi::ResourceHandle* ring;
                    };
                    // ddgi = GIGather 原始输出；primary = deferred 直射；
                    // indirect = FusionIndirect 合并结果；fusion = 合成帧（进 TAA 前）。
                    rhi::ResourceHandle ddgiTex = inputs.ddgi_tex != handles::INVALID_RESOURCE
                        ? inputs.ddgi_tex : inputs.black_texture;
                    // SPGI 槽位：物理纹理在模块内从 RG 解析（fusion 同路径）；
                    // 解析不出（SPGI 未启用）→ INVALID，跳过该目标的拷/印。
                    rhi::ResourceHandle spgiTex = inputs.spgi_tex;
                    if (spgiTex == handles::INVALID_RESOURCE && inputs.spgi_pass)
                        spgiTex = inputs.spgi_pass->GetFilteredOutputTexture();
                    if (spgiTex == handles::INVALID_RESOURCE && inputs.spgi_rg.IsValid())
                        spgiTex = resolve(inputs.spgi_rg);
                    StatTarget targets[5] = {
                        {"DDGI_ASYNC",   ddgiTex,     ddgi_stats_staging_},
                        {"PRIMARY_ASYNC", inputs.primary_input_tex, primary_stats_staging_},
                        {"IND_STATS_ASYNC", indirectTex, async_stats_staging_},
                        {"FUSION_ASYNC",  fusionTex,   fusion_stats_staging_},
                        {"SPGI_ASYNC",    spgiTex,     spgi_stats_staging_},
                    };

                    // 1) 映射 3 帧前同槽拷贝的成果并出数（该帧 fence 已等过）。
                    //    统计打印只随显式诊断门开启，生产 flush 静默。
                    if (printStats) {
                        for (auto& t : targets) {
                            if (t.tex == handles::INVALID_RESOURCE) continue;
                            if (t.ring[cbIdx] != handles::INVALID_RESOURCE && tick > 8) {
                                if (auto* p = static_cast<const u16*>(
                                        device_->MapBuffer(t.ring[cbIdx], 0, (u64)w * h * 8))) {
                                    PrintIndirectStripStats(p, w, h, t.tag, tick);
                                    device_->UnmapBuffer(t.ring[cbIdx]);
                                }
                            }
                        }
                    }

                    // 2) 惰性建本槽 readback 缓冲（尺寸固定——诊断门不做 resize 跟踪）。
                    auto ensureStaging = [&](rhi::ResourceHandle& slot) {
                        if (slot == handles::INVALID_RESOURCE) {
                            BufferDesc bd{};
                            bd.size = (u64)w * h * 8;
                            bd.bindFlags = (u32)BufferUsageFlags::TransferDst;
                            bd.memoryUsage = GPUMemoryUsage::Readback;
                            slot = device_->CreateBuffer(bd);
                        }
                        return slot != handles::INVALID_RESOURCE;
                    };

                    // 3) 把本帧输出拷进本槽，3 帧后由本 pass 读出（按掩码选边）。
                    // mask 64：ddgi 只拷 256×256 角落——验证可见性 flush 不需要
                    // 全幅拷贝（同步点是访问本身而非数据量），以便低代价上生产。
                    BufferTextureCopyRegion rr{};
                    rr.imageSubresource.mipLevel = 0;
                    rr.imageSubresource.layerCount = 1;
                    rr.imageExtent = {w, h, 1};
                    if (s_asyncMask & 64u) rr.imageExtent = {std::min(256u, w), std::min(256u, h), 1};
                    const u32 targetBits[5] = {2u, 4u, 8u, 16u, 128u};
                    for (u32 i = 0; i < 5; ++i) {
                        auto& t = targets[i];
                        const bool copyThis = (s_asyncMask & targetBits[i]) ||
                                              ((s_asyncMask & 64u) && targetBits[i] == 2u);
                        if (!copyThis) continue;
                        if (t.tex == handles::INVALID_RESOURCE) continue;
                        if (!ensureStaging(t.ring[cbIdx])) continue;
                        cmd->CopyTextureToBuffer(t.tex, t.ring[cbIdx], &rr, 1);
                    }
                    // mask 32：仅声明 RG 读边（setup），不执行任何拷贝命令。
                });
        }
    }

    // Opt-in same-frame consumer snapshot. Copies are encoded on the actual
    // RG command buffer AFTER the producers; no mid-frame submit/wait. Read
    // staging only three frames later on the same completed frame slot, like
    // the existing async stats ring. Texture dimensions come from the RHI.
    const char* roiPrefix=std::getenv("GI_ROI_AUDIT");
    if (roiPrefix && roiPrefix[0] && device_->GetPlatform()==RHIPlatform::Vulkan) {
        if (roi_audit_queued_ && !roi_audit_dumped_ &&
            inputs.frame_index>=roi_audit_frame_+3u && cbIdx==roi_audit_slot_) {
            bool complete=true;
            bool beginEndSubmitChecked=false;
            bool completionChecked=false;
            bool explicitHostDependency=false;

            // A Vulkan InsertBarrier on a buffer is currently a no-op in this
            // RHI.  Before mapping any staging buffer, submit a standalone
            // graphics command containing a real Transfer→Host dependency and
            // wait for that command's completion.  If any part of this check
            // fails, do not map or report a complete snapshot: a stale map is
            // evidence of nothing.
            const auto syncHandle=device_->CreateCommandBuffer(CommandQueueType::Graphics);
            auto* syncCmd=rhi::GetCommandBuffer(syncHandle);
            if (syncCmd && syncCmd->Begin()) {
                explicitHostDependency=true;
                syncCmd->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                    AccessFlag::TransferWrite, AccessFlag::HostRead);
                const bool ended=syncCmd->End();
                const bool submitted=ended && syncCmd->Submit();
                const bool waited=submitted && syncCmd->WaitForCompletion();
                beginEndSubmitChecked=ended && submitted;
                completionChecked=beginEndSubmitChecked && waited;
            }
            if (syncHandle!=handles::INVALID_COMMAND_BUFFER)
                device_->DestroyCommandBuffer(syncHandle);
            if (!completionChecked) complete=false;

            if (completionChecked) {
                for (auto& source : roi_audit_sources_) {
                    if (source.staging==handles::INVALID_RESOURCE) {
                        complete=false;
                        continue;
                    }
                    const u64 bytes=u64(source.width)*source.height*source.bytes_per_pixel;
                    auto* mapped=device_->MapBuffer(source.staging,0,bytes);
                    if (!mapped) { complete=false; continue; }
                    std::string path=std::string(roiPrefix)+"-"+source.name+".raw";
                    if (FILE* out=std::fopen(path.c_str(),"wb")) {
                        complete &= std::fwrite(mapped,1,bytes,out)==bytes;
                        std::fclose(out);
                    } else complete=false;
                    device_->UnmapBuffer(source.staging);
                }
            }
            std::string metaPath=std::string(roiPrefix)+"-meta.json";
            if (FILE* meta=std::fopen(metaPath.c_str(),"w")) {
                std::fprintf(meta,"{\"frame\":%u,\"read_frame\":%u,\"width\":%u,\"height\":%u,\"gain\":%.9g,\"fusion_mode\":%.9g,\"complete\":%s,\"completion_checked\":%s,\"begin_end_submit_checked\":%s,\"explicit_host_dependency\":%s,\"completion_scope\":\"checked_graphics_queue_after_snapshot_copies\",\"sources\":[",
                    roi_audit_frame_,inputs.frame_index,render_width_,render_height_,roi_audit_gain_,roi_audit_mode_,
                    complete?"true":"false",completionChecked?"true":"false",
                    beginEndSubmitChecked?"true":"false",explicitHostDependency?"true":"false");
                for (u32 i=0;i<kRoiAuditSourceCount;++i) {
                    const auto& source=roi_audit_sources_[i];
                    std::fprintf(meta,"%s{\"name\":\"%s\",\"format\":\"%s\",\"runtime_format\":%u,\"width\":%u,\"height\":%u,\"live\":%s,\"constant_black\":%s,\"resource_handle\":%llu,\"binding\":%u,\"layout\":\"%s\",\"source_identity\":\"%s\",\"readback_layout\":\"copy_source_then_shader_resource\"}",
                        i?",":"",source.name?source.name:"unknown",source.format?source.format:"unknown",
                        static_cast<unsigned>(source.runtime_format),source.width,source.height,
                        source.staging!=handles::INVALID_RESOURCE?"true":"false",source.constant_black?"true":"false",
                        static_cast<unsigned long long>(source.bound_resource),source.binding,
                        source.layout?source.layout:"unknown",source.source_identity?source.source_identity:"unknown");
                }
                std::fprintf(meta,"]}\n");std::fclose(meta);
            } else complete=false;
            roi_audit_dumped_=true;
            PRIMAL_LOG_STREAM(Error,"Render") << "[GI_ROI_AUDIT] frame=" << roi_audit_frame_
                << " read_frame=" << inputs.frame_index << " complete=" << complete
                << " completion_checked=" << completionChecked
                << " begin_end_submit_checked=" << beginEndSubmitChecked
                << " explicit_host_dependency=" << explicitHostDependency << std::endl;
        }
        const u32 requestedFrame=RoiAuditRequestedFrame();
        if (!roi_audit_queued_ && inputs.frame_index==requestedFrame) {
            struct RoiAuditData {};
            graph.AddPass<RoiAuditData>("FusionConsumerRoiAudit",rendergraph::RGPassType::Compute,
                rendergraph::RGPassCategory::PostProcess,
                [inputs,indirectRG,fusionOutRG](RoiAuditData&,rendergraph::RenderGraphBuilder& builder) {
                    builder.SideEffect();
                    builder.Read(indirectRG,ResourceState::CopySource);
                    builder.Read(fusionOutRG,ResourceState::CopySource);
                    for (auto h : {inputs.ddgi_rg,inputs.spgi_rg,inputs.ssgi_rg,inputs.ssao_rg,
                                   inputs.primary_input_rg,inputs.volume_scatter_rg,inputs.ssr_rg,
                                   inputs.ddgi_specular_rg,inputs.refl_specular_rg})
                        if (h.IsValid()) builder.Read(h,ResourceState::CopySource);
                },
                [this,inputs,cbIdx,resolve](const RoiAuditData&,rendergraph::RenderGraphContext& context) {
                    struct Source {
                        const char* name;
                        ResourceHandle tex;
                        u32 binding;
                        const char* identity;
                    };

                    const bool boundCaptured =
                        roi_audit_composite_bound_valid_[cbIdx] &&
                        roi_audit_composite_bound_frame_[cbIdx] == inputs.frame_index;
                    const auto& compositeBound = roi_audit_composite_bound_[cbIdx];
                    const auto compositeSource = [&](u32 binding) -> ResourceHandle {
                        return boundCaptured ? compositeBound[binding] : handles::INVALID_RESOURCE;
                    };

                    const bool indirectCaptured=roi_audit_indirect_bound_valid_[cbIdx] &&
                        roi_audit_indirect_bound_frame_[cbIdx]==inputs.frame_index;
                    const auto indirectSource=[&](u32 binding) -> ResourceHandle {
                        return indirectCaptured ? roi_audit_indirect_bound_[cbIdx][binding] : handles::INVALID_RESOURCE;
                    };
                    // Preserve the established eight names, then append the
                    // five actual FusionComposite sampled inputs.  Volume is
                    // the live identity handle when the production callback
                    // selected that fallback; it is not replaced by black.
                    const Source sources[kRoiAuditSourceCount]={
                        {"ddgi",          indirectSource(1),
                                         1, "FusionIndirect binding 1"},
                        {"spgi",          indirectSource(2),
                                         2, "FusionIndirect binding 2"},
                        {"ssgi",          indirectSource(0),
                                         0, "FusionIndirect binding 0"},
                        {"albedo",        compositeSource(5),
                                         5, "FusionComposite binding 5"},
                        {"ao",            indirectSource(4),
                                         4, "FusionIndirect binding 4"},
                        {"indirect",      compositeSource(1),
                                         1, "FusionComposite binding 1"},
                        {"direct",         compositeSource(0),
                                         0, "FusionComposite binding 0"},
                        {"composite",     fusion_output_[cbIdx],
                                         0xFFFFFFFFu, "FusionComposite current render target"},
                        {"orm",            compositeSource(4),
                                         4, "FusionComposite binding 4"},
                        {"ddgi_specular", compositeSource(6),
                                         6, "FusionComposite binding 6"},
                        {"ssr",            compositeSource(3),
                                         3, "FusionComposite binding 3"},
                        {"refl_off",      compositeSource(8),
                                         8, "FusionComposite binding 8"},
                        {"volume",         compositeSource(2),
                                         2, "FusionComposite binding 2 (identity fallback is live)"},
                    };

                    // A rerun must not accidentally map a previous source
                    // array.  This pass is opt-in and one-shot; clear any
                    // leftover staging before tying metadata to this frame.
                    for (auto& dst : roi_audit_sources_) {
                        if (dst.staging!=handles::INVALID_RESOURCE)
                            device_->DestroyBuffer(dst.staging);
                        dst={};
                    }

                    bool copiedAny=false;
                    for (u32 i=0;i<kRoiAuditSourceCount;++i) {
                        const auto& src=sources[i];
                        auto& dst=roi_audit_sources_[i];
                        dst.name=src.name;
                        dst.binding=src.binding;
                        dst.source_identity=src.identity;
                        dst.bound_resource=src.tex;
                        dst.constant_black=(src.tex==inputs.black_texture && src.tex!=handles::INVALID_RESOURCE);
                        dst.layout=(src.tex!=handles::INVALID_RESOURCE) ? "shader_resource" : "unknown";
                        if (src.tex==handles::INVALID_RESOURCE) continue;

                        dst.runtime_format=device_->GetTextureFormat(src.tex);
                        dst.format=RoiAuditFormatName(dst.runtime_format);
                        dst.bytes_per_pixel=RoiAuditFormatBytes(dst.runtime_format);
                        // Unknown formats are intentionally not copied: a
                        // guessed byte width would make invalid replay look
                        // like valid GPU data.
                        if (!dst.bytes_per_pixel) continue;
                        if (!device_->GetTextureSize(src.tex,dst.width,dst.height)) continue;

                        BufferDesc bd{};
                        bd.size=u64(dst.width)*dst.height*dst.bytes_per_pixel;
                        bd.bindFlags=u32(BufferUsageFlags::TransferDst);
                        bd.memoryUsage=GPUMemoryUsage::Readback;
                        dst.staging=device_->CreateBuffer(bd);
                        if (dst.staging==handles::INVALID_RESOURCE) continue;
                        ResourceBarrier barrier{};
                        barrier.resource=src.tex;
                        barrier.beforeState=ResourceState::Unknown;
                        barrier.afterState=ResourceState::CopySource;
                        context.cmdBuffer->InsertBarrier(&barrier,1);
                        BufferTextureCopyRegion rr{};
                        rr.imageSubresource.layerCount=1;
                        rr.imageExtent={dst.width,dst.height,1};
                        context.cmdBuffer->CopyTextureToBuffer(src.tex,dst.staging,&rr,1);
                        barrier.beforeState=ResourceState::CopySource;
                        barrier.afterState=ResourceState::ShaderResource;
                        context.cmdBuffer->InsertBarrier(&barrier,1);
                        copiedAny=true;
                    }
                    if (copiedAny) {
                        // InsertBarrier cannot certify destination buffers on
                        // Vulkan.  Keep this explicit dependency on the RG
                        // command and the checked standalone completion below.
                        context.cmdBuffer->MemoryBarrier(PipelineStage::Transfer, PipelineStage::Host,
                            AccessFlag::TransferWrite, AccessFlag::HostRead);
                    }
                    roi_audit_gain_=1.f;
                    if (const char* g=std::getenv("PRIMAL_GI_INDIRECT_GAIN")) {
                        const float v=float(std::atof(g));if (v>=0.f && v<=8.f) roi_audit_gain_=v;
                    }
                    roi_audit_mode_=inputs.pixel_gi_spgi_only?1.f:2.f;
                    if (const char* p=std::getenv("PRIMAL_FUSION_PERPIXEL"))
                        if (p[0]=='1') roi_audit_mode_=0.f;
                    roi_audit_frame_=inputs.frame_index;roi_audit_slot_=cbIdx;roi_audit_queued_=true;
                });
        }
    }
    return outputs;
}

} // namespace primal::graphics
