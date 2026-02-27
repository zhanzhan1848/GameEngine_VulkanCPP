#pragma once

#include "MetalCommonHeaders.h"
#include "Particles/ParticleTypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::graphics::metal::particle {

struct metal_particle_data {
    math::v4 position;
    math::v4 velocity;
    math::v4 color;
    math::v4 scale_rotation;
};

bool initialize();
void shutdown();

void set_size(math::u32v2 size);

void update_particles(MTL::CommandBuffer* cmd_buffer, f32 delta_time);
void cull_particles(MTL::CommandBuffer* cmd_buffer, const math::m4x4& view_projection);
void render_particles(MTL::CommandBuffer* cmd_buffer, const math::m4x4& view_matrix, const math::m4x4& projection_matrix);

void set_blend_mode(particles::blend_mode mode);
void set_depth_write_enabled(bool enabled);
void set_particle_texture(MTL::Texture* texture);

u32 get_visible_particle_count();
u32 get_total_particle_count();

MTL::Buffer* get_particle_buffer();
MTL::Buffer* get_visible_indices_buffer();
MTL::Buffer* get_indirect_command_buffer();

}

#endif // !DISABLE_PARTICLE_SYSTEM
