#include "ParticlePass.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include "Particles/ParticleSystem.h"
#include "Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"

#include <cstring>
#include <fstream>
#include <vector>
#include <iostream>

namespace primal::graphics {

ParticlePass::ParticlePass() = default;
ParticlePass::~ParticlePass() = default;

bool ParticlePass::initialize(rhi::RHIDeviceBase* device) {
    if (!device) {
        return false;
    }
    
    device_ = device;
    
    // Load particle shader from external .metal file
    std::string shader_path = "Engine/Graphics/Metal/shaders/ParticleAtlas.metal";
    std::ifstream shader_file(shader_path);
    
    if (!shader_file.is_open()) {
        std::cerr << "ParticlePass: Failed to open shader file: " << shader_path << std::endl;
        return false;
    }
    
    std::string shader_code((std::istreambuf_iterator<char>(shader_file)), 
                           std::istreambuf_iterator<char>());
    shader_file.close();
    
    vertex_shader_ = device->CreateShader(
        shader_code.c_str(),
        shader_code.size(),
        rhi::ShaderStage::Vertex,
        "particle_vertex_instanced"
    );
    
    if (vertex_shader_ == rhi::handles::INVALID_SHADER) {
        std::cerr << "ParticlePass: Failed to create vertex shader" << std::endl;
        return false;
    }
    
    fragment_shader_ = device->CreateShader(
        shader_code.c_str(),
        shader_code.size(),
        rhi::ShaderStage::Pixel,
        "particle_fragment"
    );
    
    if (fragment_shader_ == rhi::handles::INVALID_SHADER) {
        std::cerr << "ParticlePass: Failed to create fragment shader" << std::endl;
        return false;
    }
    
    // Create pipeline layout with push constants
    {
        rhi::DescriptorSetLayoutBinding bindings[] = {
            { 0, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Vertex },  // Uniforms
            { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Vertex },  // Particle data
            { 2, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Vertex },  // Visible count
            { 3, rhi::DescriptorType::CombinedImageSampler, 1, rhi::ShaderStage::Pixel },  // Texture
        };
        
        rhi::DescriptorSetLayoutDesc layout_desc{};
        layout_desc.bindings = bindings;
        layout_desc.bindingCount = 4;
        descriptor_set_layout_ = device->CreateDescriptorSetLayout(layout_desc);
        
        rhi::PushConstantRange push_ranges[] = {
            { rhi::ShaderStage::Vertex, 0, sizeof(ParticlePushConstants) }
        };
        
        rhi::PipelineLayoutDesc pl_desc{};
        pl_desc.setLayoutCount = 1;
        pl_desc.setLayouts = &descriptor_set_layout_;
        pl_desc.pushConstantRangeCount = 1;
        pl_desc.pushConstantRanges = push_ranges;
        
        pipeline_layout_ = device->CreatePipelineLayout(pl_desc);
    }
    
    // Create pipelines for different blend modes
    if (!create_pipelines()) {
        return false;
    }
    
    // Create descriptor sets
    if (!create_descriptor_sets()) {
        return false;
    }
    
    // Create persistent buffers
    if (!create_buffers()) {
        return false;
    }
    
    // Create default white texture
    create_default_texture();
    
    return true;
}

bool ParticlePass::create_buffers() {
    // Create uniform buffers (mapped for frequent updates)
    rhi::BufferDesc uniform_desc{};
    uniform_desc.size = sizeof(ParticlePushConstants);
    uniform_desc.type = rhi::BufferType::Constant;
    uniform_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    
    // Create particle data buffers (large enough for max particles)
    rhi::BufferDesc particle_desc{};
    particle_desc.size = MAX_PARTICLE_BUFFER_SIZE;
    particle_desc.type = rhi::BufferType::Structured;
    particle_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    particle_desc.structured.elementCount = MAX_PARTICLE_BUFFER_SIZE / sizeof(particles::particle_data);
    particle_desc.structured.elementStride = sizeof(particles::particle_data);
    
    // Create count buffers
    rhi::BufferDesc count_desc{};
    count_desc.size = sizeof(u32) * 4;  // Small buffer for count
    count_desc.type = rhi::BufferType::Structured;
    count_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    count_desc.structured.elementCount = 4;
    count_desc.structured.elementStride = sizeof(u32);
    
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        uniform_buffers_[i] = device_->CreateBuffer(uniform_desc);
        particle_buffers_[i] = device_->CreateBuffer(particle_desc);
        count_buffers_[i] = device_->CreateBuffer(count_desc);
        
        if (uniform_buffers_[i] == rhi::handles::INVALID_RESOURCE ||
            particle_buffers_[i] == rhi::handles::INVALID_RESOURCE ||
            count_buffers_[i] == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "ParticlePass: Failed to create buffers for frame " << i << std::endl;
            return false;
        }
        
        // Map uniform buffer for persistent mapping
        uniform_buffer_mapped_[i] = device_->MapBuffer(uniform_buffers_[i]);
        if (!uniform_buffer_mapped_[i]) {
            std::cerr << "ParticlePass: Failed to map uniform buffer for frame " << i << std::endl;
            return false;
        }
    }
    
    return true;
}

void ParticlePass::shutdown() {
    if (!device_) {
        return;
    }
    
    // Destroy persistent buffers
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        if (uniform_buffer_mapped_[i]) {
            device_->UnmapBuffer(uniform_buffers_[i]);
            uniform_buffer_mapped_[i] = nullptr;
        }
        if (uniform_buffers_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(uniform_buffers_[i]);
            uniform_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        }
        if (particle_buffers_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(particle_buffers_[i]);
            particle_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        }
        if (count_buffers_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(count_buffers_[i]);
            count_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        }
    }
    
    // Destroy pipelines
    for (auto& pipeline : pipelines_) {
        if (pipeline != rhi::handles::INVALID_PIPELINE) {
            device_->DestroyPipeline(pipeline);
            pipeline = rhi::handles::INVALID_PIPELINE;
        }
    }
    
    // Destroy pipeline layout
    if (pipeline_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(pipeline_layout_);
        pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    }
    
    // Destroy descriptor sets and layout
    if (descriptor_set_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(descriptor_set_layout_);
        descriptor_set_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    
    for (auto& set : descriptor_sets_) {
        if (set != rhi::handles::INVALID_DESCRIPTOR_SET) {
            device_->DestroyDescriptorSet(set);
            set = rhi::handles::INVALID_DESCRIPTOR_SET;
        }
    }
    
    // Destroy shaders
    if (vertex_shader_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(vertex_shader_);
        vertex_shader_ = rhi::handles::INVALID_SHADER;
    }
    if (fragment_shader_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(fragment_shader_);
        fragment_shader_ = rhi::handles::INVALID_SHADER;
    }
    
    // Destroy default texture
    if (particle_texture_ != rhi::handles::INVALID_RESOURCE) {
        device_->DestroyTexture(particle_texture_);
        particle_texture_ = rhi::handles::INVALID_RESOURCE;
    }
    
    device_ = nullptr;
}

void ParticlePass::execute(rhi::RHICommandBuffer* cmd_buffer,
                           u32 frame_index,
                           const math::m4x4& view_matrix,
                           const math::m4x4& projection_matrix) {
    if (!device_ || !cmd_buffer) {
        return;
    }
    
    // Get particle data from system
    auto& pool = particles::get_frame_pool(frame_index);
    auto* particle_data = pool.data();
    u32 active_count = pool.allocated_count();
    
    if (active_count == 0 || !particle_data) {
        return;
    }
    
    // Get the appropriate pipeline for blend mode
    u32 blend_idx = static_cast<u32>(current_blend_mode_);
    rhi::PipelineHandle pipeline = pipelines_[blend_idx];
    
    if (pipeline == rhi::handles::INVALID_PIPELINE) {
        return;
    }
    
    // Bind pipeline
    cmd_buffer->BindGraphicsPipeline(pipeline);
    
    // Set push constants
    ParticlePushConstants pc{};
    pc.view_projection = projection_matrix * view_matrix;
    pc.view_matrix = view_matrix;
    pc.time = 0.0f;
    pc.delta_time = 0.016f;
    pc.blend_mode = blend_idx;
    pc.texture_frames_x = texture_frames_x_;
    pc.texture_frames_y = texture_frames_y_;
    pc.frame_rate = texture_frame_rate_;
    
    cmd_buffer->PushConstants(pipeline_layout_, rhi::ShaderStage::Vertex, 0, sizeof(pc), &pc);
    
    // Update uniform buffer (already mapped)
    if (uniform_buffer_mapped_[frame_index]) {
        memcpy(uniform_buffer_mapped_[frame_index], &pc, sizeof(pc));
    }
    
    // Update particle data buffer
    size_t particle_data_size = active_count * sizeof(particles::particle_data);
    void* particle_mapped = device_->MapBuffer(particle_buffers_[frame_index]);
    if (particle_mapped) {
        memcpy(particle_mapped, particle_data, particle_data_size);
        device_->UnmapBuffer(particle_buffers_[frame_index]);
    }
    
    // Update count buffer
    u32 visible_count = active_count;
    void* count_mapped = device_->MapBuffer(count_buffers_[frame_index]);
    if (count_mapped) {
        memcpy(count_mapped, &visible_count, sizeof(u32));
        device_->UnmapBuffer(count_buffers_[frame_index]);
    }
    
    // Bind buffers using BindVertexBuffers (works for Metal shader buffers too)
    rhi::ResourceHandle buffers[3] = { 
        uniform_buffers_[frame_index], 
        particle_buffers_[frame_index], 
        count_buffers_[frame_index] 
    };
    uint64_t offsets[3] = { 0, 0, 0 };
    cmd_buffer->BindVertexBuffers(0, 3, buffers, offsets);
    
    // TODO: Bind texture through descriptor set when texture support is implemented
    
    // Draw instanced: 6 vertices per particle quad, N instances
    // Draw(vertexCount, startVertex, instanceCount, startInstance)
    cmd_buffer->Draw(6, 0, active_count, 0);
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
    // Create pipeline for additive blending (most common for particles)
    {
        rhi::GraphicsPipelineDesc desc{};
        desc.layout = pipeline_layout_;
        desc.vertexShader = vertex_shader_;
        desc.pixelShader = fragment_shader_;
        
        // Render target format - match the lighting output
        desc.renderTargetFormats[0] = rhi::DataFormat::RGBA16_Float;
        desc.renderTargetCount = 1;
        
        // Depth - read only (no depth write for particles)
        desc.depthStencilFormat = rhi::DataFormat::D32_Float;
        desc.enableDepthTest = depth_test_enabled_;
        desc.enableDepthWrite = depth_write_enabled_;
        desc.depthFunc = rhi::ComparisonFunc::Less;
        
        // Additive blending (flat fields, not nested)
        desc.enableBlend = true;
        desc.srcColorBlendFactor = rhi::BlendFactor::SrcAlpha;
        desc.dstColorBlendFactor = rhi::BlendFactor::One;
        desc.colorBlendOp = rhi::BlendOp::Add;
        desc.srcAlphaBlendFactor = rhi::BlendFactor::One;
        desc.dstAlphaBlendFactor = rhi::BlendFactor::One;
        desc.alphaBlendOp = rhi::BlendOp::Add;
        
        // No culling for particles
        desc.cullMode = rhi::CullMode::None;
        
        // Vertex pulling - no vertex attributes
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        pipelines_[0] = device_->CreateGraphicsPipeline(desc);
        
        if (pipelines_[0] == rhi::handles::INVALID_PIPELINE) {
            std::cerr << "ParticlePass: Failed to create additive blend pipeline" << std::endl;
        }
    }
    
    // Create alpha blending pipeline
    {
        rhi::GraphicsPipelineDesc desc{};
        desc.layout = pipeline_layout_;
        desc.vertexShader = vertex_shader_;
        desc.pixelShader = fragment_shader_;
        
        desc.renderTargetFormats[0] = rhi::DataFormat::RGBA16_Float;
        desc.renderTargetCount = 1;
        
        desc.depthStencilFormat = rhi::DataFormat::D32_Float;
        desc.enableDepthTest = depth_test_enabled_;
        desc.enableDepthWrite = depth_write_enabled_;
        desc.depthFunc = rhi::ComparisonFunc::Less;
        
        // Alpha blending (flat fields)
        desc.enableBlend = true;
        desc.srcColorBlendFactor = rhi::BlendFactor::SrcAlpha;
        desc.dstColorBlendFactor = rhi::BlendFactor::InvSrcAlpha;  // Correct name
        desc.colorBlendOp = rhi::BlendOp::Add;
        desc.srcAlphaBlendFactor = rhi::BlendFactor::One;
        desc.dstAlphaBlendFactor = rhi::BlendFactor::InvSrcAlpha;
        desc.alphaBlendOp = rhi::BlendOp::Add;
        
        desc.cullMode = rhi::CullMode::None;
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        pipelines_[1] = device_->CreateGraphicsPipeline(desc);
        
        if (pipelines_[1] == rhi::handles::INVALID_PIPELINE) {
            std::cerr << "ParticlePass: Failed to create alpha blend pipeline" << std::endl;
        }
    }
    
    // Pipelines 2 and 3 (multiply, premultiplied) - not implemented yet
    // pipelines_[2] and pipelines_[3] remain INVALID_PIPELINE (already initialized)
    
    return pipelines_[0] != rhi::handles::INVALID_PIPELINE;
}

bool ParticlePass::create_descriptor_sets() {
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        rhi::DescriptorSetDesc desc{};
        desc.layout = descriptor_set_layout_;
        descriptor_sets_[i] = device_->CreateDescriptorSet(desc);
        
        if (descriptor_sets_[i] == rhi::handles::INVALID_DESCRIPTOR_SET) {
            return false;
        }
    }
    
    return true;
}

void ParticlePass::update_descriptor_set(u32 frame_index) {
    // Update descriptor set with current buffers
    // This would be called when particle buffers change
}

void ParticlePass::create_default_texture() {
    // Create 1x1 white texture as fallback
    // TODO: Implement when texture creation API is clarified
    // For now, particles will render without texture (procedural)
    
    /* Example of correct API:
    rhi::TextureDesc texture_desc{};
    texture_desc.size = math::u32v3{ 1, 1, 1 };
    texture_desc.format = rhi::DataFormat::RGBA8_UNorm;
    texture_desc.type = rhi::TextureType::Texture2D;
    texture_desc.usage = rhi::TextureUsage::ShaderResource;
    
    particle_texture_ = device_->CreateTexture(texture_desc);
    */
}

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
