#pragma once

#include "Common/CommonHeaders.h"
#include "ParticleTypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles::gpu {

// -----------------------------------------------------------------------------
// GPU Particle Constants
// -----------------------------------------------------------------------------

// Workgroup size for compute shaders
constexpr u32 WORKGROUP_SIZE = 256;

// Maximum GPU particles per emitter
constexpr u32 DEFAULT_MAX_GPU_PARTICLES = 100000;

// Indirect draw command buffer size (matches Metal draw_indirect_args)
constexpr u32 DRAW_INDIRECT_SIZE = 16; // 4 x uint32

// -----------------------------------------------------------------------------
// GPU Emitter Configuration (uploaded to GPU as uniform buffer)
// Total size: 256 bytes (aligned for GPU)
// -----------------------------------------------------------------------------

struct gpu_emitter_config {
    // Transform (64 bytes)
    math::m4x4 transform;           // Emitter world transform
    math::v3 position;              // Emitter position
    f32 _pad0;
    
    // Emission settings (32 bytes)
    f32 spawn_rate;                 // Particles per second
    f32 time_accumulator;           // Accumulated time for spawning
    u32 max_particles;              // Maximum particles
    u32 active_particles;           // Current active count (GPU writes)
    
    // Lifetime range (16 bytes)
    f32 lifetime_min;
    f32 lifetime_max;
    f32 _pad1[2];
    
    // Velocity range (48 bytes)
    math::v3 velocity_min;
    f32 _pad2;
    math::v3 velocity_max;
    f32 _pad3;
    
    // Color range (32 bytes)
    math::v4 color_start;
    math::v4 color_end;
    
    // Scale range (16 bytes)
    math::v2 scale_min;
    math::v2 scale_max;
    
    // Forces (48 bytes)
    math::v3 gravity;
    f32 drag;
    math::v3 wind;
    f32 _pad4;
    
    // Flags (16 bytes)
    u32 blend_mode;                 // blend_mode enum
    u32 emit_in_local_space;
    u32 _pad5[2];
};

// -----------------------------------------------------------------------------
// GPU Particle Data (same as CPU for compatibility)
// Total size: 64 bytes
// -----------------------------------------------------------------------------

using gpu_particle_data = particle_data; // Reuse CPU structure

// -----------------------------------------------------------------------------
// GPU Indirect Draw Command (matches Metal MTLDrawPrimitivesIndirectArguments)
// -----------------------------------------------------------------------------

struct gpu_draw_indirect_command {
    u32 vertex_count;       // Number of vertices per instance
    u32 instance_count;     // Number of instances
    u32 first_vertex;       // First vertex offset
    u32 first_instance;     // First instance offset
};

// -----------------------------------------------------------------------------
// GPU Culling Result (per particle)
// -----------------------------------------------------------------------------

struct gpu_culling_result {
    u32 visible_count;      // Total visible particles
    u32 _pad[3];            // Padding to 16 bytes
};

// -----------------------------------------------------------------------------
// GPU Spawn Command (CPU -> GPU)
// -----------------------------------------------------------------------------

struct gpu_spawn_command {
    math::v3 position;
    f32 lifetime;
    math::v3 velocity;
    f32 _pad0;
    math::v4 color;
    math::v2 scale;
    math::v2 _pad1;
};

// -----------------------------------------------------------------------------
// GPU Emitter Statistics (GPU -> CPU readback)
// -----------------------------------------------------------------------------

struct gpu_emitter_stats {
    u32 active_particles;
    u32 visible_particles;
    u32 spawn_count;
    u32 cull_count;
};

// -----------------------------------------------------------------------------
// GPU Particle System Configuration
// -----------------------------------------------------------------------------

struct gpu_system_config {
    u32 max_particles_per_emitter{ DEFAULT_MAX_GPU_PARTICLES };
    u32 max_emitters{ 64 };
    bool async_compute{ true };
    bool indirect_drawing{ true };
};

// -----------------------------------------------------------------------------
// GPU Emitter ID Type
// -----------------------------------------------------------------------------

using gpu_emitter_id = u32;
constexpr gpu_emitter_id INVALID_GPU_EMITTER_ID = u32_invalid_id;

} // namespace primal::particles::gpu

#endif // !DISABLE_PARTICLE_SYSTEM
