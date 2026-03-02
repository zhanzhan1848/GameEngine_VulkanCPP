#include "Common.h"

struct ParticleData {
    float4 position;      // xyz = position, w = age (0-1)
    float4 velocity;      // xyz = velocity, w = lifetime
    float4 color;         // rgba
    float4 scale_rotation; // xy = scale, zw = rotation
};

struct ParticleUpdateParams {
    float delta_time;
    float time;
    uint max_particles;
    uint frame_index;
    float3 gravity;
    float drag;
    float3 wind;
    float _pad;
};

struct ParticleCullingParams {
    float4x4 view_projection;
    float4 frustum_planes[6];
    uint max_particles;
    uint vertex_count;
};

struct DrawIndirectCommand {
    uint vertex_count;
    uint instance_count;
    uint first_vertex;
    uint first_instance;
};

kernel void particle_update_kernel(
    device const ParticleData* particles_in [[buffer(0)]],
    device ParticleData* particles_out [[buffer(1)]],
    device const ParticleUpdateParams& params [[buffer(2)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint index = global_id.x;
    
    if (index >= params.max_particles) {
        return;
    }
    
    ParticleData p = particles_in[index];
    
    // Check if particle is dead
    if (p.position.w >= 1.0) {
        particles_out[index] = p;
        return;
    }
    
    float lifetime = p.velocity.w;
    float age_increment = params.delta_time / max(lifetime, 0.001);
    
    // Update age
    p.position.w += age_increment;
    
    // Check if particle died this frame
    if (p.position.w >= 1.0) {
        particles_out[index] = p;
        return;
    }
    
    // Apply forces
    float3 acceleration = params.gravity + params.wind;
    p.velocity.xyz += acceleration * params.delta_time;
    
    // Apply drag
    p.velocity.xyz *= (1.0 - params.drag * params.delta_time);
    
    // Integrate position
    p.position.xyz += p.velocity.xyz * params.delta_time;
    
    particles_out[index] = p;
}

kernel void particle_culling_init_kernel(
    device DrawIndirectCommand& cmd [[buffer(0)]],
    device const ParticleCullingParams& params [[buffer(1)]],
    uint3 global_id [[thread_position_in_grid]])
{
    // Initialize indirect draw command
    cmd.vertex_count = params.vertex_count;
    cmd.instance_count = 0;
    cmd.first_vertex = 0;
    cmd.first_instance = 0;
}

kernel void particle_culling_kernel(
    device const ParticleData* particles [[buffer(0)]],
    device uint* visible_indices [[buffer(1)]],
    device atomic_uint& visible_count [[buffer(2)]],
    device const ParticleCullingParams& params [[buffer(3)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint index = global_id.x;
    
    if (index >= params.max_particles) {
        return;
    }
    
    ParticleData p = particles[index];
    
    // Skip dead particles
    if (p.position.w >= 1.0) {
        return;
    }
    
    float3 pos = p.position.xyz;
    float max_scale = max(p.scale_rotation.x, p.scale_rotation.y);
    
    // Simple frustum culling using clip space
    float4 clip_pos = params.view_projection * float4(pos, 1.0);
    
    // Check if behind camera
    if (clip_pos.w <= 0.0) {
        return;
    }
    
    // Perspective divide
    float3 ndc = clip_pos.xyz / clip_pos.w;
    
    // Check NDC bounds with scale-based tolerance
    float tolerance = max_scale * 0.1;
    if (abs(ndc.x) > 1.0 + tolerance || abs(ndc.y) > 1.0 + tolerance) {
        return;
    }
    
    // Check depth
    if (ndc.z < 0.0 || ndc.z > 1.0) {
        return;
    }
    
    // Particle is visible, add to visible list
    uint visible_index = atomic_fetch_add_explicit(&visible_count, 1, memory_order_relaxed);
    
    if (visible_index < params.max_particles) {
        visible_indices[visible_index] = index;
    }
}

// Particle spawn kernel for CPU-initiated spawns
struct SpawnRequest {
    float3 position;
    float3 velocity;
    float4 color;
    float2 scale;
    float lifetime;
    float rotation;
};

kernel void particle_spawn_kernel(
    device ParticleData* particles [[buffer(0)]],
    device const SpawnRequest* spawn_requests [[buffer(1)]],
    device atomic_uint* free_indices [[buffer(2)]],
    device const uint& spawn_count [[buffer(3)]],
    uint3 global_id [[thread_position_in_grid]])
{
    uint index = global_id.x;
    
    if (index >= spawn_count) {
        return;
    }
    
    // Get a free particle slot
    uint particle_idx = atomic_fetch_add_explicit(&free_indices[0], 1, memory_order_relaxed);
    
    // Initialize particle from spawn request
    SpawnRequest req = spawn_requests[index];
    ParticleData p;
    
    p.position = float4(req.position, 0.0);
    p.velocity = float4(req.velocity, req.lifetime);
    p.color = req.color;
    p.scale_rotation = float4(req.scale, req.rotation, 0.0);
    
    particles[particle_idx] = p;
}
