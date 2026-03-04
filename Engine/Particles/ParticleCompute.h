#pragma once

#include "Particles/ParticleTypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles {

bool initialize_compute();
void shutdown_compute();

void dispatch_particle_update(u32 frame_index, f32 delta_time);
void dispatch_particle_culling(u32 frame_index, const math::m4x4& view_projection);

void* get_particle_ssbo(u32 frame_index);
void* get_visible_indices_ssbo(u32 frame_index);
void* get_indirect_command_buffer(u32 frame_index);

u32 get_particle_buffer_size();
bool has_async_compute();

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
