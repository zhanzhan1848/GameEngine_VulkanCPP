#pragma once

#include "ParticleTypes.h"
#include "ParticlePool.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include "Common/Id.h"
#include <random>

namespace primal::particles {

class particle_emitter {
public:
    explicit particle_emitter(const emitter_config& config, emitter_id id);
    ~particle_emitter() = default;
    
    DISABLE_COPY(particle_emitter);
    DISABLE_MOVE(particle_emitter);
    
    void update(f32 delta_time, particle_pool& pool);
    void burst(u32 count, particle_pool& pool);

    emitter_id get_id() const { return _id; }
    const emitter_config& get_config() const { return _config; }
    
    void set_config(const emitter_config& config);
    void set_active(bool active);
    bool is_active() const { return _is_active; }
    
    void set_position(const math::v3& position);
    const math::v3& get_position() const { return _position; }
    
    void set_rotation(const math::v4& rotation);
    const math::v4& get_rotation() const { return _rotation; }
    
    u32 get_active_count() const { return _active_count; }
    
    void kill_all();
    
private:
    u32 spawn_particle(particle_pool& pool);
    void initialize_particle(particle_data& p);
    f32 random_range(f32 min, f32 max);
    math::v3 random_direction();
    
    emitter_config _config;
    emitter_id _id;
    
    math::v3 _position{ 0.0f, 0.0f, 0.0f };
    math::v4 _rotation{ 0.0f, 0.0f, 0.0f, 1.0f };
    
    f32 _spawn_accumulator{ 0.0f };
    u32 _active_count{ 0 };
    bool _is_active{ true };
    
    std::mt19937 _rng;
    std::uniform_real_distribution<f32> _dist{ 0.0f, 1.0f };
};

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
