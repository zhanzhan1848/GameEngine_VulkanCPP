#pragma once

#include "Common/CommonHeaders.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles {

// -----------------------------------------------------------------------------
// Constants
// -----------------------------------------------------------------------------

// Maximum number of frames that can be in flight (matches RHI constant)
constexpr u32 max_frames_in_flight{ 3 };

// Default maximum particles per emitter
constexpr u32 default_max_particles{ 10000 };

// Invalid particle/emitter ID
constexpr u32 invalid_id{ u32_invalid_id };

// Particle data size in bytes (for buffer calculations)
constexpr u32 particle_data_size{ 80 };

// Particle Data Structure
// Matches GPU shader layout for direct buffer upload
// Total size: 80 bytes (2.5 cache lines)

struct particle_data {
    math::v4 position;       // xyz = world position, w = age
    math::v4 velocity;       // xyz = velocity vector, w = lifetime (seconds)
    math::v4 color;          // rgba = particle color with alpha
    math::v4 scale_rotation; // xy = scale (width, height), zw = rotation (radians)
    math::v4 uv_params;      // x = atlas_index, y = frame_progress, zw = padding
};

// -----------------------------------------------------------------------------
// Blend Modes
// -----------------------------------------------------------------------------

enum class blend_mode : u8 {
    additive = 0,       // src * src_alpha + dst * 1      (fire, sparks)
    alpha = 1,          // src * src_alpha + dst * (1-src_alpha) (smoke, dust)
    multiply = 2,       // src * dst                       (shadows, dark effects)
    premultiplied = 3   // src + dst * (1-src_alpha)      (pre-multiplied alpha)
};

// -----------------------------------------------------------------------------
// Emission Modes
// -----------------------------------------------------------------------------

enum class emission_mode : u8 {
    continuous = 0,     // Spawn at rate per second
    burst = 1,          // Instant spawn on trigger
    ring_buffer = 2     // Continuous with oldest-particle replacement
};

// -----------------------------------------------------------------------------
// Emitter Configuration
// -----------------------------------------------------------------------------

struct emitter_config {
    // Particle limits
    u32 max_particles{ default_max_particles };
    
    // Emission settings
    emission_mode mode{ emission_mode::continuous };
    f32 spawn_rate{ 100.0f };           // Particles per second (continuous mode)
    u32 burst_count{ 0 };               // Instant spawn count (burst mode)
    bool ring_buffer_mode{ true };      // Overwrite oldest when full
    
    // Lifetime
    f32 lifetime_min{ 1.0f };
    f32 lifetime_max{ 3.0f };
    
    // Initial velocity range (random between min/max)
    math::v3 velocity_min{ 0.0f, 1.0f, 0.0f };
    math::v3 velocity_max{ 0.0f, 5.0f, 0.0f };
    
    // Initial color range (random lerp between start/end)
    math::v4 color_start{ 1.0f, 1.0f, 1.0f, 1.0f };
    math::v4 color_end{ 0.5f, 0.5f, 1.0f, 0.8f };
    
    // Initial scale range
    math::v2 scale_min{ 0.1f, 0.1f };
    math::v2 scale_max{ 1.0f, 1.0f };
    
    // Forces
    math::v3 gravity{ 0.0f, -9.8f, 0.0f };
    math::v3 wind{ 0.0f, 0.0f, 0.0f };
    f32 drag{ 0.0f };                   // Velocity damping (0-1)
    
    // Rendering
    blend_mode blending{ blend_mode::additive };
    bool depth_write{ false };
    bool receive_shadows{ false };
    
    // Texture animation (sprite sheet)
    u32 texture_frames_x{ 1 };
    u32 texture_frames_y{ 1 };
    f32 texture_frame_rate{ 0.0f };     // 0 = no animation
    
    // Texture atlas
    u32 texture_atlas_columns{ 1 };     // Columns in atlas
    u32 texture_atlas_rows{ 1 };        // Rows in atlas
    bool texture_random_frame{ false }; // Random start frame vs sequential
    u32 texture_first_frame{ 0 };       // First frame index
    u32 texture_frame_count{ 1 };       // Number of frames to use

    // Transform binding
    bool emit_in_local_space{ false };  // Apply emitter rotation to velocity
};

// -----------------------------------------------------------------------------
// Emitter ID Type
// -----------------------------------------------------------------------------

using emitter_id = u32;

// -----------------------------------------------------------------------------
// Pool Statistics
// -----------------------------------------------------------------------------

struct pool_stats {
    u32 capacity{ 0 };          // Total pool capacity
    u32 allocated{ 0 };         // Currently allocated particles
    u32 peak_usage{ 0 };        // Peak allocation since creation
    u32 free_count{ 0 };        // Available slots
};

// -----------------------------------------------------------------------------
// Indirect Draw Command (matches GPU layout)
// -----------------------------------------------------------------------------

struct draw_indirect_command {
    u32 vertex_count{ 0 };      // Vertices per instance (4 for quad, 1 for point)
    u32 instance_count{ 0 };    // Number of instances to draw
    u32 first_vertex{ 0 };      // First vertex offset
    u32 first_instance{ 0 };    // First instance offset
};

// -----------------------------------------------------------------------------
// Spawn Request (CPU to GPU)
// -----------------------------------------------------------------------------

struct spawn_request {
    math::v3 position;
    math::v3 velocity;
    math::v4 color;
    math::v2 scale;
    f32 lifetime;
    f32 rotation;
};

// -----------------------------------------------------------------------------
// Component Types (for ECS integration)
// -----------------------------------------------------------------------------

using particle_component_id = id::id_type;

struct particle_component_init_info {
    emitter_config config;
    u32 material_id{ u32_invalid_id };
    bool auto_activate{ true };
};

struct particle_component_cache {
    emitter_id emitter{ invalid_id };
    u32 flags{ 0 };
    f32 spawn_rate{ 0.0f };
    math::v3 position_offset;
};

// Component cache update flags
namespace component_flags {
    enum flags : u32 {
        spawn_rate = 0x01,
        position = 0x02,
        active = 0x04,
        burst = 0x08,
        all = spawn_rate | position | active | burst
    };
}

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
