#include "MetalParticle.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include "MetalCore.h"
#include "MetalResource.h"
#include "Particles/ParticleSystem.h"

namespace primal::graphics::metal::particle {

namespace {

constexpr u32 max_particles = 100000;
constexpr u32 frame_count = 3;

struct particle_state {
    MTL::Buffer* particle_buffers[frame_count]{ nullptr };
    MTL::Buffer* visible_indices_buffer{ nullptr };
    MTL::Buffer* indirect_command_buffer{ nullptr };
    MTL::Buffer* update_params_buffer{ nullptr };
    MTL::Buffer* culling_params_buffer{ nullptr };
    MTL::Buffer* visible_count_buffer{ nullptr };
    
    MTL::ComputePipelineState* update_pipeline{ nullptr };
    MTL::ComputePipelineState* culling_pipeline{ nullptr };
    MTL::ComputePipelineState* culling_init_pipeline{ nullptr };
    MTL::RenderPipelineState* render_pipeline_additive{ nullptr };
    MTL::RenderPipelineState* render_pipeline_alpha{ nullptr };
    MTL::RenderPipelineState* render_pipeline_multiply{ nullptr };
    
    MTL::Texture* particle_texture{ nullptr };
    MTL::SamplerState* sampler{ nullptr };
    MTL::DepthStencilState* depth_state{ nullptr };
    
    math::u32v2 size{ 0, 0 };
    u32 current_frame{ 0 };
    particles::blend_mode blend_mode{ particles::blend_mode::additive };
    bool depth_write_enabled{ false };
    bool initialized{ false };
};

particle_state* g_state{ nullptr };

}

bool initialize() {
    if (g_state) {
        return true;
    }
    
    g_state = new particle_state();
    
    MTL::Device* device = core::get_device();
    if (!device) {
        return false;
    }
    
    NS::Error* error = nullptr;
    
    // Create particle buffers (double-buffered)
    for (u32 i = 0; i < frame_count; ++i) {
        g_state->particle_buffers[i] = device->newBuffer(
            max_particles * sizeof(metal_particle_data),
            MTL::ResourceStorageModeShared);
        NAME_METAL_OBJECT_INDEXED(g_state->particle_buffers[i], i, "ParticleBuffer");
    }
    
    // Create visible indices buffer
    g_state->visible_indices_buffer = device->newBuffer(
        max_particles * sizeof(u32),
        MTL::ResourceStorageModeShared);
    NAME_METAL_OBJECT(g_state->visible_indices_buffer, "VisibleIndicesBuffer");
    
    // Create indirect command buffer
    g_state->indirect_command_buffer = device->newBuffer(
        sizeof(MTL::DrawPrimitivesIndirectArguments),
        MTL::ResourceStorageModeShared);
    NAME_METAL_OBJECT(g_state->indirect_command_buffer, "IndirectCommandBuffer");
    
    // Create visible count buffer
    g_state->visible_count_buffer = device->newBuffer(
        sizeof(u32),
        MTL::ResourceStorageModeShared);
    NAME_METAL_OBJECT(g_state->visible_count_buffer, "VisibleCountBuffer");
    
    // Create parameter buffers
    g_state->update_params_buffer = device->newBuffer(
        sizeof(particles::particle_data) * 2,
        MTL::ResourceStorageModeShared);
    g_state->culling_params_buffer = device->newBuffer(
        sizeof(math::m4x4) + sizeof(u32) * 4,
        MTL::ResourceStorageModeShared);
    
    // Create sampler
    MTL::SamplerDescriptor* sampler_desc = MTL::SamplerDescriptor::alloc()->init();
    sampler_desc->setMinFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
    sampler_desc->setMagFilter(MTL::SamplerMinMagFilter::SamplerMinMagFilterLinear);
    sampler_desc->setSAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
    sampler_desc->setTAddressMode(MTL::SamplerAddressMode::SamplerAddressModeClampToEdge);
    g_state->sampler = device->newSamplerState(sampler_desc);
    sampler_desc->release();
    
    // Create depth stencil state
    MTL::DepthStencilDescriptor* depth_desc = MTL::DepthStencilDescriptor::alloc()->init();
    depth_desc->setDepthCompareFunction(MTL::CompareFunctionLessEqual);
    depth_desc->setDepthWriteEnabled(false);
    g_state->depth_state = device->newDepthStencilState(depth_desc);
    depth_desc->release();
    
    // TODO: Load shaders and create pipeline states
    // This requires shader compilation from .metal files
    
    g_state->initialized = true;
    return true;
}

void shutdown() {
    if (!g_state) {
        return;
    }
    
    for (u32 i = 0; i < frame_count; ++i) {
        core::release(g_state->particle_buffers[i]);
    }
    
    core::release(g_state->visible_indices_buffer);
    core::release(g_state->indirect_command_buffer);
    core::release(g_state->update_params_buffer);
    core::release(g_state->culling_params_buffer);
    core::release(g_state->visible_count_buffer);
    core::release(g_state->update_pipeline);
    core::release(g_state->culling_pipeline);
    core::release(g_state->culling_init_pipeline);
    core::release(g_state->render_pipeline_additive);
    core::release(g_state->render_pipeline_alpha);
    core::release(g_state->render_pipeline_multiply);
    core::release(g_state->particle_texture);
    core::release(g_state->sampler);
    core::release(g_state->depth_state);
    
    delete g_state;
    g_state = nullptr;
}

void set_size(math::u32v2 size) {
    if (g_state) {
        g_state->size = size;
    }
}

void update_particles(MTL::CommandBuffer* cmd_buffer, f32 delta_time) {
    if (!g_state || !g_state->initialized || !cmd_buffer) {
        return;
    }
    
    if (!g_state->update_pipeline) {
        return;
    }
    
    MTL::ComputeCommandEncoder* encoder = cmd_buffer->computeCommandEncoder();
    if (!encoder) {
        return;
    }
    
    encoder->setComputePipelineState(g_state->update_pipeline);
    
    u32 prev_frame = (g_state->current_frame + frame_count - 1) % frame_count;
    encoder->setBuffer(g_state->particle_buffers[prev_frame], 0, 0);
    encoder->setBuffer(g_state->particle_buffers[g_state->current_frame], 0, 1);
    encoder->setBuffer(g_state->update_params_buffer, 0, 2);
    
    MTL::Size threadgroup_size = MTL::Size::Make(256, 1, 1);
    MTL::Size grid_size = MTL::Size::Make((max_particles + 255) / 256 * 256, 1, 1);
    
    encoder->dispatchThreads(grid_size, threadgroup_size);
    encoder->endEncoding();
}

void cull_particles(MTL::CommandBuffer* cmd_buffer, const math::m4x4& view_projection) {
    if (!g_state || !g_state->initialized || !cmd_buffer) {
        return;
    }
    
    if (!g_state->culling_pipeline) {
        return;
    }
    
    // Initialize visible count to 0
    MTL::BlitCommandEncoder* blit_encoder = cmd_buffer->blitCommandEncoder();
    if (blit_encoder) {
        blit_encoder->fillBuffer(g_state->visible_count_buffer, 
                                  NS::Range::Make(0, sizeof(u32)), 0);
        blit_encoder->endEncoding();
    }
    
    MTL::ComputeCommandEncoder* encoder = cmd_buffer->computeCommandEncoder();
    if (!encoder) {
        return;
    }
    
    encoder->setComputePipelineState(g_state->culling_pipeline);
    
    u32 prev_frame = (g_state->current_frame + frame_count - 1) % frame_count;
    encoder->setBuffer(g_state->particle_buffers[prev_frame], 0, 0);
    encoder->setBuffer(g_state->visible_indices_buffer, 0, 1);
    encoder->setBuffer(g_state->visible_count_buffer, 0, 2);
    encoder->setBuffer(g_state->culling_params_buffer, 0, 3);
    
    MTL::Size threadgroup_size = MTL::Size::Make(256, 1, 1);
    MTL::Size grid_size = MTL::Size::Make((max_particles + 255) / 256 * 256, 1, 1);
    
    encoder->dispatchThreads(grid_size, threadgroup_size);
    encoder->endEncoding();
    
    g_state->current_frame = (g_state->current_frame + 1) % frame_count;
}

void render_particles(MTL::CommandBuffer* cmd_buffer, 
                      const math::m4x4& view_matrix, 
                      const math::m4x4& projection_matrix) {
    if (!g_state || !g_state->initialized || !cmd_buffer) {
        return;
    }
    
    MTL::RenderCommandEncoder* encoder = cmd_buffer->renderCommandEncoder(nullptr);
    if (!encoder) {
        return;
    }
    
    // Select pipeline based on blend mode
    MTL::RenderPipelineState* pipeline = nullptr;
    switch (g_state->blend_mode) {
        case particles::blend_mode::additive:
            pipeline = g_state->render_pipeline_additive;
            break;
        case particles::blend_mode::alpha:
            pipeline = g_state->render_pipeline_alpha;
            break;
        case particles::blend_mode::multiply:
            pipeline = g_state->render_pipeline_multiply;
            break;
        default:
            pipeline = g_state->render_pipeline_additive;
            break;
    }
    
    if (!pipeline) {
        encoder->endEncoding();
        return;
    }
    
    encoder->setRenderPipelineState(pipeline);
    encoder->setDepthStencilState(g_state->depth_state);
    
    encoder->setVertexBuffer(g_state->particle_buffers[g_state->current_frame], 0, 0);
    encoder->setVertexBuffer(g_state->visible_indices_buffer, 0, 1);
    
    if (g_state->particle_texture) {
        encoder->setFragmentTexture(g_state->particle_texture, 0);
        encoder->setFragmentSamplerState(g_state->sampler, 0);
    }
    
    // Draw instanced quads (6 vertices per quad)
    // encoder->drawPrimitives(MTL::PrimitiveTypeTriangle, NS::UInteger(0), NS::UInteger(6), visible_count);
    
    encoder->endEncoding();
}

void set_blend_mode(particles::blend_mode mode) {
    if (g_state) {
        g_state->blend_mode = mode;
    }
}

void set_depth_write_enabled(bool enabled) {
    if (g_state) {
        g_state->depth_write_enabled = enabled;
    }
}

void set_particle_texture(MTL::Texture* texture) {
    if (g_state) {
        g_state->particle_texture = texture;
    }
}

u32 get_visible_particle_count() {
    if (!g_state || !g_state->visible_count_buffer) {
        return 0;
    }
    return *static_cast<u32*>(g_state->visible_count_buffer->contents());
}

u32 get_total_particle_count() {
    return max_particles;
}

MTL::Buffer* get_particle_buffer() {
    if (!g_state) {
        return nullptr;
    }
    return g_state->particle_buffers[g_state->current_frame];
}

MTL::Buffer* get_visible_indices_buffer() {
    if (!g_state) {
        return nullptr;
    }
    return g_state->visible_indices_buffer;
}

MTL::Buffer* get_indirect_command_buffer() {
    if (!g_state) {
        return nullptr;
    }
    return g_state->indirect_command_buffer;
}

}

#endif // !DISABLE_PARTICLE_SYSTEM
