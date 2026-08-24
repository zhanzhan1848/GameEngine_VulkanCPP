/**
 * @file RuntimeVTVolume.h
 * @brief VT M5 — Runtime Virtual Texture Volume(世界 AABB 正交 -Z 投影)
 * @details RVT = GPU 按需生产的着色缓存。本实现:IVTProducer 的三 pass
 *          页生产(捕获 Draw → 压缩 CS → GPUFinalizer 存储镜像直写物理
 *          atlas),静态内容 = 世界空间棋盘(验证锚点;PCG 地形混合为
 *          后续接入点)。页淘汰后再次请求自动重生产(内容确定性)。
 *
 * 投影:世界 XZ 平面 → 虚拟纹理 UV(volume AABB);-Z 正交语义与 UE 一致
 *          (俯视)。
 *
 * @author GameEngine VulkanCPP Team
 * @date 2026-08-23
 */

#pragma once

#include "VirtualTextureSystem.h"
#include "../RHI/Core/RHITypes.h"
#include "../RHI/Core/RHICommand.h"

namespace primal::graphics::vt {

class runtime_vt_volume : public IVTProducer {
public:
    struct desc {
        float world_x0 = 0.0f, world_z0 = 0.0f;    // AABB(world_x0 < x1)
        float world_x1 = 256.0f, world_z1 = 256.0f;
        u32 vt_id = 0;                              // register_runtime_texture 返回
        u32 tile_size = 128;
        u32 border = 4;
    };

    bool initialize(RHIDeviceBase* device, const desc& d);
    void shutdown();

    // IVTProducer:三 pass 生产(同步;graphics queue)
    bool produce_page(RHIDeviceBase* device, u32 vt_id, u32 page, u32 layer,
                      ResourceHandle atlas, u32 atlas_x, u32 atlas_y) override;

    /// 页 (px,py,mip) → 世界矩形(含 border 外扩;图外 clamp)
    void page_world_rect(u32 px, u32 py, u32 mip, u32 pages_top,
                         float& wx0, float& wz0, float& wx1, float& wz1) const;

    u32 produced_pages() const { return produced_pages_; }

private:
    bool build_pipelines(RHIDeviceBase* device);
    bool ensure_targets(RHIDeviceBase* device);

    RHIDeviceBase* device_ = nullptr;
    desc desc_{};

    // 捕获 RT(136×136 RGBA8,Render+Sampled)
    ResourceHandle capture_rt_ = 0;
    // packed 中间纹理(压缩钩子输出;RGBA8 恒等)
    ResourceHandle packed_tex_ = 0;

    ShaderHandle capture_vs_ = 0, capture_fs_ = 0, compress_cs_ = 0, finalize_cs_ = 0;
    PipelineHandle capture_pipeline_ = 0;
    PipelineHandle compress_pipeline_ = 0;
    PipelineHandle finalize_pipeline_ = 0;
    PipelineLayoutHandle capture_layout_ = 0, compress_layout_ = 0, finalize_layout_ = 0;
    DescriptorSetLayoutHandle compress_ds_layout_ = 0, finalize_ds_layout_ = 0;
    DescriptorSetHandle compress_ds_ = 0, finalize_ds_ = 0;
    SamplerHandle sampler_ = 0;

    u32 produced_pages_ = 0;
};

} // namespace primal::graphics::vt
