#include "ParticleCompute.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include "ParticleSystem.h"

namespace primal::particles {

namespace {

struct compute_state {
    bool initialized{ false };
    bool async_compute_available{ false };
    u32 particle_buffer_size{ 0 };
};

compute_state* g_compute{ nullptr };

} // anonymous namespace

bool initialize_compute() {
    if (g_compute) {
        return true;
    }
    
    g_compute = new compute_state();
    g_compute->particle_buffer_size = default_max_particles * sizeof(particle_data);
    g_compute->initialized = true;
    
    return true;
}

void shutdown_compute() {
    if (!g_compute) {
        return;
    }
    
    g_compute->initialized = false;
    delete g_compute;
    g_compute = nullptr;
}

void dispatch_particle_update(u32 frame_index, f32 delta_time) {
    if (!g_compute || !g_compute->initialized) {
        return;
    }
    
    // TODO: Dispatch compute shader for particle simulation
    // 1. Bind particle SSBOs (input/output ping-pong)
    // 2. Set push constants (delta_time, gravity, wind, drag)
    // 3. Dispatch with (max_particles + 255) / 256 workgroups
    // 4. Memory barrier before culling
}

void dispatch_particle_culling(u32 frame_index, const math::m4x4& view_projection) {
    if (!g_compute || !g_compute->initialized) {
        return;
    }
    
    // TODO: Dispatch compute shader for frustum culling
    // 1. Reset indirect draw command instance_count to 0
    // 2. Bind particle SSBO (read), visible indices SSBO (write), indirect command SSBO
    // 3. Set push constants (view_projection, frustum planes)
    // 4. Dispatch with (max_particles + 255) / 256 workgroups
    // 5. Memory barrier before rendering
}

void* get_particle_ssbo(u32 frame_index) {
    if (!g_compute || !g_compute->initialized) {
        return nullptr;
    }
    
    // TODO: Return the Vulkan buffer handle for the current frame's particle SSBO
    return nullptr;
}

void* get_visible_indices_ssbo(u32 frame_index) {
    if (!g_compute || !g_compute->initialized) {
        return nullptr;
    }
    
    // TODO: Return the Vulkan buffer handle for visible particle indices
    return nullptr;
}

void* get_indirect_command_buffer(u32 frame_index) {
    if (!g_compute || !g_compute->initialized) {
        return nullptr;
    }
    
    // TODO: Return the Vulkan buffer handle for indirect draw command
    return nullptr;
}

u32 get_particle_buffer_size() {
    if (!g_compute) {
        return 0;
    }
    
    return g_compute->particle_buffer_size;
}

bool has_async_compute() {
    if (!g_compute) {
        return false;
    }
    
    return g_compute->async_compute_available;
}

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
