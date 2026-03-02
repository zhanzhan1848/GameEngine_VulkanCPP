#pragma once

#include "Particles/GPUParticleTypes.h"
#include "Graphics/RHI/Core/RHIDevice.h"
#include "Graphics/RHI/Core/RHICommand.h"
#include "Graphics/RHI/Core/RHITypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <vector>
#include <memory>

namespace primal::particles::gpu {

// Import RHI namespace for convenience
namespace rhi = primal::graphics::rhi;

class gpu_emitter;

// -----------------------------------------------------------------------------
// GPU Particle System
// Manages GPU buffers and dispatches compute shaders
// -----------------------------------------------------------------------------

class gpu_particle_system {
public:
    gpu_particle_system() = default;
    ~gpu_particle_system() = default;
    
    // Non-copyable
    gpu_particle_system(const gpu_particle_system&) = delete;
    gpu_particle_system& operator=(const gpu_particle_system&) = delete;
    
    // Initialize/shutdown
    bool initialize(rhi::RHIDeviceBase* device, const gpu_system_config& config = {});
    void shutdown();
    
    // Emitter management
    gpu_emitter_id create_emitter(const emitter_config& config);
    void destroy_emitter(gpu_emitter_id id);
    gpu_emitter* get_emitter(gpu_emitter_id id);
    const gpu_emitter* get_emitter_const(gpu_emitter_id id) const;
    
    // Per-frame operations
    void update(f32 delta_time, u32 frame_index);
    void dispatch_update(rhi::RHICommandBuffer* cmd_buffer, u32 frame_index, f32 delta_time);
    void dispatch_culling(rhi::RHICommandBuffer* cmd_buffer, u32 frame_index, const math::m4x4& view_projection);
    
    // Rendering
    void prepare_draw(u32 frame_index);
    
    // Getters
    rhi::ResourceHandle get_particle_buffer(u32 frame_index) const;
    rhi::ResourceHandle get_visible_buffer(u32 frame_index) const;
    rhi::ResourceHandle get_indirect_buffer(u32 frame_index) const;
    rhi::ResourceHandle get_emitter_buffer(u32 frame_index) const;
    
    rhi::PipelineHandle get_update_pipeline() const { return update_pipeline_; }
    rhi::PipelineHandle get_culling_pipeline() const { return culling_pipeline_; }
    rhi::PipelineLayoutHandle get_pipeline_layout() const { return pipeline_layout_; }
    
    u32 get_max_particles() const { return config_.max_particles_per_emitter; }
    u32 get_emitter_count() const { return static_cast<u32>(emitters_.size()); }
    bool is_initialized() const { return initialized_; }
    
private:
    bool create_buffers();
    bool create_pipelines();
    bool create_shaders();
    
    void destroy_buffers();
    void destroy_pipelines();
    
    // Device reference
    rhi::RHIDeviceBase* device_{ nullptr };
    bool initialized_{ false };
    
    // Configuration
    gpu_system_config config_;
    
    // Emitters
    std::vector<std::unique_ptr<gpu_emitter>> emitters_;
    std::vector<u32> free_emitter_indices_;
    
    // Per-frame GPU buffers (double/triple buffered)
    static constexpr u32 BUFFER_COUNT = rhi::MAX_FRAMES_IN_FLIGHT;
    
    // Particle data buffers (ping-pong for update)
    rhi::ResourceHandle particle_buffers_[BUFFER_COUNT]{ 
        rhi::handles::INVALID_RESOURCE, 
        rhi::handles::INVALID_RESOURCE, 
        rhi::handles::INVALID_RESOURCE 
    };
    
    // Visible particle indices buffer
    rhi::ResourceHandle visible_buffers_[BUFFER_COUNT]{ 
        rhi::handles::INVALID_RESOURCE, 
        rhi::handles::INVALID_RESOURCE, 
        rhi::handles::INVALID_RESOURCE 
    };
    
    // Indirect draw command buffer
    rhi::ResourceHandle indirect_buffers_[BUFFER_COUNT]{ 
        rhi::handles::INVALID_RESOURCE, 
        rhi::handles::INVALID_RESOURCE, 
        rhi::handles::INVALID_RESOURCE 
    };
    
    // Emitter config uniform buffer
    rhi::ResourceHandle emitter_buffers_[BUFFER_COUNT]{ 
        rhi::handles::INVALID_RESOURCE, 
        rhi::handles::INVALID_RESOURCE, 
        rhi::handles::INVALID_RESOURCE 
    };
    
    // Counter buffers (alive count, dead count, etc.)
    rhi::ResourceHandle counter_buffers_[BUFFER_COUNT]{ 
        rhi::handles::INVALID_RESOURCE, 
        rhi::handles::INVALID_RESOURCE, 
        rhi::handles::INVALID_RESOURCE 
    };
    
    // Compute shaders
    rhi::ShaderHandle update_shader_{ rhi::handles::INVALID_SHADER };
    rhi::ShaderHandle culling_shader_{ rhi::handles::INVALID_SHADER };
    
    // Compute pipelines
    rhi::PipelineHandle update_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineHandle culling_pipeline_{ rhi::handles::INVALID_PIPELINE };
    rhi::PipelineLayoutHandle pipeline_layout_{ rhi::handles::INVALID_PIPELINE_LAYOUT };
    
    // Descriptor sets
    rhi::DescriptorSetLayoutHandle descriptor_layout_{ rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT };
    rhi::DescriptorSetHandle descriptor_sets_[BUFFER_COUNT]{ 
        rhi::handles::INVALID_DESCRIPTOR_SET, 
        rhi::handles::INVALID_DESCRIPTOR_SET, 
        rhi::handles::INVALID_DESCRIPTOR_SET 
    };
};

// -----------------------------------------------------------------------------
// Global GPU Particle System Functions
// -----------------------------------------------------------------------------

namespace system {

bool initialize(rhi::RHIDeviceBase* device, const gpu_system_config& config = {});
void shutdown();

gpu_particle_system* get();
bool is_initialized();

} // namespace system

} // namespace primal::particles::gpu

#else

// Stub implementation when particle system is disabled
namespace primal::particles::gpu {

class gpu_particle_system {
public:
    bool initialize(rhi::RHIDeviceBase*, const gpu_system_config& = {}) { return true; }
    void shutdown() {}
};

namespace system {
    inline bool initialize(rhi::RHIDeviceBase*, const gpu_system_config& = {}) { return true; }
    inline void shutdown() {}
    inline gpu_particle_system* get() { return nullptr; }
    inline bool is_initialized() { return false; }
}

} // namespace primal::particles::gpu

#endif // !DISABLE_PARTICLE_SYSTEM
