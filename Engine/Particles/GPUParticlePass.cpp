#include "GPUParticlePass.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include "Graphics/RHI/Platforms/Metal/MetalCommandBuffer.h"

#include <cstring>
#include <iostream>

namespace primal::particles::gpu {

// Embedded vertex/fragment shader for GPU particle rendering
static const char* gpu_particle_render_shader = R"(
#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;

struct GPUParticleData {
    float4 position;       // xyz = world position, w = age
    float4 velocity;       // xyz = velocity, w = lifetime
    float4 color;          // rgba
    float4 scale_rotation; // xy = scale, zw = rotation
};

struct GPURenderUniforms {
    float4x4 view_projection;
    float4x4 view_matrix;
    float time;
    float _pad[3];
};

struct GPUVertexOut {
    float4 position [[position]];
    float4 color;
    float2 uv;
    float age;
};

// Quad vertices (billboard)
constant float2 quad_positions[6] = {
    float2(-0.5, -0.5),
    float2( 0.5, -0.5),
    float2(-0.5,  0.5),
    float2(-0.5,  0.5),
    float2( 0.5, -0.5),
    float2( 0.5,  0.5)
};

constant float2 quad_uvs[6] = {
    float2(0.0, 0.0),
    float2(1.0, 0.0),
    float2(0.0, 1.0),
    float2(0.0, 1.0),
    float2(1.0, 0.0),
    float2(1.0, 1.0)
};

vertex GPUVertexOut gpu_particle_vertex(
    device const GPURenderUniforms& uniforms [[buffer(0)]],
    device const GPUParticleData* particles [[buffer(1)]],
    device const uint* visible_indices [[buffer(2)]],
    constant uint& visible_count [[buffer(3)]],
    uint vertex_id [[vertex_id]],
    uint instance_id [[instance_id]])
{
    GPUVertexOut out;
    
    // Cull instances beyond visible count
    if (instance_id >= visible_count) {
        out.position = float4(0.0, 0.0, -1000.0, 1.0);
        out.color = float4(0.0);
        out.uv = float2(0.0);
        out.age = 1.0;
        return out;
    }
    
    // Get particle from visible indices (sorted/culled)
    uint particle_idx = visible_indices[instance_id];
    device const GPUParticleData& p = particles[particle_idx];
    
    float2 scale = p.scale_rotation.xy;
    float2 quad_pos = quad_positions[vertex_id];
    float2 uv = quad_uvs[vertex_id];
    
    // Billboard: extract camera vectors from view matrix
    float3 camera_right = float3(uniforms.view_matrix[0][0], uniforms.view_matrix[1][0], uniforms.view_matrix[2][0]);
    float3 camera_up = float3(uniforms.view_matrix[0][1], uniforms.view_matrix[1][1], uniforms.view_matrix[2][1]);
    
    // Calculate world position
    float3 world_pos = p.position.xyz;
    float3 offset = camera_right * quad_pos.x * scale.x + camera_up * quad_pos.y * scale.y;
    
    float4 world_position = float4(world_pos + offset, 1.0);
    out.position = uniforms.view_projection * world_position;
    
    // Color with age-based alpha fade
    float age = p.position.w;
    float alpha_fade = 1.0 - smoothstep(0.7, 1.0, age);
    out.color = float4(p.color.rgb, p.color.a * alpha_fade);
    out.uv = uv;
    out.age = age;
    
    return out;
}

fragment float4 gpu_particle_fragment(
    GPUVertexOut in [[stage_in]])
{
    // Create circular shape from UV
    float2 centered = in.uv - 0.5;
    float dist = length(centered);
    
    if (dist > 0.5) {
        discard_fragment();
    }
    
    // Soft edge
    float alpha = 1.0 - smoothstep(0.3, 0.5, dist);
    
    return float4(in.color.rgb, in.color.a * alpha);
}
)";

bool gpu_particle_pass::initialize(rhi::RHIDeviceBase* device) {
    if (!device) {
        return false;
    }
    
    device_ = device;
    
    // Create shaders
    if (!create_shaders()) {
        std::cerr << "[GPUParticlePass] Failed to create shaders" << std::endl;
        return false;
    }
    
    // Create pipelines
    if (!create_pipelines()) {
        std::cerr << "[GPUParticlePass] Failed to create pipelines" << std::endl;
        return false;
    }
    
    // Create uniform buffers
    rhi::BufferDesc uniform_desc{};
    uniform_desc.size = sizeof(math::m4x4) * 2 + sizeof(f32) * 4;
    uniform_desc.type = rhi::BufferType::Constant;
    uniform_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        uniform_buffers_[i] = device_->CreateBuffer(uniform_desc);
        if (uniform_buffers_[i] == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[GPUParticlePass] Failed to create uniform buffer " << i << std::endl;
            return false;
        }
        
        uniform_mapped_[i] = device_->MapBuffer(uniform_buffers_[i]);
        if (!uniform_mapped_[i]) {
            std::cerr << "[GPUParticlePass] Failed to map uniform buffer " << i << std::endl;
            return false;
        }
    }
    
    std::cout << "[GPUParticlePass] Initialized successfully" << std::endl;
    return true;
}

void gpu_particle_pass::shutdown() {
    if (!device_) {
        return;
    }
    
    // Unmap and destroy uniform buffers
    for (u32 i = 0; i < rhi::MAX_FRAMES_IN_FLIGHT; ++i) {
        if (uniform_mapped_[i]) {
            device_->UnmapBuffer(uniform_buffers_[i]);
            uniform_mapped_[i] = nullptr;
        }
        if (uniform_buffers_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(uniform_buffers_[i]);
            uniform_buffers_[i] = rhi::handles::INVALID_RESOURCE;
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
    
    // Destroy descriptor layout
    if (descriptor_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(descriptor_layout_);
        descriptor_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
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
    
    device_ = nullptr;
}

bool gpu_particle_pass::create_shaders() {
    vertex_shader_ = device_->CreateShader(
        gpu_particle_render_shader,
        strlen(gpu_particle_render_shader),
        rhi::ShaderStage::Vertex,
        "gpu_particle_vertex"
    );
    
    if (vertex_shader_ == rhi::handles::INVALID_SHADER) {
        return false;
    }
    
    fragment_shader_ = device_->CreateShader(
        gpu_particle_render_shader,
        strlen(gpu_particle_render_shader),
        rhi::ShaderStage::Pixel,
        "gpu_particle_fragment"
    );
    
    if (fragment_shader_ == rhi::handles::INVALID_SHADER) {
        return false;
    }
    
    return true;
}

bool gpu_particle_pass::create_pipelines() {
    // Create descriptor layout
    rhi::DescriptorSetLayoutBinding bindings[] = {
        { 0, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Vertex },
        { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Vertex },
        { 2, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Vertex },
        { 3, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Vertex },
    };
    
    rhi::DescriptorSetLayoutDesc layout_desc{};
    layout_desc.bindings = bindings;
    layout_desc.bindingCount = 4;
    descriptor_layout_ = device_->CreateDescriptorSetLayout(layout_desc);
    
    if (descriptor_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        return false;
    }
    
    // Create pipeline layout
    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.setLayoutCount = 1;
    pl_desc.setLayouts = &descriptor_layout_;
    pipeline_layout_ = device_->CreatePipelineLayout(pl_desc);
    
    if (pipeline_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) {
        return false;
    }
    
    // Create additive blend pipeline
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
        
        desc.enableBlend = true;
        desc.srcColorBlendFactor = rhi::BlendFactor::SrcAlpha;
        desc.dstColorBlendFactor = rhi::BlendFactor::One;
        desc.colorBlendOp = rhi::BlendOp::Add;
        desc.srcAlphaBlendFactor = rhi::BlendFactor::One;
        desc.dstAlphaBlendFactor = rhi::BlendFactor::One;
        desc.alphaBlendOp = rhi::BlendOp::Add;
        
        desc.cullMode = rhi::CullMode::None;
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        pipelines_[0] = device_->CreateGraphicsPipeline(desc);
    }
    
    // Create alpha blend pipeline
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
        
        desc.enableBlend = true;
        desc.srcColorBlendFactor = rhi::BlendFactor::SrcAlpha;
        desc.dstColorBlendFactor = rhi::BlendFactor::InvSrcAlpha;
        desc.colorBlendOp = rhi::BlendOp::Add;
        desc.srcAlphaBlendFactor = rhi::BlendFactor::One;
        desc.dstAlphaBlendFactor = rhi::BlendFactor::InvSrcAlpha;
        desc.alphaBlendOp = rhi::BlendOp::Add;
        
        desc.cullMode = rhi::CullMode::None;
        desc.vertexAttributes.clear();
        desc.vertexBindings.clear();
        
        pipelines_[1] = device_->CreateGraphicsPipeline(desc);
    }
    
    // Pipelines 2 and 3 not implemented yet
    pipelines_[2] = rhi::handles::INVALID_PIPELINE;
    pipelines_[3] = rhi::handles::INVALID_PIPELINE;
    
    return pipelines_[0] != rhi::handles::INVALID_PIPELINE;
}

void gpu_particle_pass::execute(rhi::RHICommandBuffer* cmd_buffer,
                                 u32 frame_index,
                                 const math::m4x4& view_matrix,
                                 const math::m4x4& projection_matrix,
                                 rhi::ResourceHandle particle_buffer,
                                 rhi::ResourceHandle indirect_buffer,
                                 u32 max_particles) {
    if (!device_ || !cmd_buffer || max_particles == 0) {
        return;
    }
    
    // Get current pipeline
    u32 blend_idx = static_cast<u32>(current_blend_mode_);
    rhi::PipelineHandle pipeline = pipelines_[blend_idx];
    
    if (pipeline == rhi::handles::INVALID_PIPELINE) {
        return;
    }
    
    // Bind pipeline
    cmd_buffer->BindGraphicsPipeline(pipeline);
    
    // Update uniform buffer
    struct Uniforms {
        math::m4x4 view_projection;
        math::m4x4 view_matrix;
        f32 time;
        f32 _pad[3];
    };
    
    Uniforms uniforms{};
    uniforms.view_projection = projection_matrix * view_matrix;
    uniforms.view_matrix = view_matrix;
    uniforms.time = 0.0f;
    
    if (uniform_mapped_[frame_index]) {
        memcpy(uniform_mapped_[frame_index], &uniforms, sizeof(Uniforms));
    }
    
    // Create visible count buffer (simple: use max_particles as visible count)
    rhi::BufferDesc count_desc{};
    count_desc.size = sizeof(u32);
    count_desc.type = rhi::BufferType::Constant;
    count_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    
    rhi::ResourceHandle count_buffer = device_->CreateBuffer(count_desc);
    if (count_buffer != rhi::handles::INVALID_RESOURCE) {
        void* mapped = device_->MapBuffer(count_buffer);
        if (mapped) {
            u32 visible_count = max_particles;
            memcpy(mapped, &visible_count, sizeof(u32));
            device_->UnmapBuffer(count_buffer);
        }
    }
    
    // Create visible indices buffer (identity mapping for now)
    rhi::BufferDesc indices_desc{};
    indices_desc.size = max_particles * sizeof(u32);
    indices_desc.type = rhi::BufferType::Structured;
    indices_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    indices_desc.structured.elementCount = max_particles;
    indices_desc.structured.elementStride = sizeof(u32);
    
    rhi::ResourceHandle indices_buffer = device_->CreateBuffer(indices_desc);
    if (indices_buffer != rhi::handles::INVALID_RESOURCE) {
        void* mapped = device_->MapBuffer(indices_buffer);
        if (mapped) {
            u32* indices = static_cast<u32*>(mapped);
            for (u32 i = 0; i < max_particles; ++i) {
                indices[i] = i; // Identity mapping
            }
            device_->UnmapBuffer(indices_buffer);
        }
    }
    
    // Bind all buffers
    rhi::ResourceHandle buffers[4] = {
        uniform_buffers_[frame_index],
        particle_buffer,
        indices_buffer,
        count_buffer
    };
    uint64_t offsets[4] = { 0, 0, 0, 0 };
    cmd_buffer->BindVertexBuffers(0, 4, buffers, offsets);
    
    // Draw instanced
    cmd_buffer->Draw(6, 0, max_particles, 0);
    
    // Cleanup temporary buffers
    device_->DestroyBuffer(count_buffer);
    device_->DestroyBuffer(indices_buffer);
}

void gpu_particle_pass::set_blend_mode(blend_mode mode) {
    current_blend_mode_ = mode;
}

void gpu_particle_pass::set_depth_write_enabled(bool enabled) {
    depth_write_enabled_ = enabled;
}

void gpu_particle_pass::set_depth_test_enabled(bool enabled) {
    depth_test_enabled_ = enabled;
}

rhi::PipelineHandle gpu_particle_pass::get_current_pipeline() const {
    return pipelines_[static_cast<u32>(current_blend_mode_)];
}

} // namespace primal::particles::gpu

#endif // !DISABLE_PARTICLE_SYSTEM
