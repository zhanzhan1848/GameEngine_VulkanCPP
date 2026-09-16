#pragma once

// ParticleFXMeshPass.h — PFX mesh 粒子渲染 pass（架构文档 §7-N2 渲染半边）。
//
// 数据流：CPU 侧 render::pack_mesh_instances 打包 48B mesh_instance_data 实例缓冲
// （布局已锁）→ stage_instances() 上传到 per-frame mapped 缓冲 → execute() 按几何槽
// 分组索引化实例绘制（每槽一次 BindDescriptorSets/BindIndexBuffer/DrawIndexed）。
// 顶点/片段 shader：EngineTest/shaders/ParticleFXMesh.metal（顶点拉取，运行时加载，
// 沿 ParticlePass 的 Metal 路径惯例）；Vulkan 对称路径（WGSL/SPIR-V）为后续批次。
// 几何槽：slot 0 = 内置程序化立方体（回退/默认）；外部 mesh 经 register_geometry()
// 注册（ForwardSceneRenderer 经 render::load_instance_geometry 懒加载后调用）。

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
    // instance_slots 为每实例的几何槽（null = 全部 slot 0）；count ≤ max_instances()。
    void stage_instances(const void* data, const u32* instance_slots, u32 count, u32 frame_index);
    u32  staged_count() const { return staged_count_; }
    static constexpr u32 max_instances() { return 4096; }

    // 注册外部几何源（渲染 instance）。返回槽号（≥2；0 = 内置立方体，1 = 内置球体，
    // 均在 initialize 预留），槽满/创建失败返回 u32_invalid_id。调用方缓存路径→槽映射。
    u32 register_geometry(const math::v3* positions, const math::v3* normals, u32 vertex_count,
                          const u32* indices, u32 index_count);
    static constexpr u32 kMaxGeometrySlots{ 8 };
    static constexpr u32 max_geometry_slots() { return kMaxGeometrySlots; }
    static constexpr u32 builtin_cube_slot() { return 0; }
    static constexpr u32 builtin_sphere_slot() { return 1; }

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

    // 几何槽：静态顶点/索引缓冲 + 每帧描述符集（binding 0/1 指向本槽几何，
    // 2/3 共享全局实例/常量缓冲）。
    struct geometry_buffers {
        rhi::ResourceHandle positions{ rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle normals{ rhi::handles::INVALID_RESOURCE };
        rhi::ResourceHandle index{ rhi::handles::INVALID_RESOURCE };
        u32 vertex_count{ 0 };
        u32 index_count{ 0 };
        bool index_u32{ false };
        rhi::DescriptorSetHandle descriptor_sets[rhi::MAX_FRAMES_IN_FLIGHT]{
            rhi::handles::INVALID_DESCRIPTOR_SET,
            rhi::handles::INVALID_DESCRIPTOR_SET,
            rhi::handles::INVALID_DESCRIPTOR_SET,
        };
    };

    bool create_pipeline();
    bool create_buffer_and_upload(rhi::ResourceHandle& out, u64 size, u32 element_count,
                                  u32 element_stride, const void* data);
    bool create_geometry_descriptor_sets(geometry_buffers& g);
    const geometry_buffers& slot_geometry(u32 slot) const {
        return slots_[slot < geometry_count_ ? slot : 0];
    }

    rhi::RHIDeviceBase* device_{ nullptr };

    rhi::ShaderHandle vertex_shader_{ rhi::handles::INVALID_SHADER };
    rhi::ShaderHandle fragment_shader_{ rhi::handles::INVALID_SHADER };
    rhi::PipelineLayoutHandle pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineHandle pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::DescriptorSetLayoutHandle descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };

    // 几何槽表（[0] = 内置立方体；其余由 register_geometry 填充）
    geometry_buffers slots_[kMaxGeometrySlots];
    u32 geometry_count_{ 1 };

    // per-frame 实例与常量（全槽共享）
    rhi::ResourceHandle instance_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};
    rhi::ResourceHandle uniform_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{};

    u32 instance_slots_[4096]{};
    u32 staged_count_{ 0 };
    bool first_draw_logged_{ false };
    f32 color_[4]{ 0.35f, 0.8f, 1.f, 1.f };
};

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
