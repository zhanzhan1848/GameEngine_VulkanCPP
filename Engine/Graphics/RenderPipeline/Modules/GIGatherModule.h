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
    bool IsInitialized() const { return pipeline_ != rhi::handles::INVALID_PIPELINE; }

private:
    rhi::RHIDeviceBase* device_ = nullptr;
    u32 render_width_ = 0;
    u32 render_height_ = 0;

    rhi::PipelineHandle pipeline_{rhi::handles::INVALID_PIPELINE};
    rhi::PipelineLayoutHandle layout_{rhi::handles::INVALID_PIPELINE_LAYOUT};
    rhi::DescriptorSetLayoutHandle set_layout_{rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT};
    rhi::DescriptorSetHandle descriptor_set_{rhi::handles::INVALID_DESCRIPTOR_SET};

    rhi::ResourceHandle gi_halfres_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gi_halfres_history_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle gi_specular_texture_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle probe_cb_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle far_cb_{rhi::handles::INVALID_RESOURCE};      // T4.4 far volume params
    rhi::ResourceHandle static_probe_cb_{rhi::handles::INVALID_RESOURCE};
    rhi::ResourceHandle dummy_buffer_{rhi::handles::INVALID_RESOURCE};
    // T6.3 sub-B: N-volume 参数 CB（binding 18，144B std140）+ 能力位。
    // nvolume_supported_ = Vulkan 且设备开启 descriptor indexing
    // （VulkanDevice::SupportsDescriptorIndexing）。不可用时 DSL 不带
    // PartiallyBound 标志、CB meta.x 恒 0 → shader 走 legacy 两槽路径。
    rhi::ResourceHandle nvolumes_cb_{rhi::handles::INVALID_RESOURCE};
    bool nvolume_supported_ = false;

    // Debug (SC_DUMP_CARDS=1): staging copy of the gather output + dump frame
    // (deferred print >=3 frames later — correlates probe health with what
    // Fusion actually receives).
    rhi::ResourceHandle out_staging_{rhi::handles::INVALID_RESOURCE};
    u64  out_staging_size_ = 0;
    u32  out_dump_frame_ = 0xFFFFFFFFu;
};

} // namespace primal::graphics
