#pragma once

#include "CommonHeaders.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"
#include "Graphics/Lumen/GIDebugParams.h"
#include "Graphics/Lumen/WRC/WRCTypes.h"
#include "Graphics/Lumen/WRC/WRCSlotMap.h"
#include "Graphics/Lumen/WRC/WRCCacheView.h"
#include "Graphics/Lumen/GICacheView.h"
#include "Graphics/RHI/Core/RHIReadbackRing.h"
#include <vector>
#include <string>

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::lumen {

// ============================================================================
// LumenWRCPass — [WRC] World Space Radiance Cache（独立代码域）。
//
// 标签：[WRC] ｜ 关联：[DDGI] [SC] [SPGI]
// 设计：Docs/2026-09-18-world-space-radiance-cache-design-and-plan.md
//
// 身份：纯 cache——缓存"trace SC/SDF 得到的 radiance"，只被上层（SPGI
// 远场）消费，从不下注能量到任何被缓存对象（能量单向律 §0.3）。
//
// 生命周期（三缓冲惯例）：
//   1. Initialize(device, params)  —— 一次
//   2. AddPass(graph, cam, frame)  —— 每帧（trace → finalize → update）
//   3. Shutdown()                  —— 一次
//
// PRIMAL_WRC_ENABLE=0（默认）时管线不创建本 pass —— A0 零行为变化。
// Metal 后端：A 线 Vulkan 先行（设计 §1.1），Metal 上 Initialize 返回
// false，接线层跳过（按 Docs/RHI/GraphicsAPI_Parity_Matrix.md 跟进）。
// ============================================================================

/// 每帧相机/光照输入（调用端填写；字段与 DDGI 无关）。
struct WRCCameraData {
    math::v3   light_direction;   ///< 归一化太阳方向
    math::v3   light_color;       ///< 太阳颜色（预乘强度）
    math::v3   sky_color;         ///< 天空色（xyz）；trace 逃逸契约用
    u32        frame_index;
};

class LumenWRCPass {
public:
    LumenWRCPass() = default;
    ~LumenWRCPass();

    bool Initialize(rhi::RHIDeviceBase* device, const WRCRuntimeParams& params = {});
    void Shutdown();

    /// trace → finalize → update（→ prefilter，PRIMAL_WRC_PREFILTER=1）。
    /// 纯 cache：无像素输出；消费端经 GetCacheView() 读探针数据。
    /// sc_lighting_atlas_rg：SC atlas 的 RG 句柄（声明写读屏障——无此声明
    /// RG 在 SC Lighting 写与本 pass 读之间不插屏障，atlas 数据竞争）。
    void AddPass(
        rendergraph::RenderGraph& graph,
        const WRCCameraData& camera_data,
        u32 current_frame_index,
        rendergraph::RGResourceHandle sc_lighting_atlas_rg = {});

    bool IsInitialized() const { return initialized_; }

    /// 消费视图（对齐 GICacheView 模式）。frame 为绝对帧号，内部经
    /// WRCSlotsForFrame 派生槽位。
    WRCCacheView GetCacheView(u32 frame_idx) const;

    /// SC 数据快照（trace finalize 的近场采样源）——从
    /// SurfaceCachePass::GetCacheView() 直传（视图签名，与消费端解耦）。
    void SetSurfaceCacheResources(const SurfaceCacheView& view);
    void ClearSurfaceCacheResources();

    /// 离线 SDF 数据源（与 GlobalSDF 运行时级联二选一；离线优先）。
    void SetOfflineSDFSource(rhi::ResourceHandle texture,
                             math::v3 origin, math::v3 extent, u32 resolution) {
        offline_sdf_texture_ = texture;
        offline_sdf_origin_ = origin;
        offline_sdf_extent_ = extent;
        offline_sdf_resolution_ = resolution;
        has_offline_sdf_ = (texture != rhi::handles::INVALID_RESOURCE && resolution > 0);
    }
    /// 有符号粗场旗标（Phase 4 零穿越命中判定；资产为 signed 烘焙时置位。
    /// PRIMAL_WRC_UNSIGNED_TRACE=1 强制回退无符号带判定）。
    void SetCoarseSdfSigned(bool s) { coarse_sdf_signed_ = s; }

    /// near/far 多 volume 实例的 RG 名字前缀（防两实例同名互踩，同 DDGI T4.4）。
    void SetRGNamePrefix(const std::string& prefix) { rg_name_prefix_ = prefix; }

    /// 场景切换时清探针场（valid 位全清——EMA 首帧改直写）。
    void ClearProbeField();

    /// 探针格世界原点（probe[0] 位置；A5 WRCFitGrid 自动化前的手工入口）。
    void SetVolumeOrigin(const math::v3& origin) { probe_origin_ = origin; }
    math::v3 GetVolumeOrigin() const { return probe_origin_; }

    /// 调试/降级 prefilter dispatch 开关（PRIMAL_WRC_PREFILTER=1）。
    void SetPrefilterEnabled(bool e) { prefilter_enabled_ = e; }

    /// 探针数据缓冲（诊断读回 / GIDebug wrc 通道数据面）。
    rhi::ResourceHandle GetProbeDataBuffer() const { return probe_data_; }
    /// 消费计数缓冲（u32/probe，SPGI 消费端原子累加；A5 预算优先级数据源）。
    rhi::ResourceHandle GetConsumeCounterBuffer() const { return consume_counter_; }
    /// A5: 场景贴合网格覆盖盒（离线 SDF 就位时 = merger volume；设置后
    /// InitializeLumenPasses 重建时经 WRCFitGridFromBounds 派生探针网格）。
    void SetFitBounds(math::v3 bmin, math::v3 bmax, float margin) {
        fit_bounds_min_ = bmin;
        fit_bounds_max_ = bmax;
        fit_margin_ = margin;
        has_fit_bounds_ = true;
    }
    bool HasFitBounds() const { return has_fit_bounds_; }
    math::v3 GetFitBoundsMin() const { return fit_bounds_min_; }
    math::v3 GetFitBoundsMax() const { return fit_bounds_max_; }
    float GetFitMargin() const { return fit_margin_; }

    const WRCRuntimeParams& GetParams() const { return params_; }
    u32 GetProbeCountTotal() const {
        return params_.probe_count_x * params_.probe_count_y * params_.probe_count_z;
    }
    /// 最近一次 GIDebug wrc 通道 dump 的统计（探针有效率/平均亮度）。
    struct DumpStats {
        u32 valid_probes = 0;
        u32 in_geometry_probes = 0;
        u32 total_probes = 0;
        float mean_lum = 0.0f;
    };
    const DumpStats& GetLastDumpStats() const { return dump_stats_; }

private:
    void CreatePipelines();
    void FillVolumeData(WRCVolumeData& vd, const WRCCameraData& cam, u32 frame);

    bool initialized_{ false };
    rhi::RHIDeviceBase* device_{ nullptr };
    WRCRuntimeParams params_{};

    // --- compute pipelines（trace/finalize/update + 调试 prefilter）---
    rhi::PipelineHandle trace_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle finalize_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle update_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle prefilter_pipeline_{ rhi::handles::INVALID_PIPELINE };

    rhi::PipelineLayoutHandle trace_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle finalize_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle update_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle prefilter_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };

    rhi::DescriptorSetLayoutHandle trace_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle finalize_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle update_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle prefilter_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

    rhi::DescriptorSetHandle trace_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET };
    rhi::DescriptorSetHandle finalize_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET };
    rhi::DescriptorSetHandle update_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET };
    rhi::DescriptorSetHandle prefilter_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET };

    // --- 持久 GPU 资源 ---
    // 探针 radiance storage（单缓冲持久 + RMW EMA：一线程独占一探针，无跨
    // 线程读写；消费端同帧 RG 定序在 update 之后——不需要三缓冲，
    // 内存预算复核见 WRCTypes.h）。
    rhi::ResourceHandle probe_data_{ rhi::handles::INVALID_RESOURCE };
    // 逐射线 scratch（本帧 trace/finalize 间传递；绝对探针索引 × K）。
    rhi::ResourceHandle ray_data_{ rhi::handles::INVALID_RESOURCE };
    // 逐射线命中距离（update-slot × K 索引）。
    rhi::ResourceHandle hit_distance_{ rhi::handles::INVALID_RESOURCE };
    // 消费计数（u32/probe，A3 SPGI 原子累加 / A5 预算优先级）。
    rhi::ResourceHandle consume_counter_{ rhi::handles::INVALID_RESOURCE };
    // 预算 update list（三缓冲，CPU 每帧填充轮转窗口）。
    rhi::ResourceHandle update_list_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE };
    u32 update_offset_{ 0 };   // 轮转游标（A5 起按消费计数优先级排序）
    // volume UBO（三缓冲）。
    rhi::ResourceHandle volume_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE };

    // prefilter 输出（SH9，调试/降级档；probeTotal × 9 × 16B）。
    rhi::ResourceHandle prefilter_sh_{ rhi::handles::INVALID_RESOURCE };

    // SC 数据（SetSurfaceCacheResources 快照）。
    rhi::ResourceHandle sc_lighting_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle sc_depth_atlas_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle sc_card_lookup_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle sc_card_data_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle sc_grid_offsets_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle sc_grid_cards_{ rhi::handles::INVALID_RESOURCE };
    u32 sc_atlas_size_{ 0 };
    u32 sc_lookup_count_{ 0 };
    bool sc_enabled_{ false };

    // 离线 SDF（优先于 GlobalSDF 运行时级联）。
    bool has_offline_sdf_{ false };
    rhi::ResourceHandle offline_sdf_texture_{ rhi::handles::INVALID_RESOURCE };
    math::v3 offline_sdf_origin_{ 0, 0, 0 };
    math::v3 offline_sdf_extent_{ 0, 0, 0 };
    u32 offline_sdf_resolution_{ 0 };
    bool coarse_sdf_signed_{ false };

    std::string rg_name_prefix_;
    // SC 缺席时的占位资源（1x1 RGBA16F dummy 2D + 16B dummy SSBO）。
    rhi::ResourceHandle dummy_2d_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle dummy_buffer_{ rhi::handles::INVALID_RESOURCE };
    bool dummy_2d_transitioned_{ false };   ///< 首次 dispatch 前 UNDEFINED→SRO 一次性转换
    math::v3 probe_origin_{ 0.0f, 0.0f, 0.0f };   ///< 探针格原点（SetVolumeOrigin）
    bool prefilter_enabled_{ false };              ///< PRIMAL_WRC_PREFILTER=1
    bool probe_field_seeded_{ false };             ///< 场景切换检测（ClearProbeField）
    rhi_readback_ring dump_ring_;                  ///< PRIMAL_WRC_DUMP 读回环
    u32 dump_copies_{ 0 };
    DumpStats dump_stats_{};
    // A5 需求驱动预算：消费计数周期读回（每 256 帧 copy，+2 帧 map）+
    // 优先级窗口（消费计数降序 → 0 计数轮转保底）。
    rhi_readback_ring consume_ring_;
    u32 consume_copies_{ 0 };
    std::vector<u32> consume_snapshot_;
    u32 last_consume_read_frame_{ 0 };
    // A5 场景贴合覆盖盒（离线 SDF 就位时由接线端设置）。
    bool has_fit_bounds_{ false };
    math::v3 fit_bounds_min_{ 0.0f };
    math::v3 fit_bounds_max_{ 0.0f };
    float fit_margin_{ 0.0f };
};

} // namespace primal::graphics::lumen
