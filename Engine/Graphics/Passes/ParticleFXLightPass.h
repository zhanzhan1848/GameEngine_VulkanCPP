#pragma once

// ParticleFXLightPass.h — PFX light 渲染器（架构文档 §7-N6）。
// 将最多 max_lights() 个粒子渲染为 HDR 自发光 billboard（加色进光照输出）。
// 实例数据 32B：float4 pos_intensity(xyz,w) + float4 color_rgb_pad。
// 选择策略由调用方决定（v1：前 N 个活粒子；按亮度 Top-N 为增强项）。

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::graphics {

class ParticleFXLightPass {
public:
    ParticleFXLightPass() = default;
    ~ParticleFXLightPass() = default;

    static constexpr u32 max_lights() { return 256; }
    static constexpr u32 instance_stride() { return 32; }

    bool initialize(rhi::RHIDeviceBase* device);
    void shutdown();

    void stage_lights(const void* data, u32 count, u32 frame_index);
    u32 staged_count() const { return staged_count_; }

    void execute(rhi::RHICommandBuffer* cmd_buffer,
                 u32 frame_index,
                 const math::m4x4& view_matrix,
                 const math::m4x4& projection_matrix);

private:
    struct VPUniforms {
        math::m4x4 view_projection;
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
    rhi::ResourceHandle light_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    rhi::ResourceHandle uniform_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    u32 staged_count_{ 0 };
    bool first_draw_logged_{ false };
};

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
