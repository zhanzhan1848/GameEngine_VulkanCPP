#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

/// Inputs for the real-mesh DDGI probe sphere overlay.
struct DebugProbeSphereInputs {
    rendergraph::RGResourceHandle backbuffer_rg;   // fresh alias of the swapchain image (WRITE target)
    rendergraph::RGResourceHandle blit_output_rg;  // FinalBlit's output node — declared READ to serialize after it
    u32 current_buffer_index = 0;
    u32 render_width = 0;
    u32 render_height = 0;

    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    math::v4   camera_position{ 0, 0, 0, 0 };
    math::v4   sun_dir{ 0, 1, 0, 0 };              // toward light, normalized

    // Probe grid definition (instance transforms are built from this — the
    // same definition the trace chain uses; no per-frame derivation).
    math::v3   probe_origin{ 0, 0, 0 };
    u32 probe_counts[3] = {0, 0, 0};
    u32 probe_count = 0;
    f32  probe_spacing = 3.0f;
    f32  sphere_radius = 0.0f;   // 0 → spacing * 0.4

    // B1 L3.2: DDGI SH9 irradiance buffer (probeIdx×9 float4, triple-buffered
    // triple index = current_buffer_index). When valid, spheres sample real
    // probe irradiance by direction instead of the hue fallback.
    rhi::ResourceHandle irradiance_buffer{ rhi::handles::INVALID_RESOURCE };

    // [④ 2026-09-21] DDGI 深度 moments（probeIdx×128 floats = 8×8 mean+var，
    // DDGIUpdateDepth 写入；confidence/variance 模式的 DDGI 数据源）。可缺省
    // （INVALID → mode 2/3 回退 radiance 视图 + 一次性日志）。
    rhi::ResourceHandle ddgi_depth_buffer{ rhi::handles::INVALID_RESOURCE };

    // B1 L3 遮挡: GBuffer depth (D32_Float, already imported into the graph by
    // the caller). Attached as the overlay pass's depth attachment (Load/Don't-
    // Care) so spheres behind scene geometry fail the depth test. Only used
    // when depth_width/height match the render target (render_scale != 1 falls
    // back to the no-occlusion overlay).
    rendergraph::RGResourceHandle depth_rg{};
    u32 depth_width = 0;
    u32 depth_height = 0;

    // 2026-09-17 审计 §8.3：per-probe 重定位状态（CPU 读回的 relocated origins，
    // probeIdx 索引；xyz=有效起点，w=分类 0/0.5/1.0）。nullptr/0 = 无状态数据
    //（保持 legacy 色相）。version 变化触发 VB 重建（状态到达是一次性事件）。
    const math::v4* origin_states = nullptr;
    u32 origin_states_count = 0;
    u32 origin_states_version = 0;

    // [WRC] PassDebugView WRC Probes：有效时改走 WRC 采样管线——球面法线
    // 方向 cos 加权混合 64 方向 radiance（probe_data 布局见 WRCTypes.h），
    // 状态色由 buffer 内 state 字派生（忽略 origin_states）。
    rhi::ResourceHandle wrc_data_buffer{ rhi::handles::INVALID_RESOURCE };
};

/// Renders one real mesh sphere per DDGI probe on top of the tonemapped
/// scene: shared icosphere VB/IB + per-instance transform matrices, standard
/// vertex/fragment path, NO GPU culling (all probes draw), no descriptors
/// (view_proj/sun via push constants) — the exact shape of the engine's
/// proven instanced-mesh paths.
class DebugProbeSphereModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device,
                    rhi::ShaderHandle vertex_shader,
                    rhi::ShaderHandle pixel_shader);
    void Shutdown();

    void AddPass(rendergraph::RenderGraph& graph, const DebugProbeSphereInputs& inputs);

    bool IsInitialized() const { return pipeline_ != rhi::handles::INVALID_PIPELINE; }
    /// True when the SH9 irradiance-sampling pixel pipeline compiled (Metal).
    bool HasIrradiancePipeline() const { return irradiance_pipeline_ != rhi::handles::INVALID_PIPELINE; }
    /// True when the [WRC] 64-dir radiance-sampling pixel pipeline compiled.
    bool HasWrcPipeline() const { return wrc_pipeline_ != rhi::handles::INVALID_PIPELINE; }

private:
    bool BuildMeshAndInstances(const math::v3& origin, const u32 counts[3],
                               u32 count, f32 spacing, f32 radius,
                               f32 slice_center_y);

    rhi::RHIDeviceBase* device_ = nullptr;

    // B1 L3 遮挡 + L3.2: pipeline family selected per AddPass —
    //   [irradiance?] × [depth attachment?]. The no-depth variants reproduce
    // the pre-L3 behavior exactly (Vulkan test path passes no depth_rg).
    rhi::PipelineHandle pipeline_{ rhi::handles::INVALID_PIPELINE };                  // hue + depth
    rhi::PipelineHandle pipeline_nodepth_{ rhi::handles::INVALID_PIPELINE };          // hue, no depth
    rhi::PipelineHandle irradiance_pipeline_{ rhi::handles::INVALID_PIPELINE };       // SH9 + depth
    rhi::PipelineHandle irradiance_nodepth_pipeline_{ rhi::handles::INVALID_PIPELINE };// SH9, no depth
    rhi::PipelineHandle wrc_pipeline_{ rhi::handles::INVALID_PIPELINE };              // [WRC] 64-dir + depth
    rhi::PipelineHandle wrc_nodepth_pipeline_{ rhi::handles::INVALID_PIPELINE };      // [WRC], no depth
    rhi::PipelineLayoutHandle layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::DescriptorSetLayoutHandle set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::ResourceHandle ubo_[3]{
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE
    };

    // Expanded flat vertex stream: all probes' triangles baked out
    // (pos+normal+probeIdx+state per vertex, 32B stride). No instancing.
    rhi::ResourceHandle sphere_vb_{ rhi::handles::INVALID_RESOURCE };
    u32 instance_count_ = 0;   // total expanded vertex count
    math::v3 instances_origin_{};   // grid params the buffer was built for
    f32 instances_spacing_ = 0.0f;
    f32 instances_radius_ = 0.0f;
    // 2026-09-17：VB 重建键的附加项——状态版本（一次性读回后触发重建）与
    // 切片中心（量化到 spacing，相机跨层时重建）。
    u32 states_version_ = 0;
    f32 slice_center_y_ = 0.0f;
    // AddPass 暂存 → BuildMesh 固化的状态数据流。
    const math::v4* states_pending_ptr_ = nullptr;
    u32 states_pending_count_ = 0;
    u32 states_version_pending_ = 0;
    const math::v4* origin_states_ = nullptr;
    u32 origin_states_count_ = 0;
};

} // namespace primal::graphics
