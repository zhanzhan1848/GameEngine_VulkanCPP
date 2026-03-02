#include "ParticlePass.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include "Particles/ParticleSystem.h"
#include "Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"
#include "Engine/Content/ContentToEngine.h"
#include "Utilities/IOStream.h"

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
    
    // Prevent double initialization - shutdown must be called first
    if (device_) {
        // Already initialized, return success
        return true;
    }
    
    device_ = device;
    // Load particle shader from shaders directory (relative to executable)
    std::string shader_path = "shaders/ParticleAtlas.metal";
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
    
    // Create default sampler for particle texture
    rhi::SamplerDesc samplerDesc{};
    samplerDesc.minFilter = rhi::FilterMode::Linear;
    samplerDesc.magFilter = rhi::FilterMode::Linear;
    samplerDesc.addressU = rhi::TextureAddressMode::Clamp;
    samplerDesc.addressV = rhi::TextureAddressMode::Clamp;
    defaultSampler_ = device_->CreateSampler(samplerDesc);
    
    // Update descriptor sets with texture and sampler
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        update_descriptor_set(i);
    }
    
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
        
        // Create index buffer for sorted indices
        index_buffers_[i] = device_->CreateBuffer(count_desc);  // Same size as count buffer
        if (index_buffers_[i] == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "ParticlePass: Failed to create index buffer for frame " << i << std::endl;
            return false;
        }
    }
    
    // Initialize sorted indices buffer
    sorted_indices_.resize(MAX_PARTICLE_BUFFER_SIZE / sizeof(particles::particle_data));
    
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
    
    // Destroy default sampler
    if (defaultSampler_ != rhi::handles::INVALID_SAMPLER) {
        device_->DestroySampler(defaultSampler_);
        defaultSampler_ = rhi::handles::INVALID_SAMPLER;
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
    
    std::cout << "[ParticlePass::execute] frame=" << frame_index 
              << ", active_count=" << active_count 
              << ", pipeline=" << (pipeline == rhi::handles::INVALID_PIPELINE ? "INVALID" : "VALID") 
              << std::endl;
    
    if (pipeline == rhi::handles::INVALID_PIPELINE) {
        std::cerr << "[ParticlePass::execute] ERROR: Pipeline is INVALID!" << std::endl;
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
    
    // Sort particles by distance if enabled
    // TODO: Need camera position from caller or implement math::inverse
    // For now, sorting is disabled until proper camera position extraction
    /*
    if (enable_sorting_ && active_count > 1) {
        // Camera position extraction would go here
        // math::v3 camera_pos = ...;
        
        // Resize sorted indices buffer if needed
        if (sorted_indices_.size() < active_count) {
            sorted_indices_.resize(active_count);
        }
        
        // Sort particles back-to-front
        particles::particle_sorter::sort_by_distance(
            particle_data,
            sorted_indices_.data(),
            active_count,
            camera_pos
        );
        
        // Upload sorted indices to index buffer (for future shader use)
        void* index_mapped = device_->MapBuffer(index_buffers_[frame_index]);
        if (index_mapped) {
            memcpy(index_mapped, sorted_indices_.data(), active_count * sizeof(u32));
            device_->UnmapBuffer(index_buffers_[frame_index]);
        }
    }
    */
    
    // Bind descriptor set with texture, sampler, and buffers
    // This binds:
    // - buffer slot 0: uniforms
    // - buffer slot 1: particle data
    // - buffer slot 2: count
    // - texture slot 0 + sampler slot 0: particle texture
    if (descriptor_sets_[frame_index] != rhi::handles::INVALID_DESCRIPTOR_SET) {
        cmd_buffer->BindDescriptorSets(rhi::PipelineBindPoint::Graphics, pipeline_layout_, 0, 1, &descriptor_sets_[frame_index], 0, nullptr);
    }
    
    
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
    if (frame_index >= rhi::MAX_FRAMES_IN_FLIGHT) return;
    if (descriptor_sets_[frame_index] == rhi::handles::INVALID_DESCRIPTOR_SET) return;
    
    // Update descriptor set with current buffers and texture
    rhi::WriteDescriptorSet writes[4];
    rhi::DescriptorBufferInfo bufferInfos[3];
    rhi::DescriptorImageInfo imageInfo{};
    
    // Binding 0: Uniform buffer
    writes[0].dstSet = descriptor_sets_[frame_index];
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = rhi::DescriptorType::UniformBuffer;
    bufferInfos[0].buffer = uniform_buffers_[frame_index];
    bufferInfos[0].offset = 0;
    bufferInfos[0].range = sizeof(ParticlePushConstants);
    writes[0].bufferInfo = &bufferInfos[0];
    
    // Binding 1: Particle data buffer
    writes[1].dstSet = descriptor_sets_[frame_index];
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = rhi::DescriptorType::StorageBuffer;
    bufferInfos[1].buffer = particle_buffers_[frame_index];
    bufferInfos[1].offset = 0;
    bufferInfos[1].range = MAX_PARTICLE_BUFFER_SIZE;
    writes[1].bufferInfo = &bufferInfos[1];
    
    // Binding 2: Count buffer
    writes[2].dstSet = descriptor_sets_[frame_index];
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = rhi::DescriptorType::StorageBuffer;
    bufferInfos[2].buffer = count_buffers_[frame_index];
    bufferInfos[2].offset = 0;
    bufferInfos[2].range = sizeof(u32) * 4;
    writes[2].bufferInfo = &bufferInfos[2];
    
    // Binding 3: Texture + Sampler (CombinedImageSampler)
    writes[3].dstSet = descriptor_sets_[frame_index];
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = rhi::DescriptorType::CombinedImageSampler;
    imageInfo.imageView = particle_texture_;
    imageInfo.imageLayout = rhi::ResourceState::ShaderResource;
    imageInfo.sampler = defaultSampler_;
    writes[3].imageInfo = &imageInfo;
    
    device_->UpdateDescriptorSets(4, writes);
}

void ParticlePass::create_default_texture() {
    // Create 1x1 white texture using content system
    uint32_t width = 1;
    uint32_t height = 1;
    rhi::DataFormat format = rhi::DataFormat::RGBA8_UNorm;
    uint32_t row_pitch = 4;
    uint32_t slice_pitch = 4;
    
    // White pixel data (RGBA)
    uint8_t white_pixel[4] = { 255, 255, 255, 255 };
    
    size_t blob_size = (6 * sizeof(uint32_t)) + (2 * sizeof(uint32_t) + slice_pitch);
    std::vector<uint8_t> blob(blob_size);
    utl::blob_stream_writer writer(blob.data(), blob.size());
    
    writer.write(width);
    writer.write(height);
    writer.write((uint32_t)1);  // array_size
    writer.write((uint32_t)0);  // flags
    writer.write((uint32_t)1);  // mip_levels
    writer.write((uint32_t)format);
    writer.write(row_pitch);
    writer.write(slice_pitch);
    writer.write(white_pixel, slice_pitch);

    id::id_type id = content::create_resource(blob.data(), content::asset_type::texture);
    if (id::is_valid(id)) {
        particle_texture_ = content::get_rhi_texture_handle(id);
        std::cout << "ParticlePass: Created default white texture" << std::endl;
    } else {
        std::cerr << "ParticlePass: Failed to create default texture" << std::endl;
    }
}

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
