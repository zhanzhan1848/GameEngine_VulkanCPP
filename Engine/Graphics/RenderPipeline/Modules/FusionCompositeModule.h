#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/Lumen/DDGISlotMap.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }
namespace lumen { class ScreenProbeGIPass; }
class LightmapModule;

struct FusionInputs {
    // Scene color (deferred output or raw albedo)
    rendergraph::RGResourceHandle primary_input_rg{};
    rhi::ResourceHandle primary_input_tex{rhi::handles::INVALID_RESOURCE};

    // GI textures (may be invalid if feature disabled)
    rendergraph::RGResourceHandle ssgi_rg{};
    rhi::ResourceHandle ssgi_tex{rhi::handles::INVALID_RESOURCE};
    rendergraph::RGResourceHandle ddgi_rg{};
    rhi::ResourceHandle ddgi_tex{rhi::handles::INVALID_RESOURCE};
    rendergraph::RGResourceHandle spgi_rg{};
    rhi::ResourceHandle spgi_tex{rhi::handles::INVALID_RESOURCE};
    // SPGI pass 指针——诊断 readback 取持久物理纹理（绕 RG 瞬态布局迁移）。
    lumen::ScreenProbeGIPass* spgi_pass = nullptr;
    rendergraph::RGResourceHandle ssao_rg{};
    rhi::ResourceHandle ssao_tex{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gbuffer_albedo{rhi::handles::INVALID_RESOURCE};

    // SSR reflection color (RGBA16_Float: RGB=reflection, A=reflection strength mask).
    // May be invalid if SSR is disabled — FusionComposite falls back to black.
    rendergraph::RGResourceHandle ssr_rg{};
    rhi::ResourceHandle ssr_tex{rhi::handles::INVALID_RESOURCE};
    // GBuffer ORM (AO.r, Roughness.g, Metallic.b) — used to attenuate SSR by roughness.
    rhi::ResourceHandle gbuffer_orm{rhi::handles::INVALID_RESOURCE};
    // T4.3: DDGI glossy specular (half-res RGBA16F: RGB=SH(R) lobe radiance,
    // A=NdotV). Invalid → black → glossy relay degrades to SSR-only.
    rendergraph::RGResourceHandle ddgi_specular_rg{};
    rhi::ResourceHandle ddgi_specular_tex{rhi::handles::INVALID_RESOURCE};

    // [R4/R5 2026-09-20] 屏幕外 specular 兜底（ReflSpecularTrace 输出，
    // 全分辨率 RGBA16F：RGB = Fresnel 加权后的屏幕外反射辐射，A = 权重）。
    // Invalid → black → v1 形态（SSR + glossy relay）。
    rendergraph::RGResourceHandle refl_specular_rg{};
    rhi::ResourceHandle refl_specular_tex{rhi::handles::INVALID_RESOURCE};

    // Volume scatter (RGBA16_Float: RGB=scatter, A=transmittance)
    rendergraph::RGResourceHandle volume_scatter_rg{};
    rhi::ResourceHandle volume_scatter_tex{rhi::handles::INVALID_RESOURCE};

    u32 current_buffer_index = 0;
    // Phase 1d: 绝对帧号——三缓冲槽位单一事实源 DDGISlotMap.h（cbIdx 时钟
    // 废除，与 DDGI 体系统一；current_buffer_index 仅为兼容保留）。
    u32 frame_index = 0;
    u32 render_width = 0;
    u32 render_height = 0;
    rhi::ResourceHandle black_texture{rhi::handles::INVALID_RESOURCE};

    // WRC 计划 Stream C1（Fusion 档位化）：像素级 GI 来源档位——true =
    // High/Ultra（axes.use_screen_probes）→ 像素只读 SPGI；false = Medium
    // → 像素只读 DDGI。use_screen_probes 档位结构不变（决定 SPGI 是否运行）；
    // 本位只决定 fusion 像素读哪路。PRIMAL_FUSION_PERPIXEL=1 整体回滚旧
    // 逐像素置信度混合（A/B）。
    bool pixel_gi_spgi_only = false;

    // sun-audit P1（暗地面 GI 逐段查）: GI_LADDER_DUMP 能量台账输入——
    // 直射可见性（ShadowFilter 半分辨率 R8，读回的是上一完成帧内容，锁定
    // 机位下逐帧一致）与 invVP（dump 像素 → 世界射线，固定世界位置标注）。
    rhi::ResourceHandle shadow_vis_tex{rhi::handles::INVALID_RESOURCE};
    math::m4x4 inv_view_projection{};
};

// T4.6.5 part 30.6 (X4 fix): default member initializers prevent stack garbage.
// Call sites that skip AddPasses (e.g. fusion_module_==nullptr OR Lumen disabled)
// leave FusionOutputs uninitialized → garbage output_tex=0 resolves via FreeList
// to slot 0 (first swapchain backbuffer) → FinalBlit samples a non-SAMPLED image.
struct FusionOutputs {
    rendergraph::RGResourceHandle output_rg{};
    rhi::ResourceHandle output_tex{rhi::handles::INVALID_RESOURCE};
};

class FusionCompositeModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device,
                    rhi::ShaderHandle indirect_ps, rhi::ShaderHandle composite_vs,
                    rhi::ShaderHandle composite_ps,
                    u32 render_width, u32 render_height);
    void Shutdown();

    // Debug/testing: resolved fusion intermediate outputs.
    // Phase 1d: 参数 = 绝对帧号（原 cbIdx 槽号废除）。indirect 返回上一完成
    // 槽（previousIndex——DDGI finalize 的 prev-screen 供给语义）；fusion 返回
    // 本帧完成槽（currentIndex）。
    rhi::ResourceHandle GetIndirectOutput(u32 frame) const {
        return indirect_output_[lumen::DDGISlotsForFrame(frame).previousIndex];
    }
    rhi::ResourceHandle GetFusionOutput(u32 frame) const {
        return fusion_output_[lumen::DDGISlotsForFrame(frame).currentIndex];
    }


    FusionOutputs AddPasses(rendergraph::RenderGraph& graph, const FusionInputs& inputs);

    // Lightmap 旁路（2026-10-19 计划）：注入改写级模块。非空且场景注册了
    // lightmap 页时，AddPasses 在 FusionIndirect 与 FusionComposite 之间
    // 追加该模块的改写 pass，并将 Pass 2 的 indirect 绑定切到其输出；
    // 未注册时零额外 pass、绑定原样——既有路径零差异。
    void SetLightmapHook(LightmapModule* m) { lightmap_hook_ = m; }

private:
    static constexpr u32 kRoiAuditSourceCount = 13;
    static constexpr u32 kRoiAuditCompositeBindingCount = 9;

    struct RoiAuditSource {
        bool constant_black{false};
        rhi::ResourceHandle staging{rhi::handles::INVALID_RESOURCE};
        // The handle that was actually written into the sampled-image
        // descriptor.  This is kept separately from staging so the audit
        // cannot accidentally turn a guessed/fallback source into evidence.
        rhi::ResourceHandle bound_resource{rhi::handles::INVALID_RESOURCE};
        u32 width{0}, height{0}, bytes_per_pixel{0};
        u32 binding{0xFFFFFFFFu};
        const char* name{nullptr};
        const char* format{nullptr};
        const char* layout{"unknown"};
        const char* source_identity{nullptr};
        rhi::DataFormat runtime_format{rhi::DataFormat::Unknown};
    };
    RoiAuditSource roi_audit_sources_[kRoiAuditSourceCount]{};
    // Captured at the point where FusionComposite's descriptor params are
    // assembled.  The later RG readback pass consumes these exact handles;
    // it never reconstructs them from FusionInputs or guessed formats.
    rhi::ResourceHandle roi_audit_composite_bound_[3][kRoiAuditCompositeBindingCount]{
        {
            rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE
        },
        {
            rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE
        },
        {
            rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
            rhi::handles::INVALID_RESOURCE
        }
    };
    rhi::ResourceHandle roi_audit_indirect_bound_[3][5]{};
    u32 roi_audit_indirect_bound_frame_[3]{};
    bool roi_audit_indirect_bound_valid_[3]{};
    u32 roi_audit_composite_bound_frame_[3]{};
    bool roi_audit_composite_bound_valid_[3]{};
    bool roi_audit_queued_{false}, roi_audit_dumped_{false};
    u32 roi_audit_frame_{0}, roi_audit_slot_{0};
    float roi_audit_gain_{1}, roi_audit_mode_{0};
    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;

    // Lightmap 旁路（2026-10-19 计划）：不拥有生命周期，由
    // StandardRenderPipeline 持有并注入。
    LightmapModule* lightmap_hook_ = nullptr;

    // Pass 1: FusionIndirect (half-res, 5 textures)
    rhi::PipelineHandle indirect_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle indirect_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle indirect_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle indirect_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle indirect_output_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // UNITS: DiffuseOutgoingRadiance（Lo = albedo·E/π，FusionIndirect.frag
    // 输出；审计 §5 铁律 2：作为 ray radiance 合法、作为 irradiance 非法）。

    // Pass 2: FusionComposite (full-res, 2 textures)
    rhi::PipelineHandle composite_pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle composite_layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle composite_set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle composite_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle fusion_output_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // UNITS: ScreenIndirectLighting 合成输出前的融合中间面（Lo 量纲，合成在
    // primary 直射之上进 TAA）。

    // Shared vertex shader (full-screen triangle)
    rhi::ShaderHandle vertex_shader_{rhi::handles::INVALID_SHADER};

    // Vulkan-only: explicit linear-clamp sampler (Metal uses implicit samplers).
    rhi::SamplerHandle default_sampler_{rhi::handles::INVALID_SAMPLER};
    // sun-audit P2 用户反馈（间接光过弱）：GI 融合增益运行时旋钮（UBO，三缓冲）
    rhi::ResourceHandle indirect_gain_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};

    // 1x1 identity texture for volume scatter fallback: RGBA = (0,0,0,1)
    rhi::ResourceHandle volume_identity_tex_{rhi::handles::INVALID_RESOURCE};

    // PRIMAL_IND_ASYNC=1 诊断：无同步污染的 indirect 条带统计读回环。
    // 拷贝挂进本帧命令缓冲，槽位随 cbIdx 三轮转——帧 N 映射槽 N%3 时其内容
    // 来自帧 N-3（该槽 fence 已在 BeginFrame 等过），全程零额外 GPU 同步。
    // 第八轮结论：SC_DUMP_CARDS 一次性读回的帧中途 WaitForCompletion 会意外
    // 掩盖跨命令缓冲竞态，此环即为在不受污染的时序下观测同一数据面。
    rhi::ResourceHandle async_stats_staging_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // 同一诊断门的 primary（deferred 直射）输入读回环——直射区与 GI 区同时
    // 爆白时，用它区分爆白源在 deferred 输出还是 fusion 合成。
    rhi::ResourceHandle primary_stats_staging_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // fusion 输出（合成结果，进 TAA 前）与 ddgi（GIGather 原始输出）读回环。
    rhi::ResourceHandle fusion_stats_staging_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle ddgi_stats_staging_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // spgi（ScreenProbeGI 全分辨率输出，RGB=辐照 A=置信度）读回环——能量标定
    // 与 DDGI 槽位同数据面对比（mask 位 128）。
    rhi::ResourceHandle spgi_stats_staging_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
};

} // namespace primal::graphics
