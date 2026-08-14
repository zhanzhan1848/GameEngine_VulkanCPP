#pragma once

#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"
#include "Particles/ParticleTypes.h"
#include "Particles/ParticleSorter.h"
#ifndef DISABLE_PARTICLE_SYSTEM



namespace primal::graphics {

class ParticlePass {
public:
    ParticlePass();
    ~ParticlePass();
    
    bool initialize(rhi::RHIDeviceBase* device);
    void shutdown();
    
    void execute(rhi::RHICommandBuffer* cmd_buffer,
                 u32 frame_index,
                 const math::m4x4& view_matrix,
                 const math::m4x4& projection_matrix);
    
    void set_blend_mode(particles::blend_mode mode);
    void set_depth_write_enabled(bool enabled);
    void set_depth_test_enabled(bool enabled);
    
    void set_texture_sheet(u32 frames_x, u32 frames_y, f32 frame_rate);
    void set_particle_texture(rhi::ResourceHandle texture);
    void set_sorting_enabled(bool enabled) { enable_sorting_ = enabled; }
    bool is_sorting_enabled() const { return enable_sorting_; }
    rhi::PipelineHandle get_current_pipeline() const;
private:
    struct ParticlePushConstants {
        math::m4x4 view_projection;
        math::m4x4 view_matrix;
        f32 time;
        f32 delta_time;
        u32 blend_mode;
        u32 texture_frames_x;
        u32 texture_frames_y;
        f32 frame_rate;
    };
    
    rhi::RHIDeviceBase* device_{ nullptr };
    
    rhi::ShaderHandle vertex_shader_{ rhi::handles::INVALID_SHADER };
    rhi::ShaderHandle fragment_shader_{ rhi::handles::INVALID_SHADER };
    
    rhi::PipelineLayoutHandle pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineHandle pipelines_[4]{ 
        rhi::handles::INVALID_PIPELINE, 
        rhi::handles::INVALID_PIPELINE, 
        rhi::handles::INVALID_PIPELINE, 
        rhi::handles::INVALID_PIPELINE 
    };  // additive, alpha, multiply, premultiplied
    
    rhi::DescriptorSetLayoutHandle descriptor_set_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle descriptor_sets_[rhi::MAX_FRAMES_IN_FLIGHT]{ 
        rhi::handles::INVALID_DESCRIPTOR_SET, 
        rhi::handles::INVALID_DESCRIPTOR_SET, 
        rhi::handles::INVALID_DESCRIPTOR_SET 
    };
    rhi::ResourceHandle particle_texture_{ rhi::handles::INVALID_RESOURCE };
    rhi::SamplerHandle defaultSampler_{ rhi::handles::INVALID_SAMPLER };
    
    particles::blend_mode current_blend_mode_{ particles::blend_mode::additive };
    bool depth_write_enabled_{ false };
    bool depth_test_enabled_{ false };  // DEBUG: Disabled for visibility testing
    
    u32 texture_frames_x_{ 1 };
    u32 texture_frames_y_{ 1 };
    f32 texture_frame_rate_{ 0.0f };
    
    // Persistent buffers for each frame in flight
    static constexpr size_t MAX_PARTICLE_BUFFER_SIZE = 1024 * 1024;  // 1MB for particle data
    
    rhi::ResourceHandle uniform_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{ rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle particle_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{ rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle count_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{ rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE };
    rhi::ResourceHandle index_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{ rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE, rhi::handles::INVALID_RESOURCE };
    void* uniform_buffer_mapped_[rhi::MAX_FRAMES_IN_FLIGHT]{ nullptr, nullptr, nullptr };
    
    // CPU-side sorting data
    utl::vector<u32> sorted_indices_;
    bool enable_sorting_{ true };
    
    bool create_pipelines();
    bool create_descriptor_sets();
    bool create_buffers();
    void create_default_texture();
    void update_descriptor_set(u32 frame_index);
};

} // namespace primal::graphics

#else

namespace primal::graphics {

// Stub types used when DISABLE_PARTICLE_SYSTEM is defined.
namespace particles {
enum class blend_mode : u8 {};
}

class ParticlePass {
public:
    ParticlePass() = default;
    ~ParticlePass() = default;

    bool initialize(rhi::RHIDeviceBase*) { return true; }
    void shutdown() {}

    void execute(rhi::RHICommandBuffer*, u32, const math::m4x4&, const math::m4x4&) {}
    void set_blend_mode(particles::blend_mode) {}
    void set_depth_write_enabled(bool) {}
};

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
