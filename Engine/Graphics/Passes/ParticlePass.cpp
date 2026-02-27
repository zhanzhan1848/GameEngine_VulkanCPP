#include "ParticlePass.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include "Particles/ParticleSystem.h"
#include "Particles/ParticleCompute.h"

namespace primal::graphics {

ParticlePass::ParticlePass() = default;
ParticlePass::~ParticlePass() = default;

bool ParticlePass::initialize(rhi::RHIDeviceBase* device) {
    if (!device) {
        return false;
    }
    
    device_ = device;
    
    // TODO: Load shaders and create pipelines
    // vertex_shader_ = device->create_shader("particle.vert.spv", ...);
    // fragment_shader_ = device->create_shader("particle.frag.spv", ...);
    
    // TODO: Create descriptor set layout
    // TODO: Create pipeline layout with push constants
    // TODO: Create pipelines for each blend mode
    
    return true;
}

void ParticlePass::shutdown() {
    if (!device_) {
        return;
    }
    
    // TODO: Destroy pipelines
    // for (auto& pipeline : pipelines_) {
    //     device_->destroy_pipeline(pipeline);
    // }
    
    // TODO: Destroy pipeline layout, descriptor sets, etc.
    
    device_ = nullptr;
}

void ParticlePass::execute(rhi::RHICommandBuffer* cmd_buffer,
                           u32 frame_index,
                           const math::m4x4& view_matrix,
                           const math::m4x4& projection_matrix) {
    if (!device_ || !cmd_buffer) {
        return;
    }
    
    // TODO: Implement particle rendering
    // 1. Get visible particle count from indirect command buffer
    // 2. Bind pipeline based on current blend mode
    // 3. Bind descriptor sets (particle SSBO, textures)
    // 4. Set push constants (view_projection, time)
    // 5. Execute indirect draw command
    
    // Placeholder: No particles to render yet
    // auto visible_count = particles::get_visible_count(frame_index);
    // if (visible_count == 0) return;
    
    // rhi::PipelineHandle pipeline = pipelines_[static_cast<u32>(current_blend_mode_)];
    // cmd_buffer->bind_pipeline(pipeline);
    
    // ParticlePushConstants pc;
    // pc.view_projection = projection_matrix * view_matrix;
    // pc.time = 0.0f;  // TODO: Get from engine
    // pc.delta_time = 0.016f;
    // pc.vertex_count = 4;  // Instanced quad
    // cmd_buffer->push_constants(pipeline_layout_, &pc, sizeof(pc));
    
    // void* indirect_buffer = particles::get_indirect_command_buffer(frame_index);
    // cmd_buffer->draw_indexed_indirect(indirect_buffer, 0, 1);
}

void ParticlePass::set_blend_mode(particles::blend_mode mode) {
    current_blend_mode_ = mode;
}

void ParticlePass::set_depth_write_enabled(bool enabled) {
    depth_write_enabled_ = enabled;
}

void ParticlePass::set_depth_test_enabled(bool enabled) {
    depth_test_enabled_ = enabled;
}

void ParticlePass::set_texture_sheet(u32 frames_x, u32 frames_y, f32 frame_rate) {
    texture_frames_x_ = frames_x;
    texture_frames_y_ = frames_y;
    texture_frame_rate_ = frame_rate;
}

void ParticlePass::set_particle_texture(rhi::ResourceHandle texture) {
    particle_texture_ = texture;
}

rhi::PipelineHandle ParticlePass::get_current_pipeline() const {
    return pipelines_[static_cast<u32>(current_blend_mode_)];
}

bool ParticlePass::create_pipelines() {
    // Blend mode configurations:
    // 0 = Additive: src * src_alpha + dst * 1
    // 1 = Alpha: src * src_alpha + dst * (1 - src_alpha)
    // 2 = Multiply: src * dst
    // 3 = Premultiplied: src + dst * (1 - src_alpha)
    
    // TODO: Create 4 pipelines with different blend states
    // for (u32 i = 0; i < 4; ++i) {
    //     rhi::BlendState blend = get_blend_state_for_mode(static_cast<particles::blend_mode>(i));
    //     rhi::DepthState depth{ depth_test_enabled_, depth_write_enabled_ };
    //     pipelines_[i] = device_->create_graphics_pipeline(...);
    // }
    
    return true;
}

bool ParticlePass::create_descriptor_sets() {
    // TODO: Create descriptor sets for each frame
    // for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
    //     descriptor_sets_[i] = device_->create_descriptor_set(descriptor_set_layout_);
    // }
    return true;
}

void ParticlePass::update_descriptor_set(u32 frame_index) {
    // TODO: Update descriptor set with current particle SSBO and texture
    // device_->update_descriptor_set(descriptor_sets_[frame_index], ...);
}

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
