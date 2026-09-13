#pragma once

// ParticleFXRibbonPass.h — PFX ribbon/trail 渲染 pass（架构文档 §7-N6）。
//
// 数据流：CPU 侧 ribbon_trail::build_strip 产出 32B 条带顶点 + u16 索引 →
// stage_strip() 上传 per-frame mapped 缓冲 → execute() 以索引绘制提交。
// 管线：加色混合（拖尾发光观感）、深度测试开/写关。
// shader：EngineTest/shaders/ParticleFXRibbon.metal（运行时加载，沿 ParticlePass 惯例）。

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::graphics {

class ParticleFXRibbonPass {
public:
    ParticleFXRibbonPass() = default;
    ~ParticleFXRibbonPass() = default;

    static constexpr u32 max_vertices() { return 256; }       // 128 样本 × 2
    static constexpr u32 max_indices() { return 64 * 6 * 4; } // 冗余余量

    bool initialize(rhi::RHIDeviceBase* device);
    void shutdown();

    // 上传条带几何到 frame_index 的缓冲（build_strip 的输出）。
    void stage_strip(const void* vertices, u32 vertex_count, const u16* indices, u32 index_count,
                     u32 frame_index);
    u32 staged_vertex_count() const { return staged_vertices_; }
    u32 staged_index_count() const { return staged_indices_; }

    void set_color(f32 r, f32 g, f32 b, f32 a) {
        color_[0] = r; color_[1] = g; color_[2] = b; color_[3] = a;
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

    rhi::ResourceHandle vertex_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    rhi::ResourceHandle index_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    rhi::ResourceHandle uniform_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    u32 staged_vertices_{ 0 };
    u32 staged_indices_{ 0 };
    bool first_draw_logged_{ false };
    f32 color_[4]{ 1.f, 0.6f, 0.2f, 0.8f };
};

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
