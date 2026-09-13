#pragma once

// ParticleFXDecalPass.h — PFX decal 渲染器（架构文档 §7-N6，延迟贴花 v1）。
// 全屏实例绘制：片段采样场景深度重建世界位置，球形体积内径向衰减着色，
// alpha 混合叠加到光照输出。实例数据 48B（render::decal_instance_data，布局锁定）。

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::graphics {

class ParticleFXDecalPass {
public:
    ParticleFXDecalPass() = default;
    ~ParticleFXDecalPass() = default;

    static constexpr u32 max_decals() { return 256; }
    static constexpr u32 instance_stride() { return 48; }

    bool initialize(rhi::RHIDeviceBase* device);
    void shutdown();

    // 绑定场景深度（重建世界位置必需）。
    void set_scene_depth(rhi::ResourceHandle depth) { scene_depth_ = depth; }

    void stage_decals(const void* data, u32 count, u32 frame_index);
    u32 staged_count() const { return staged_count_; }

    void execute(rhi::RHICommandBuffer* cmd_buffer,
                 u32 frame_index,
                 const math::m4x4& view_matrix,
                 const math::m4x4& projection_matrix);

private:
    struct VPUniforms {
        math::m4x4 inv_view_proj;
        math::v4 screen_size;
        math::v4 decal_color;
        math::v4 pad;
    };

    rhi::RHIDeviceBase* device_{ nullptr };
    rhi::ShaderHandle vertex_shader_{ rhi::handles::INVALID_SHADER };
    rhi::ShaderHandle fragment_shader_{ rhi::handles::INVALID_SHADER };
    rhi::PipelineLayoutHandle pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineHandle pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::DescriptorSetLayoutHandle descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle descriptor_sets_[rhi::MAX_FRAMES_IN_FLIGHT]{
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
    };
    rhi::ResourceHandle scene_depth_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle decal_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    rhi::ResourceHandle uniform_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    u32 staged_count_{ 0 };
    bool first_draw_logged_{ false };
};

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
