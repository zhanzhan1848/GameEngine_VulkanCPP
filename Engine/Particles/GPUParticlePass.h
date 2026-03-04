#pragma once

#include "Particles/GPUParticleTypes.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles::gpu {

// Import RHI namespace for convenience
namespace rhi = primal::graphics::rhi;

// -----------------------------------------------------------------------------
// GPU Particle Render Pass
// Renders particles using GPU-computed data with indirect drawing
// -----------------------------------------------------------------------------

class gpu_particle_pass {
public:
    gpu_particle_pass() = default;
    ~gpu_particle_pass() = default;
    
    // Non-copyable
    gpu_particle_pass(const gpu_particle_pass&) = delete;
    gpu_particle_pass& operator=(const gpu_particle_pass&) = delete;
    
    // Initialize/shutdown
    bool initialize(rhi::RHIDeviceBase* device);
    void shutdown();
    
    // Execute rendering
    void execute(rhi::RHICommandBuffer* cmd_buffer,
                 u32 frame_index,
                 const math::m4x4& view_matrix,
                 const math::m4x4& projection_matrix,
                 rhi::ResourceHandle particle_buffer,
                 rhi::ResourceHandle indirect_buffer,
                 u32 max_particles);
    
    // Configuration
    void set_blend_mode(blend_mode mode);
    void set_depth_write_enabled(bool enabled);
    void set_depth_test_enabled(bool enabled);
    
    rhi::PipelineHandle get_current_pipeline() const;
    
private:
    bool create_shaders();
    bool create_pipelines();
    
    rhi::RHIDeviceBase* device_{ nullptr };
    
    // Shaders
    rhi::ShaderHandle vertex_shader_{ rhi::handles::INVALID_SHADER };
    rhi::ShaderHandle fragment_shader_{ rhi::handles::INVALID_SHADER };
    
    // Pipelines
    rhi::PipelineLayoutHandle pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    rhi::PipelineHandle pipelines_[4]{ 
        rhi::handles::INVALID_PIPELINE,
        rhi::handles::INVALID_PIPELINE,
        rhi::handles::INVALID_PIPELINE,
        rhi::handles::INVALID_PIPELINE
    };
    
    // Descriptor sets
    rhi::DescriptorSetLayoutHandle descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle descriptor_sets_[rhi::MAX_FRAMES_IN_FLIGHT]{ 
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET,
        rhi::handles::INVALID_DESCRIPTOR_SET
    };
    
    // Persistent uniform buffers per frame
    rhi::ResourceHandle uniform_buffers_[rhi::MAX_FRAMES_IN_FLIGHT]{ 
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE,
        rhi::handles::INVALID_RESOURCE
    };
    void* uniform_mapped_[rhi::MAX_FRAMES_IN_FLIGHT]{ nullptr, nullptr, nullptr };
    
    // Settings
    blend_mode current_blend_mode_{ blend_mode::additive };
    bool depth_write_enabled_{ false };
    bool depth_test_enabled_{ true };
};

} // namespace primal::particles::gpu

#else

// Stub
namespace primal::particles::gpu {

class gpu_particle_pass {
public:
    bool initialize(rhi::RHIDeviceBase*) { return true; }
    void shutdown() {}
    void execute(rhi::RHICommandBuffer*, u32, const math::m4x4&, const math::m4x4&,
                 rhi::ResourceHandle, rhi::ResourceHandle, u32) {}
};

} // namespace primal::particles::gpu

#endif // !DISABLE_PARTICLE_SYSTEM
