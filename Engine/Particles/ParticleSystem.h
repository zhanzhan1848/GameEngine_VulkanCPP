#pragma once

#include "ParticleTypes.h"
#include "ParticlePool.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles {

class particle_emitter;

bool initialize();
void shutdown();

emitter_id create_emitter(const emitter_config& config);
void destroy_emitter(emitter_id id);

particle_emitter* get_emitter(emitter_id id);
const particle_emitter* get_emitter_const(emitter_id id);

void update(f32 delta_time);
void render(u32 frame_index);

u32 get_active_particle_count();
u32 get_emitter_count();
pool_stats get_pool_stats();

void set_frame_index(u32 frame_index);
u32 get_frame_index();

particle_pool& get_frame_pool(u32 frame_index);

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
