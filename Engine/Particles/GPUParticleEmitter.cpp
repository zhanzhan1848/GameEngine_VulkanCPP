#include "GPUParticleEmitter.h"
#include "Graphics/RHI/Core/RHIMath.h"
#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles::gpu {

gpu_emitter::gpu_emitter(gpu_emitter_id id, const emitter_config& config)
    : id_(id)
    , config_(config) {
}

void gpu_emitter::set_config(const emitter_config& config) {
    config_ = config;
}

void gpu_emitter::set_position(const math::v3& position) {
    position_ = position;
}

void gpu_emitter::set_rotation(const math::v3& rotation) {
    rotation_ = rotation;
}

void gpu_emitter::set_scale(const math::v3& scale) {
    scale_ = scale;
}

void gpu_emitter::set_transform(const math::m4x4& transform) {
    // Extract position from transform columns
    position_ = math::v3{ transform.columns[3][0], transform.columns[3][1], transform.columns[3][2] };
    // TODO: Extract rotation and scale if needed
}

math::m4x4 gpu_emitter::get_transform() const {
    // Use rhi::math for matrix creation
    return graphics::rhi::math::CreateTranslationMatrix(position_);
}

void gpu_emitter::set_active(bool active) {
    active_ = active;
}

void gpu_emitter::burst(u32 count) {
    // Queue burst spawn command
    // This will be processed on GPU in next update
    config_.burst_count = count;
    config_.mode = emission_mode::burst;
}

void gpu_emitter::set_spawn_rate(f32 rate) {
    config_.spawn_rate = rate;
}

gpu_emitter_config gpu_emitter::get_gpu_config() const {
    gpu_emitter_config gpu_config{};
    
    // Transform
    gpu_config.transform = get_transform();
    gpu_config.position = position_;
    
    // Emission
    gpu_config.spawn_rate = config_.spawn_rate;
    gpu_config.time_accumulator = time_accumulator_;
    gpu_config.max_particles = config_.max_particles;
    gpu_config.active_particles = stats_.active_particles;
    
    // Lifetime
    gpu_config.lifetime_min = config_.lifetime_min;
    gpu_config.lifetime_max = config_.lifetime_max;
    
    // Velocity
    gpu_config.velocity_min = config_.velocity_min;
    gpu_config.velocity_max = config_.velocity_max;
    
    // Color
    gpu_config.color_start = config_.color_start;
    gpu_config.color_end = config_.color_end;
    
    // Scale
    gpu_config.scale_min = config_.scale_min;
    gpu_config.scale_max = config_.scale_max;
    
    // Forces
    gpu_config.gravity = config_.gravity;
    gpu_config.drag = config_.drag;
    gpu_config.wind = config_.wind;
    
    // Flags
    gpu_config.blend_mode = static_cast<u32>(config_.blending);
    gpu_config.emit_in_local_space = config_.emit_in_local_space ? 1 : 0;
    
    return gpu_config;
}

} // namespace primal::particles::gpu

#endif // !DISABLE_PARTICLE_SYSTEM
