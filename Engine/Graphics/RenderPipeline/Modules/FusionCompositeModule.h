#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }
namespace lumen { class ScreenProbeGIPass; }

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

    // Volume scatter (RGBA16_Float: RGB=scatter, A=transmittance)
    rendergraph::RGResourceHandle volume_scatter_rg{};
    rhi::ResourceHandle volume_scatter_tex{rhi::handles::INVALID_RESOURCE};

    u32 current_buffer_index = 0;
    u32 render_width = 0;
    u32 render_height = 0;
    rhi::ResourceHandle black_texture{rhi::handles::INVALID_RESOURCE};

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
    rhi::ResourceHandle GetIndirectOutput(u32 cb) const { return indirect_output_[cb % 3]; }
    rhi::ResourceHandle GetFusionOutput(u32 cb) const { return fusion_output_[cb % 3]; }


    FusionOutputs AddPasses(rendergraph::RenderGraph& graph, const FusionInputs& inputs);

private:
    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;

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
