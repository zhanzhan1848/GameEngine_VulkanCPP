#pragma once

#include "Particles/GPUParticleTypes.h"
#include "Particles/ParticleTypes.h"
#include "Graphics/RHI/Core/RHITypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles::gpu {

// -----------------------------------------------------------------------------
// GPU Particle Emitter
// Represents a single particle emitter on the GPU
// -----------------------------------------------------------------------------

class gpu_emitter {
public:
    gpu_emitter(gpu_emitter_id id, const emitter_config& config);
    ~gpu_emitter() = default;
    
    // Non-copyable
    gpu_emitter(const gpu_emitter&) = delete;
    gpu_emitter& operator=(const gpu_emitter&) = delete;
    
    // Configuration
    void set_config(const emitter_config& config);
    const emitter_config& get_config() const { return config_; }
    
    // Transform
    void set_position(const math::v3& position);
    void set_rotation(const math::v3& rotation);
    void set_scale(const math::v3& scale);
    void set_transform(const math::m4x4& transform);
    
    math::v3 get_position() const { return position_; }
    math::m4x4 get_transform() const;
    
    // Active state
    void set_active(bool active);
    bool is_active() const { return active_; }
    
    // Spawn control
    void burst(u32 count);
    void set_spawn_rate(f32 rate);
    f32 get_spawn_rate() const { return config_.spawn_rate; }
    
    // Stats (updated by GPU readback)
    u32 get_active_count() const { return stats_.active_particles; }
    u32 get_visible_count() const { return stats_.visible_particles; }
    
    void update_stats(const gpu_emitter_stats& stats) { stats_ = stats; }
    
    // ID
    gpu_emitter_id get_id() const { return id_; }
    
    // Get GPU-ready config
    gpu_emitter_config get_gpu_config() const;
    
    // Time accumulator for spawning
    f32 get_time_accumulator() const { return time_accumulator_; }
    void add_time(f32 dt) { time_accumulator_ += dt; }
    void reset_time_accumulator() { time_accumulator_ = 0.0f; }
    
private:
    gpu_emitter_id id_;
    emitter_config config_;
    
    // Transform
    math::v3 position_{ 0.0f, 0.0f, 0.0f };
    math::v3 rotation_{ 0.0f, 0.0f, 0.0f };
    math::v3 scale_{ 1.0f, 1.0f, 1.0f };
    
    // State
    bool active_{ true };
    f32 time_accumulator_{ 0.0f };
    
    // Statistics (from GPU readback)
    gpu_emitter_stats stats_{};
};

} // namespace primal::particles::gpu

#else

// Stub
namespace primal::particles::gpu {

class gpu_emitter {
public:
    gpu_emitter(gpu_emitter_id, const emitter_config&) {}
    void set_position(const math::v3&) {}
    void set_active(bool) {}
    bool is_active() const { return false; }
    gpu_emitter_id get_id() const { return 0; }
};

} // namespace primal::particles::gpu

#endif // !DISABLE_PARTICLE_SYSTEM
