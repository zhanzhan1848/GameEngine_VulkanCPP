#pragma once

// ParticleFXMeshPass.h — PFX mesh 粒子渲染 pass（架构文档 §7-N2 渲染半边）。
//
// 数据流：CPU 侧 render::pack_mesh_instances 打包 48B mesh_instance_data 实例缓冲
// （布局已锁）→ stage_instances() 上传到 per-frame mapped 缓冲 → execute() 以
// 索引化实例绘制（DrawIndexed，instanceCount>1）提交。
// 顶点/片段 shader：EngineTest/shaders/ParticleFXMesh.metal（顶点拉取，运行时加载，
// 沿 ParticlePass 的 Metal 路径惯例）；Vulkan 对称路径（WGSL/SPIR-V）为后续批次。
// 网格源 v1 为内置程序化立方体（含法线）；外部 mesh_id 绑定（RenderMesh）随后续
// 资产管线集成接入。

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <cstring>

namespace primal::graphics {

class ParticleFXMeshPass {
public:
    ParticleFXMeshPass() = default;
    ~ParticleFXMeshPass() = default;

    bool initialize(rhi::RHIDeviceBase* device);
    void shutdown();

    // 上传实例缓冲（render::pack_mesh_instances 的输出）到 frame_index 的缓冲。
    // count ≤ max_instances()。
    void stage_instances(const void* data, u32 count, u32 frame_index);
    u32  staged_count() const { return staged_count_; }
    static constexpr u32 max_instances() { return 4096; }

    void set_color(f32 r, f32 g, f32 b) {
        color_[0] = r;
        color_[1] = g;
        color_[2] = b;
        color_[3] = 1.f;
    }

    void execute(rhi::RHICommandBuffer* cmd_buffer,
                 u32 frame_index,
                 const math::m4x4& view_matrix,
                 const math::m4x4& projection_matrix);

private:
    struct VPUniforms {
        math::m4x4 view_projection;
        math::v4 color;
    };

    bool create_mesh_buffers();
    bool create_pipeline();

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

    // 网格源（程序化立方体）
    rhi::ResourceHandle position_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle normal_buffer_{ rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle index_buffer_{ rhi::handles::INVALID_RESOURCE };
    u32 index_count_{ 0 };

    // per-frame 实例与常量
    rhi::ResourceHandle instance_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    rhi::ResourceHandle uniform_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    u32 staged_count_{ 0 };
    bool first_draw_logged_{ false };
    f32 color_[4]{ 0.35f, 0.8f, 1.f, 1.f };
};

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
