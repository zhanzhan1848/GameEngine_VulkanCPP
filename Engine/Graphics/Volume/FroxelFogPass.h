#pragma once

#include "CommonHeaders.h"
#include "Graphics/Volume/FroxelTypes.h"
#include "Graphics/Volume/VolumeTypes.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rhi {
    class RHIDeviceBase;
}

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::volume {

struct FroxelInputs {
    rendergraph::RGResourceHandle gbuffer_depth;
    rendergraph::RGResourceHandle scene_color;
    // 旧输入：屏幕空间表面阴影可见性图。P2-B 契约修正后仅作 legacy 兼容
    // 保留字段，light integrate 不再采样它（屏幕 UV 与 light VP UV 不对应，
    // 且表面可见性不描述任意空间点的遮挡）。
    rendergraph::RGResourceHandle shadow_map;
    // P2-B: light-space VSM moments（RG32F z,z²，每级联一张）+ 匹配的
    // light VP 与级联数。cascade_count=0 时雾体恒 lit（明确定义的 fallback，
    // 不再用 camera depth 冒充阴影）。
    rendergraph::RGResourceHandle shadow_moments[4];
    // P1（reaudit §4.4）: PCSS 链的 light-space 深度图（D32→R32 blit，
    // 与 shadow_matrices 同源同帧）。Vulkan 端无 moments 时改做逐 froxel
    // 深度比较（shadow_mode=2），使默认 PCSS 场景获得真实体积阴影；
    // Metal MSL 尚未支持该模式，运行时按平台门控回退恒 lit。
    rendergraph::RGResourceHandle shadow_depth[4];
    math::m4x4 shadow_matrices[4];
    u32 shadow_cascade_count = 0;
    rendergraph::RGResourceHandle sdf_cascade_0;
    rendergraph::RGResourceHandle sdf_cascade_1;
    rendergraph::RGResourceHandle sdf_cascade_2;
    // WP2 (2026-09-30): 上一帧 SPGI 过滤输出（屏幕空间间接辐照）。雾积分在
    // 渲染图中先于本帧 SPGI 输出，该纹理在 fog pass 执行时尚未被本帧覆写
    // （同 cmd buffer 内图定序），读到的是"最近完成帧"——体积雾低频，计划
    // §4 WP2 认可的退路。INVALID 时 integrate 走硬编码天空色 fallback。
    // 有意不走 RG 跟踪（SPGI pass 自管理，跨帧直读与 froxel 纹理同模式）。
    rhi::ResourceHandle spgi_ambient_tex;
    VolumeCameraData camera_data;
    math::m4x4 shadow_matrix0;  // legacy（被 shadow_matrices[0..1] 取代）
    math::m4x4 shadow_matrix1;
    u32 width = 0;
    u32 height = 0;
};

struct FroxelOutput {
    rendergraph::RGResourceHandle volume_scatter;
    bool valid = false;
};

class FroxelFogPass {
public:
    FroxelFogPass() = default;
    ~FroxelFogPass();

    bool Initialize(rhi::RHIDeviceBase* device, u32 render_width, u32 render_height,
                    const FroxelGridConfig& config = {});
    void Shutdown();
    FroxelOutput AddPass(rendergraph::RenderGraph& graph, const FroxelInputs& inputs);
    bool IsInitialized() const { return initialized_; }

    // Debug/testing: resolved screen-space scatter texture (RGB=scatter, A=transmittance).
    rhi::ResourceHandle GetScatterTexture(u32 buffer_index) const {
        return scatter_texture_[buffer_index % 3];
    }
    // Debug/testing: resolved scatter texture dimensions (half render res).
    u32 GetScatterWidth() const { return render_width_ / 2; }
    u32 GetScatterHeight() const { return render_height_ / 2; }
    // Debug/testing: injected density grid (R16F, grid_width×grid_height×grid_depth).
    rhi::ResourceHandle GetDensityTexture(u32 buffer_index) const {
        return froxel_density_[buffer_index % 3];
    }
    // Debug/testing: per-slice integrated scatter grid（RGBA16F，
    // grid_width×grid_height×grid_depth，RGB=累计散射 A=透射率）。TestVulkan-
    // FroxelRenderGraph 门禁 G1-G5 的读回对象（计划 §6.4）。temporal 开启时
    // resolve 采的是混合结果（hist），此处返回与 resolve 同源纹理。
    rhi::ResourceHandle GetScatter3DTexture(u32 buffer_index) const {
        return temporal_enabled_
            ? froxel_scatter_hist_[buffer_index % 3]
            : froxel_scatter_[buffer_index % 3];
    }
    u32 GetGridWidth() const { return config_.grid_width; }
    u32 GetGridHeight() const { return config_.grid_height; }
    u32 GetGridDepth() const { return config_.grid_depth; }

    // ---- WP1/WP2/WP3 运行时旋钮（门禁 G5 双相位 / A-B 用；CB 每帧重传，
    // 免 pass 重建）----
    void SetMarchSteps(u32 steps) { march_steps_override_ = steps; }
    void SetTemporalEnabled(bool on) { temporal_enabled_ = on; }
    void SetAmbientSourceSPGI(bool on) { ambient_spgi_override_ = on ? 1u : 0u; }

private:
    void CreateDescriptorSetLayouts();
    void CreatePipelines();
    void CreateConstantBuffers();
    void CreateNoiseTexture();
    void CreateFroxelTextures();
    void CreateScatterTextures();

    bool              initialized_{ false };
    rhi::RHIDeviceBase* device_{ nullptr };
    u32               render_width_{ 0 };
    u32               render_height_{ 0 };
    FroxelGridConfig  config_{};

    // WP1/WP2/WP3 runtime knobs（见 public setters）
    u32   march_steps_override_{ 0xFFFFFFFFu };  // 0xFFFFFFFF=未覆写,用 config_
    u32   ambient_spgi_override_{ 0xFFFFFFFFu };
    bool  temporal_enabled_{ false };            // reproject pass 是否执行
    bool  temporal_available_{ false };          // reproject 管线编译成功

    // WP3: 上一帧 inverse view-projection（重投影用）。首帧用当前矩阵
    // （恒等重投影，temporal 输出 = 轻微平滑的当前值，无鬼影）。
    math::m4x4 prev_inv_view_proj_{};
    bool       has_prev_camera_{ false };

    // Pipelines (3: density_inject, light_integrate, resolve; WP3: +reproject)
    rhi::PipelineHandle density_inject_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle light_integrate_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle resolve_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle reproject_pipeline_{ rhi::handles::INVALID_PIPELINE };

    // Pipeline layouts
    rhi::PipelineLayoutHandle density_inject_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle light_integrate_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle resolve_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineLayoutHandle reproject_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };

    // Descriptor set layouts
    rhi::DescriptorSetLayoutHandle density_inject_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle light_integrate_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle resolve_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetLayoutHandle reproject_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

    // Descriptor sets (triple-buffered: 4 types x 3 frames)
    rhi::DescriptorSetHandle density_inject_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle light_integrate_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle resolve_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };
    rhi::DescriptorSetHandle reproject_ds_[3]{
        rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET, rhi::handles::INVALID_DESCRIPTOR_SET
    };

    // Constant buffers (triple-buffered)
    rhi::ResourceHandle params_cb_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };

    // Persistent noise texture
    rhi::ResourceHandle noise_texture_{ rhi::handles::INVALID_RESOURCE };
    // WP5 云原语（3D 纹理路线，用户建议对齐参考实现）：初始化时 CPU 生成
    // 积云体积（R8, CLOUD_RES³，密度 0-1 归一化），注入 pass 按云 AABB 采样。
    rhi::ResourceHandle cloud_texture_{ rhi::handles::INVALID_RESOURCE };
    void CreateCloudTexture();
    // P2-B: 1x1 2D shadow-moments stand-in（空槽位绑定，保持 view type 合法）
    rhi::ResourceHandle dummy_shadow_2d_{ rhi::handles::INVALID_RESOURCE };

    // Froxel 3D textures (triple-buffered)
    rhi::ResourceHandle froxel_density_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    rhi::ResourceHandle froxel_scatter_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // WP3: temporal 混合结果（resolve 采样源；下一帧作为 reproject 的 prev）。
    // triple 轮转下 prev 槽 = (outIdx+2)%3，上上帧写、本帧不被写。
    rhi::ResourceHandle froxel_scatter_hist_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
    // WP2: 1x1 2D 黑纹理（spgi_ambient 空槽位绑定）
    rhi::ResourceHandle dummy_ambient_2d_{ rhi::handles::INVALID_RESOURCE };

    // Output scatter texture (triple-buffered, RGBA16_Float 2D, half-res)
    rhi::ResourceHandle scatter_texture_[3]{
        rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE
    };
};

} // namespace primal::graphics::volume
