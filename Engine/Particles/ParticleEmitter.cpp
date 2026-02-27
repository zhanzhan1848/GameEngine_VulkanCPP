#include "ParticleEmitter.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <cmath>
#include <algorithm>

namespace primal::particles {

particle_emitter::particle_emitter(const emitter_config& config, emitter_id id)
    : _config(config)
    , _id(id)
    , _rng(std::random_device{}())
{
}

void particle_emitter::update(f32 delta_time, particle_pool& pool) {
    if (!_is_active) {
        return;
    }
    
    switch (_config.mode) {
        case emission_mode::continuous:
        case emission_mode::ring_buffer: {
            _spawn_accumulator += delta_time * _config.spawn_rate;
            
            while (_spawn_accumulator >= 1.0f) {
                spawn_particle(pool);
                _spawn_accumulator -= 1.0f;
            }
            break;
        }
        case emission_mode::burst:
            break;
    }
}

void particle_emitter::burst(u32 count, particle_pool& pool) {
    if (!_is_active || count == 0) {
        return;
    }
    
    u32 available = _config.max_particles - _active_count;
    u32 to_spawn = std::min(count, available);
    
    for (u32 i = 0; i < to_spawn; ++i) {
        spawn_particle(pool);
    }
}

void particle_emitter::set_config(const emitter_config& config) {
    _config = config;
}

void particle_emitter::set_active(bool active) {
    _is_active = active;
}

void particle_emitter::set_texture(primal::graphics::rhi::ResourceHandle texture) {
    _texture = texture;
}

void particle_emitter::set_position(const math::v3& position) {
    _position = position;
}

void particle_emitter::set_rotation(const math::v4& rotation) {
    _rotation = rotation;
}

void particle_emitter::kill_all() {
    _active_count = 0;
    _spawn_accumulator = 0.0f;
}

u32 particle_emitter::spawn_particle(particle_pool& pool) {
    if (pool.is_full()) {
        if (_config.ring_buffer_mode) {
            u32 oldest = 0;
            pool.free(oldest);
        } else {
            return invalid_id;
        }
    }
    
    u32 index = pool.allocate();
    if (index == invalid_id) {
        return invalid_id;
    }
    
    particle_data& p = pool.get(index);
    initialize_particle(p);
    
    _active_count++;
    return index;
}

void particle_emitter::initialize_particle(particle_data& p) {
    p.position.x = _position.x;
    p.position.y = _position.y;
    p.position.z = _position.z;
    p.position.w = 0.0f;
    
    f32 lifetime = random_range(_config.lifetime_min, _config.lifetime_max);
    p.velocity.w = lifetime;
    
    f32 vx = random_range(_config.velocity_min.x, _config.velocity_max.x);
    f32 vy = random_range(_config.velocity_min.y, _config.velocity_max.y);
    f32 vz = random_range(_config.velocity_min.z, _config.velocity_max.z);
    p.velocity.x = vx;
    p.velocity.y = vy;
    p.velocity.z = vz;
    
    f32 t = random_range(0.0f, 1.0f);
    p.color.x = _config.color_start.x + t * (_config.color_end.x - _config.color_start.x);
    p.color.y = _config.color_start.y + t * (_config.color_end.y - _config.color_start.y);
    p.color.z = _config.color_start.z + t * (_config.color_end.z - _config.color_start.z);
    p.color.w = _config.color_start.w + t * (_config.color_end.w - _config.color_start.w);
    
    f32 sx = random_range(_config.scale_min.x, _config.scale_max.x);
    f32 sy = random_range(_config.scale_min.y, _config.scale_max.y);
    p.scale_rotation.x = sx;
    p.scale_rotation.y = sy;
    p.scale_rotation.z = 0.0f;
    p.scale_rotation.w = 0.0f;
}

f32 particle_emitter::random_range(f32 min, f32 max) {
    return min + (max - min) * _dist(_rng);
}

math::v3 particle_emitter::random_direction() {
    f32 theta = random_range(0.0f, 2.0f * 3.14159265f);
    f32 phi = random_range(0.0f, 3.14159265f);
    
    return math::v3{
        std::sin(phi) * std::cos(theta),
        std::sin(phi) * std::sin(theta),
        std::cos(phi)
    };
}

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
