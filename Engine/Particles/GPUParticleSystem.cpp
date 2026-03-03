#include "GPUParticleSystem.h"
#include "GPUParticleEmitter.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <iostream>
#include <cstring>

namespace primal::particles::gpu {

// Embedded Metal compute shader source for particle update
static const char* gpu_particle_compute_source = R"(
#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;

// Must match GPUParticleTypes.h structures
struct GPUEmitterConfig {
    float4x4 transform;
    float3 position;
    float _pad0;
    float spawn_rate;
    float time_accumulator;
    uint max_particles;
    uint active_particles;
    float lifetime_min;
    float lifetime_max;
    float _pad1[2];
    float3 velocity_min;
    float _pad2;
    float3 velocity_max;
    float _pad3;
    float4 color_start;
    float4 color_end;
    float2 scale_min;
    float2 scale_max;
    float3 gravity;
    float drag;
    float3 wind;
    float _pad4;
    uint blend_mode;
    uint emit_in_local_space;
    uint _pad5[2];
};

struct GPUParticleData {
    float4 position;       // xyz = world position, w = age
    float4 velocity;       // xyz = velocity, w = lifetime
    float4 color;          // rgba
    float4 scale_rotation; // xy = scale, zw = rotation
};

struct GPUCounters {
    uint alive_count;
    uint dead_count;
    uint visible_count;
    uint _pad;
};

// Random number generator (hash-based)
float hash(uint seed) {
    seed = (seed ^ 61) ^ (seed >> 16);
    seed *= 9;
    seed = seed ^ (seed >> 4);
    seed *= 0x27d4eb2d;
    seed = seed ^ (seed >> 15);
    return float(seed) / 4294967295.0;
}

float rand_range(float min_val, float max_val, thread uint& seed) {
    float r = hash(seed);
    seed++;
    return min_val + r * (max_val - min_val);
}

float3 rand_range3(float3 min_val, float3 max_val, thread uint& seed) {
    return float3(
        rand_range(min_val.x, max_val.x, seed),
        rand_range(min_val.y, max_val.y, seed),
        rand_range(min_val.z, max_val.z, seed)
    );
}

// -----------------------------------------------------------------------------
// Particle Update Kernel
// -----------------------------------------------------------------------------
kernel void particle_update_kernel(
    device GPUParticleData* particles [[buffer(0)]],
    device GPUCounters* counters [[buffer(1)]],
    constant GPUEmitterConfig& emitter [[buffer(2)]],
    constant float& delta_time [[buffer(3)]],
    uint gid [[thread_position_in_grid]])
{
    uint max_particles = emitter.max_particles;
    if (gid >= max_particles) return;
    
    device GPUParticleData& p = particles[gid];
    
    // Check if particle is alive (age < 1.0)
    float age = p.position.w;
    float lifetime = p.velocity.w;
    
    if (age >= 1.0) {
        // Particle is dead - try to respawn
        uint spawn_count = uint(emitter.spawn_rate * delta_time + emitter.time_accumulator);
        uint dead_count = counters[0].dead_count;
        
        // Simple respawn logic - in production use atomic counters
        if (gid < spawn_count && gid < max_particles) {
            thread uint seed = gid + uint(emitter.time_accumulator * 1000);
            
            // Initialize new particle
            float3 spawn_pos = emitter.position;
            float3 velocity = rand_range3(emitter.velocity_min, emitter.velocity_max, seed);
            float lifetime = rand_range(emitter.lifetime_min, emitter.lifetime_max, seed);
            float4 color = emitter.color_start;
            float2 scale = float2(
                rand_range(emitter.scale_min.x, emitter.scale_max.x, seed),
                rand_range(emitter.scale_min.y, emitter.scale_max.y, seed)
            );
            
            p.position = float4(spawn_pos, 0.0); // age = 0
            p.velocity = float4(velocity, lifetime);
            p.color = color;
            p.scale_rotation = float4(scale, 0.0, 0.0);
        }
        return;
    }
    
    // Update alive particle
    float dt = delta_time;
    
    // Apply forces
    float3 acceleration = emitter.gravity + emitter.wind;
    float3 new_velocity = p.velocity.xyz + acceleration * dt;
    
    // Apply drag
    float drag_factor = 1.0 - emitter.drag * dt;
    new_velocity *= max(0.0, drag_factor);
    
    // Update position
    float3 new_position = p.position.xyz + new_velocity * dt;
    
    // Update age
    float new_age = age + dt / lifetime;
    
    // Color interpolation based on age
    float4 new_color = mix(emitter.color_start, emitter.color_end, new_age);
    new_color.a *= (1.0 - new_age); // Fade out
    
    // Write back
    p.position = float4(new_position, new_age);
    p.velocity = float4(new_velocity, lifetime);
    p.color = new_color;
}

// -----------------------------------------------------------------------------
// Particle Culling Kernel
// -----------------------------------------------------------------------------
struct CullingUniforms {
    float4x4 view_projection;
    float4 frustum_planes[6];
};

kernel void particle_culling_kernel(
    device const GPUParticleData* particles [[buffer(0)]],
    device uint* visible_indices [[buffer(1)]],
    device GPUCounters* counters [[buffer(2)]],
    constant CullingUniforms& uniforms [[buffer(3)]],
    uint gid [[thread_position_in_grid]])
{
    uint max_particles = counters[0].alive_count;
    if (gid >= max_particles) return;
    
    device const GPUParticleData& p = particles[gid];
    
    // Check if particle is alive
    if (p.position.w >= 1.0) return;
    
    // Simple frustum culling - check against near plane only for now
    float4 clip_pos = uniforms.view_projection * float4(p.position.xyz, 1.0);
    
    // Check if behind camera or outside frustum
    if (clip_pos.w <= 0.0) return;
    if (abs(clip_pos.x) > clip_pos.w) return;
    if (abs(clip_pos.y) > clip_pos.w) return;
    
    // Particle is visible - add to visible list
    uint idx = atomic_fetch_add_explicit(
        (device atomic_uint*)&counters[0].visible_count, 
        1, 
        memory_order_relaxed
    );
    
    visible_indices[idx] = gid;
}
)";

// -----------------------------------------------------------------------------
// Global System Instance
// -----------------------------------------------------------------------------
namespace {
    gpu_particle_system* g_gpu_system = nullptr;
}

// -----------------------------------------------------------------------------
// gpu_particle_system Implementation
// -----------------------------------------------------------------------------

bool gpu_particle_system::initialize(rhi::RHIDeviceBase* device, const gpu_system_config& config) {
    if (!device) {
        std::cerr << "[GPUParticleSystem] Invalid device" << std::endl;
        return false;
    }
    
    if (initialized_) {
        return true;
    }
    
    device_ = device;
    config_ = config;
    
    // Create shaders
    if (!create_shaders()) {
        std::cerr << "[GPUParticleSystem] Failed to create shaders" << std::endl;
        return false;
    }
    
    // Create buffers
    if (!create_buffers()) {
        std::cerr << "[GPUParticleSystem] Failed to create buffers" << std::endl;
        return false;
    }
    
    // Create pipelines
    if (!create_pipelines()) {
        std::cerr << "[GPUParticleSystem] Failed to create pipelines" << std::endl;
        return false;
    }
    
    initialized_ = true;
    std::cout << "[GPUParticleSystem] Initialized successfully" << std::endl;
    return true;
}

void gpu_particle_system::shutdown() {
    if (!initialized_) {
        return;
    }
    
    // Destroy emitters
    emitters_.clear();
    free_emitter_indices_.clear();
    
    // Destroy GPU resources
    destroy_pipelines();
    destroy_buffers();
    
    // Destroy shaders
    if (update_shader_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(update_shader_);
        update_shader_ = rhi::handles::INVALID_SHADER;
    }
    if (culling_shader_ != rhi::handles::INVALID_SHADER) {
        device_->DestroyShader(culling_shader_);
        culling_shader_ = rhi::handles::INVALID_SHADER;
    }
    
    initialized_ = false;
    device_ = nullptr;
}

bool gpu_particle_system::create_shaders() {
    // Create update shader
    update_shader_ = device_->CreateShader(
        gpu_particle_compute_source,
        strlen(gpu_particle_compute_source),
        rhi::ShaderStage::Compute,
        "particle_update_kernel"
    );
    
    if (update_shader_ == rhi::handles::INVALID_SHADER) {
        std::cerr << "[GPUParticleSystem] Failed to create update shader" << std::endl;
        return false;
    }
    
    // Create culling shader
    culling_shader_ = device_->CreateShader(
        gpu_particle_compute_source,
        strlen(gpu_particle_compute_source),
        rhi::ShaderStage::Compute,
        "particle_culling_kernel"
    );
    
    if (culling_shader_ == rhi::handles::INVALID_SHADER) {
        std::cerr << "[GPUParticleSystem] Failed to create culling shader" << std::endl;
        return false;
    }
    
    return true;
}

bool gpu_particle_system::create_buffers() {
    u32 max_particles = config_.max_particles_per_emitter;
    
    // Particle data buffer (64 bytes per particle)
    rhi::BufferDesc particle_desc{};
    particle_desc.size = max_particles * sizeof(gpu_particle_data);
    particle_desc.type = rhi::BufferType::Structured;
    particle_desc.usage = rhi::GPUMemoryUsage::Static;
    particle_desc.structured.elementCount = max_particles;
    particle_desc.structured.elementStride = sizeof(gpu_particle_data);
    
    // Visible indices buffer
    rhi::BufferDesc visible_desc{};
    visible_desc.size = max_particles * sizeof(u32);
    visible_desc.type = rhi::BufferType::Structured;
    visible_desc.usage = rhi::GPUMemoryUsage::Static;
    visible_desc.structured.elementCount = max_particles;
    visible_desc.structured.elementStride = sizeof(u32);
    
    // Indirect command buffer
    rhi::BufferDesc indirect_desc{};
    indirect_desc.size = DRAW_INDIRECT_SIZE;
    indirect_desc.type = rhi::BufferType::Indirect;
    indirect_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    
    // Emitter config buffer
    rhi::BufferDesc emitter_desc{};
    emitter_desc.size = sizeof(gpu_emitter_config);
    emitter_desc.type = rhi::BufferType::Constant;
    emitter_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    
    // Counter buffer
    rhi::BufferDesc counter_desc{};
    counter_desc.size = sizeof(gpu_emitter_stats);
    counter_desc.type = rhi::BufferType::Structured;
    counter_desc.usage = rhi::GPUMemoryUsage::Dynamic;
    counter_desc.structured.elementCount = 1;
    counter_desc.structured.elementStride = sizeof(gpu_emitter_stats);
    
    for (u32 i = 0; i < BUFFER_COUNT; ++i) {
        particle_buffers_[i] = device_->CreateBuffer(particle_desc);
        visible_buffers_[i] = device_->CreateBuffer(visible_desc);
        indirect_buffers_[i] = device_->CreateBuffer(indirect_desc);
        emitter_buffers_[i] = device_->CreateBuffer(emitter_desc);
        counter_buffers_[i] = device_->CreateBuffer(counter_desc);
        
        if (particle_buffers_[i] == rhi::handles::INVALID_RESOURCE ||
            visible_buffers_[i] == rhi::handles::INVALID_RESOURCE ||
            indirect_buffers_[i] == rhi::handles::INVALID_RESOURCE ||
            emitter_buffers_[i] == rhi::handles::INVALID_RESOURCE ||
            counter_buffers_[i] == rhi::handles::INVALID_RESOURCE) {
            std::cerr << "[GPUParticleSystem] Failed to create buffers for frame " << i << std::endl;
            return false;
        }
    }
    
    return true;
}

bool gpu_particle_system::create_pipelines() {
    // Create descriptor set layout
    rhi::DescriptorSetLayoutBinding bindings[] = {
        { 0, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute },  // Particles
        { 1, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute },  // Visible indices
        { 2, rhi::DescriptorType::StorageBuffer, 1, rhi::ShaderStage::Compute },  // Counters
        { 3, rhi::DescriptorType::UniformBuffer, 1, rhi::ShaderStage::Compute },  // Emitter config
    };
    
    rhi::DescriptorSetLayoutDesc layout_desc{};
    layout_desc.bindings = bindings;
    layout_desc.bindingCount = 4;
    descriptor_layout_ = device_->CreateDescriptorSetLayout(layout_desc);
    
    if (descriptor_layout_ == rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        return false;
    }
    
    // Create pipeline layout
    rhi::PushConstantRange push_ranges[] = {
        { rhi::ShaderStage::Compute, 0, sizeof(f32) * 4 }  // delta_time + padding
    };
    
    rhi::PipelineLayoutDesc pl_desc{};
    pl_desc.setLayoutCount = 1;
    pl_desc.setLayouts = &descriptor_layout_;
    pl_desc.pushConstantRangeCount = 1;
    pl_desc.pushConstantRanges = push_ranges;
    pipeline_layout_ = device_->CreatePipelineLayout(pl_desc);
    
    if (pipeline_layout_ == rhi::handles::INVALID_PIPELINE_LAYOUT) {
        return false;
    }
    
    // Create update compute pipeline
    {
        rhi::ComputePipelineDesc desc{};
        desc.layout = pipeline_layout_;
        desc.computeShader = update_shader_;
        update_pipeline_ = device_->CreateComputePipeline(desc);
        
        if (update_pipeline_ == rhi::handles::INVALID_PIPELINE) {
            std::cerr << "[GPUParticleSystem] Failed to create update pipeline" << std::endl;
            return false;
        }
    }
    
    // Create culling compute pipeline
    {
        rhi::ComputePipelineDesc desc{};
        desc.layout = pipeline_layout_;
        desc.computeShader = culling_shader_;
        culling_pipeline_ = device_->CreateComputePipeline(desc);
        
        if (culling_pipeline_ == rhi::handles::INVALID_PIPELINE) {
            std::cerr << "[GPUParticleSystem] Failed to create culling pipeline" << std::endl;
            return false;
        }
    }
    
    // Create descriptor sets
    for (u32 i = 0; i < BUFFER_COUNT; ++i) {
        rhi::DescriptorSetDesc desc{};
        desc.layout = descriptor_layout_;
        descriptor_sets_[i] = device_->CreateDescriptorSet(desc);
        
        if (descriptor_sets_[i] == rhi::handles::INVALID_DESCRIPTOR_SET) {
            return false;
        }
    }
    
    return true;
}

void gpu_particle_system::destroy_buffers() {
    for (u32 i = 0; i < BUFFER_COUNT; ++i) {
        if (particle_buffers_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(particle_buffers_[i]);
            particle_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        }
        if (visible_buffers_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(visible_buffers_[i]);
            visible_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        }
        if (indirect_buffers_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(indirect_buffers_[i]);
            indirect_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        }
        if (emitter_buffers_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(emitter_buffers_[i]);
            emitter_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        }
        if (counter_buffers_[i] != rhi::handles::INVALID_RESOURCE) {
            device_->DestroyBuffer(counter_buffers_[i]);
            counter_buffers_[i] = rhi::handles::INVALID_RESOURCE;
        }
    }
}

void gpu_particle_system::destroy_pipelines() {
    if (update_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(update_pipeline_);
        update_pipeline_ = rhi::handles::INVALID_PIPELINE;
    }
    if (culling_pipeline_ != rhi::handles::INVALID_PIPELINE) {
        device_->DestroyPipeline(culling_pipeline_);
        culling_pipeline_ = rhi::handles::INVALID_PIPELINE;
    }
    if (pipeline_layout_ != rhi::handles::INVALID_PIPELINE_LAYOUT) {
        device_->DestroyPipelineLayout(pipeline_layout_);
        pipeline_layout_ = rhi::handles::INVALID_PIPELINE_LAYOUT;
    }
    if (descriptor_layout_ != rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT) {
        device_->DestroyDescriptorSetLayout(descriptor_layout_);
        descriptor_layout_ = rhi::handles::INVALID_DESCRIPTOR_SET_LAYOUT;
    }
    for (u32 i = 0; i < BUFFER_COUNT; ++i) {
        if (descriptor_sets_[i] != rhi::handles::INVALID_DESCRIPTOR_SET) {
            device_->DestroyDescriptorSet(descriptor_sets_[i]);
            descriptor_sets_[i] = rhi::handles::INVALID_DESCRIPTOR_SET;
        }
    }
}

gpu_emitter_id gpu_particle_system::create_emitter(const emitter_config& config) {
    gpu_emitter_id id;
    
    if (!free_emitter_indices_.empty()) {
        id = free_emitter_indices_.back();
        free_emitter_indices_.erase(id);
        emitters_[id] = std::make_unique<gpu_emitter>(id, config);
    } else {
        id = static_cast<gpu_emitter_id>(emitters_.size());
        emitters_.push_back(std::make_unique<gpu_emitter>(id, config));
    }
    
    return id;
}

void gpu_particle_system::destroy_emitter(gpu_emitter_id id) {
    if (id >= emitters_.size() || !emitters_[id]) {
        return;
    }
    
    emitters_[id].reset();
    free_emitter_indices_.push_back(id);
}

gpu_emitter* gpu_particle_system::get_emitter(gpu_emitter_id id) {
    if (id >= emitters_.size()) {
        return nullptr;
    }
    return emitters_[id].get();
}

const gpu_emitter* gpu_particle_system::get_emitter_const(gpu_emitter_id id) const {
    if (id >= emitters_.size()) {
        return nullptr;
    }
    return emitters_[id].get();
}

void gpu_particle_system::update(f32 delta_time, u32 frame_index) {
    if (!initialized_ || emitters_.empty()) {
        return;
    }
    
    // Update each emitter's emitter config buffer
    for (auto& emitter : emitters_) {
        if (!emitter || !emitter->is_active()) {
            continue;
        }
        
        // Update time accumulator for spawning
        emitter->add_time(delta_time);
        
        // Upload emitter config to GPU
        gpu_emitter_config gpu_config = emitter->get_gpu_config();
        void* mapped = device_->MapBuffer(emitter_buffers_[frame_index]);
        if (mapped) {
            memcpy(mapped, &gpu_config, sizeof(gpu_emitter_config));
            device_->UnmapBuffer(emitter_buffers_[frame_index]);
        }
    }
    
    // TODO: Dispatch compute shader
    // For now, CPU simulation continues to work
}

void gpu_particle_system::dispatch_update(rhi::RHICommandBuffer* cmd_buffer, u32 frame_index, f32 delta_time) {
    if (!initialized_ || !cmd_buffer || emitters_.empty()) {
        return;
    }
    
    // Calculate workgroup count based on max particles
    const u32 workgroup_size = WORKGROUP_SIZE;
    const u32 max_particles = config_.max_particles_per_emitter;
    const u32 group_count = (max_particles + workgroup_size - 1) / workgroup_size;
    
    // Bind compute pipeline
    cmd_buffer->BindComputePipeline(update_pipeline_);
    
    // Bind descriptor set
    cmd_buffer->BindDescriptorSets(
        rhi::PipelineBindPoint::Compute,
        pipeline_layout_,
        0,  // first set
        1,  // set count
        &descriptor_sets_[frame_index],
        0,  // dynamic offset count
        nullptr
    );
    
    // Push constants (delta_time)
    struct PushData {
        f32 delta_time;
        f32 time_accumulator;
        f32 _pad[2];
    } push_data{ delta_time, 0.0f, {0.0f, 0.0f} };
    
    cmd_buffer->PushConstants(
        pipeline_layout_,
        rhi::ShaderStage::Compute,
        0,
        sizeof(PushData),
        &push_data
    );
    
    // Dispatch compute shader
    cmd_buffer->Dispatch(group_count, 1, 1);
}

void gpu_particle_system::dispatch_culling(rhi::RHICommandBuffer* cmd_buffer, u32 frame_index, const math::m4x4& view_projection) {
    if (!initialized_ || !cmd_buffer || emitters_.empty()) {
        return;
    }
    
    // Calculate workgroup count
    const u32 workgroup_size = WORKGROUP_SIZE;
    const u32 max_particles = config_.max_particles_per_emitter;
    const u32 group_count = (max_particles + workgroup_size - 1) / workgroup_size;
    
    // Bind compute pipeline
    cmd_buffer->BindComputePipeline(culling_pipeline_);
    
    // Bind descriptor set
    cmd_buffer->BindDescriptorSets(
        rhi::PipelineBindPoint::Compute,
        pipeline_layout_,
        0,
        1,
        &descriptor_sets_[frame_index],
        0,
        nullptr
    );
    
    // Push constants (view_projection matrix)
    cmd_buffer->PushConstants(
        pipeline_layout_,
        rhi::ShaderStage::Compute,
        0,
        sizeof(math::m4x4),
        &view_projection
    );
    
    // Dispatch compute shader
    cmd_buffer->Dispatch(group_count, 1, 1);
}

void gpu_particle_system::prepare_draw(u32 frame_index) {
    // TODO: Prepare indirect draw commands
    (void)frame_index;
}

rhi::ResourceHandle gpu_particle_system::get_particle_buffer(u32 frame_index) const {
    if (frame_index >= BUFFER_COUNT) {
        return rhi::handles::INVALID_RESOURCE;
    }
    return particle_buffers_[frame_index];
}

rhi::ResourceHandle gpu_particle_system::get_visible_buffer(u32 frame_index) const {
    if (frame_index >= BUFFER_COUNT) {
        return rhi::handles::INVALID_RESOURCE;
    }
    return visible_buffers_[frame_index];
}

rhi::ResourceHandle gpu_particle_system::get_indirect_buffer(u32 frame_index) const {
    if (frame_index >= BUFFER_COUNT) {
        return rhi::handles::INVALID_RESOURCE;
    }
    return indirect_buffers_[frame_index];
}

rhi::ResourceHandle gpu_particle_system::get_emitter_buffer(u32 frame_index) const {
    if (frame_index >= BUFFER_COUNT) {
        return rhi::handles::INVALID_RESOURCE;
    }
    return emitter_buffers_[frame_index];
}

// -----------------------------------------------------------------------------
// Global System Functions
// -----------------------------------------------------------------------------

namespace system {

bool initialize(rhi::RHIDeviceBase* device, const gpu_system_config& config) {
    if (g_gpu_system) {
        return true;
    }
    
    g_gpu_system = new gpu_particle_system();
    if (!g_gpu_system->initialize(device, config)) {
        delete g_gpu_system;
        g_gpu_system = nullptr;
        return false;
    }
    
    return true;
}

void shutdown() {
    if (g_gpu_system) {
        g_gpu_system->shutdown();
        delete g_gpu_system;
        g_gpu_system = nullptr;
    }
}

gpu_particle_system* get() {
    return g_gpu_system;
}

bool is_initialized() {
    return g_gpu_system && g_gpu_system->is_initialized();
}

} // namespace system

} // namespace primal::particles::gpu

#endif // !DISABLE_PARTICLE_SYSTEM
