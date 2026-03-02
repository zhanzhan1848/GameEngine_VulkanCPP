#include "ParticleEmitter.h"
#include "ParticleCurve.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <cmath>
#include <algorithm>
#include <unordered_map>

namespace primal::particles {

// Per-particle curve state (stored separately from GPU data)
struct particle_curve_state {
    f32 initial_scale_x{ 1.0f };
    f32 initial_scale_y{ 1.0f };
    f32 initial_alpha{ 1.0f };
    math::v3 initial_velocity{ 0.0f, 0.0f, 0.0f };
    f32 initial_velocity_magnitude{ 1.0f };
};

// Static storage for curve states (simple approach for now)
static std::unordered_map<u32, particle_curve_state> g_curve_states;

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
    
    const bool has_curves = _config.curves.has_any_curves();
    const bool has_texture_anim = _config.texture_frame_rate > 0.0f;
    
    // Update existing particles
    for (u32 i = 0; i < pool.capacity(); ++i) {
        const particle_data& p = pool.get(i);
        
        // Check if particle is active (has a valid age < lifetime)
        if (p.position.w >= p.velocity.w - 0.001f) {
            continue; // Particle is dead
        }
        
        particle_data& modified_p = pool.get(i);
        
        // Calculate normalized age [0, 1] for curve evaluation
        const f32 normalized_age = (p.velocity.w > 0.0f) ? (p.position.w / p.velocity.w) : 0.0f;
        
        // Apply texture frame animation
        if (has_texture_anim) {
            f32 frame_progress = modified_p.uv_params.y + delta_time * _config.texture_frame_rate;
            
            if (frame_progress >= 1.0f) {
                u32 current_frame = static_cast<u32>(modified_p.uv_params.x);
                current_frame = (current_frame + 1) % _config.texture_frame_count;
                modified_p.uv_params.x = static_cast<f32>(current_frame);
                frame_progress -= 1.0f;
            }
            modified_p.uv_params.y = frame_progress;
        }
        
        // Apply curve-based property updates
        if (has_curves) {
            auto it = g_curve_states.find(i);
            if (it == g_curve_states.end()) {
                continue; // No curve state for this particle
            }
            
            const particle_curve_state& state = it->second;
            
            // Scale curve
            if (_config.curves.use_scale_curve && _config.curves.scale_curve) {
                const f32 scale_mult = _config.curves.scale_curve->evaluate(normalized_age);
                modified_p.scale_rotation.x = state.initial_scale_x * scale_mult;
                modified_p.scale_rotation.y = state.initial_scale_y * scale_mult;
            }
            
            // Alpha curve
            if (_config.curves.use_alpha_curve && _config.curves.alpha_curve) {
                const f32 alpha_mult = _config.curves.alpha_curve->evaluate(normalized_age);
                modified_p.color.w = state.initial_alpha * alpha_mult;
            }
            
            // Color gradient
            if (_config.curves.use_color_gradient && _config.curves.color_gradient) {
                const math::v4 gradient_color = _config.curves.color_gradient->evaluate(normalized_age);
                modified_p.color.x = gradient_color.x;
                modified_p.color.y = gradient_color.y;
                modified_p.color.z = gradient_color.z;
                // Alpha is controlled by alpha_curve if set, otherwise use gradient alpha
                if (!_config.curves.use_alpha_curve) {
                    modified_p.color.w = gradient_color.w;
                }
            }
            
            // Velocity curve (speed multiplier)
            if (_config.curves.use_velocity_curve && _config.curves.velocity_curve) {
                const f32 speed_mult = _config.curves.velocity_curve->evaluate(normalized_age);
                const f32 base_speed = state.initial_velocity_magnitude;
                
                // Calculate current velocity direction
                const f32 vx = modified_p.velocity.x;
                const f32 vy = modified_p.velocity.y;
                const f32 vz = modified_p.velocity.z;
                const f32 current_speed = std::sqrt(vx*vx + vy*vy + vz*vz);
                
                if (current_speed > 0.001f) {
                    const f32 target_speed = base_speed * speed_mult;
                    const f32 ratio = target_speed / current_speed;
                    modified_p.velocity.x *= ratio;
                    modified_p.velocity.y *= ratio;
                    modified_p.velocity.z *= ratio;
                }
            }
            
            // Rotation curve (rotation speed multiplier)
            if (_config.curves.use_rotation_curve && _config.curves.rotation_curve) {
                const f32 rot_mult = _config.curves.rotation_curve->evaluate(normalized_age);
                // Rotation is stored in scale_rotation.zw, apply as rotation speed
                modified_p.scale_rotation.z *= rot_mult;
                modified_p.scale_rotation.w *= rot_mult;
            }
        }
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
    g_curve_states.clear();
}

u32 particle_emitter::spawn_particle(particle_pool& pool) {
    if (pool.is_full()) {
        if (_config.ring_buffer_mode) {
            u32 oldest = 0;
            pool.free(oldest);
            g_curve_states.erase(oldest);
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
    
    // Store initial values for curve evaluation
    if (_config.curves.has_any_curves()) {
        particle_curve_state state;
        state.initial_scale_x = p.scale_rotation.x;
        state.initial_scale_y = p.scale_rotation.y;
        state.initial_alpha = p.color.w;
        state.initial_velocity = { p.velocity.x, p.velocity.y, p.velocity.z };
        state.initial_velocity_magnitude = std::sqrt(
            p.velocity.x * p.velocity.x +
            p.velocity.y * p.velocity.y +
            p.velocity.z * p.velocity.z
        );
        g_curve_states[index] = state;
    }
    
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
    
    f32 sx = random_range(_config.scale_min.x, _config.scale_max.x) * _config.scale_multiplier;
    f32 sy = random_range(_config.scale_min.y, _config.scale_max.y) * _config.scale_multiplier;
    p.scale_rotation.x = sx;
    p.scale_rotation.y = sy;
    p.scale_rotation.z = 0.0f;
    p.scale_rotation.w = 0.0f;
    
    // Calculate texture frame
    u32 frame_index = 0;
    if (_config.texture_random_frame) {
        frame_index = static_cast<u32>(_rng() % _config.texture_frame_count);
    } else {
        frame_index = _config.texture_first_frame;
    }
    
    p.uv_params.x = static_cast<f32>(frame_index);
    p.uv_params.y = 0.0f; // Frame progress for animation
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
