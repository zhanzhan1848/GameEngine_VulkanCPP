#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/Lumen/GICacheView.h"

namespace primal::graphics {
namespace rendergraph { class RenderGraph; }

namespace lumen { class LumenDDGIPass; class StaticProbeVolume; }

struct GIGatherInputs {
    // T6.3 sub-B: 全部 DDGI volume 视图（上限 4，与 ddgi_volumes_ 容量一致）。
    // valid 数 ≥ 2 且设备支持 descriptor indexing 时，GIGatherModule 走 N-volume
    // gather 路径（binding 17 SSBO 数组 + binding 18 参数 CB）；否则回退
    // near+far 两槽路径（行为与 T4.4/T5.5 一致）。volume_views[0] 缺省时模块
    // 用 ddgi_pass 视图补齐，[1] 用 far_volume_view/ddgi_far_pass——现有调用
    // 点不填本字段也完全兼容（零行为变化）。
    static constexpr u32 kMaxVolumes = 4;
    lumen::DDGIVolumeView volume_views[kMaxVolumes]{};
    u32 volume_count = 0;

    lumen::LumenDDGIPass* ddgi_pass = nullptr;
    lumen::LumenDDGIPass* ddgi_far_pass = nullptr;  // T4.4 far-field fallback
    lumen::StaticProbeVolume* static_probe_volume = nullptr;

    // X2/A3: 远场 volume 数据视图（可选）。有效时远场 CB 上传与 binding 16
    // 的 irradiance 句柄优先从 view 取数；无效时回退 ddgi_far_pass 指针路径
    // （两条路径字段来源相同，上传逐字节等价）。ddgi_far_pass 仍负责 RG 侧
    // "DDGIFarIrrRead" import 声明，保持 RG 声明零变化。
    lumen::DDGIVolumeView far_volume_view{};

    rhi::ResourceHandle gbuffer_depth;
    rhi::ResourceHandle gbuffer_normal;
    rhi::ResourceHandle gbuffer_orm;      // T4.3: roughness for lobe widening
    rendergraph::RGResourceHandle gbuffer_depth_rg;
    rendergraph::RGResourceHandle gbuffer_normal_rg;
    rendergraph::RGResourceHandle gbuffer_orm_rg;

    math::m4x4 view_matrix{};
    math::m4x4 proj_matrix{};
    u32 current_buffer_index = 0;
    // Irradiance triple-buffer slot source. DDGI/SC-DDGI writers rotate on
    // the frame counter (slot = frame_index % 3); current_buffer_index is a
    // swapchain index that can drift out of phase with it. Callers that leave
    // this at the sentinel keep the legacy (cbIdx + 2) % 3 read slot.
    static constexpr u32 kFrameIndexUnset = 0xFFFFFFFFu;
    u32 frame_index = kFrameIndexUnset;
    rhi::ResourceHandle fallback_texture{rhi::handles::INVALID_RESOURCE};  // when ORM missing
};

struct GIGatherOutputs {
    rendergraph::RGResourceHandle gi_output_rg;
    rhi::ResourceHandle gi_output_tex;
    // T4.3: half-res glossy specular (RGB = SH(R) lobe radiance, A = NdotV).
    rendergraph::RGResourceHandle gi_specular_rg;
    rhi::ResourceHandle gi_specular_tex;
};

class GIGatherModule {
public:
    bool Initialize(rhi::RHIDeviceBase* device, rhi::ShaderHandle shader,
                    u32 render_width, u32 render_height);
    void Shutdown();

    GIGatherOutputs AddPasses(rendergraph::RenderGraph& graph, const GIGatherInputs& inputs);

    rhi::ResourceHandle GetOutputTexture() const { return gi_halfres_texture_; }
    // gather 实际输出分辨率（Metal 全分辨率，Vulkan 维持半分辨率基线）。
    u32 GetGatherWidth() const { return gather_width_; }
    u32 GetGatherHeight() const { return gather_height_; }
    bool IsInitialized() const { return pipeline_ != rhi::handles::INVALID_PIPELINE; }

private:
    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;
    u32 gather_width_ = 0;
    u32 gather_height_ = 0;

    rhi::PipelineHandle pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    // Rotate CPU-updated bindings with the in-flight command buffers.  This is
    // ordinary resource-lifetime isolation; DDGI probe-state consistency is
    // handled separately by the update/integration passes.
    rhi::DescriptorSetHandle descriptor_sets_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET};

    rhi::ResourceHandle gi_halfres_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gi_halfres_history_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gi_specular_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle probe_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle far_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};      // T4.4 far volume params
    rhi::ResourceHandle static_probe_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dummy_buffer_{rhi::handles::INVALID_RESOURCE};
    // T6.3 sub-B → T3.6/D1: N-volume 参数 CB（144B std140）。原 binding 18
    // NVolumeCB 原位改型为 FlatVolumeCB（probe offset 表）。
    rhi::ResourceHandle nvolumes_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    // T3.6/D1: flat irradiance SSBO（triple-buffered，按需增长）。所有 volume
    // 的 9-vec4/probe 探针数据连续打包：[header 256B][vol0][vol1]...；描述符
    // 绑 offset 256 跳过头部，shader 侧即纯探针数组。取代 Vulkan 的
    // descriptor-indexing SSBO 数组（原 binding 17 array[4]）与双端 legacy
    // near/far 双槽绑定——Metal 与 Vulkan 走同一条 flat 路径。
    rhi::ResourceHandle flat_irr_buffer_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    u64 flat_irr_capacity_[3]{0, 0, 0};
    // GI 根治 Phase 3.2: flat openSky 缓冲（256B 头 + probes×4B，同槽位轮转）。
    rhi::ResourceHandle flat_opensky_buffer_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE};
    u64 flat_opensky_capacity_[3]{0, 0, 0};   // 已分配字节数（16 头部 + probes×144）

    // Debug (SC_DUMP_CARDS=1): staging copy of the gather output + dump frame
    // (deferred print >=3 frames later — correlates probe health with what
    // Fusion actually receives).
    rhi::ResourceHandle out_staging_{rhi::handles::INVALID_RESOURCE};
    u64  out_staging_size_ = 0;
    u32  out_dump_frame_ = 0xFFFFFFFFu;
};

} // namespace primal::graphics
