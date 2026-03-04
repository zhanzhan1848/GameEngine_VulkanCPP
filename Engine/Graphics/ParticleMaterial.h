#pragma once

#include "CommonHeaders.h"
#include "Graphics/Material.h"
#include "Particles/ParticleTypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::graphics {

class ParticleMaterial {
public:
    ParticleMaterial();
    ~ParticleMaterial();
    
    DISABLE_COPY(ParticleMaterial);
    
    void set_blend_mode(particles::blend_mode mode);
    particles::blend_mode get_blend_mode() const { return blend_mode_; }
    
    void set_base_texture(rhi::ResourceHandle texture);
    rhi::ResourceHandle get_base_texture() const { return base_texture_; }
    
    void set_texture_sheet(u32 frames_x, u32 frames_y, f32 frame_rate);
    u32 get_texture_frames_x() const { return texture_frames_x_; }
    u32 get_texture_frames_y() const { return texture_frames_y_; }
    f32 get_texture_frame_rate() const { return texture_frame_rate_; }
    
    void set_depth_write_enabled(bool enabled);
    bool is_depth_write_enabled() const { return depth_write_enabled_; }
    
    void set_receive_shadows(bool enabled);
    bool receives_shadows() const { return receive_shadows_; }
    
    void set_soft_particles(bool enabled, f32 distance = 1.0f);
    bool uses_soft_particles() const { return soft_particles_; }
    f32 get_soft_particle_distance() const { return soft_particle_distance_; }
    
    Material* get_material() { return material_.get(); }
    const Material* get_material() const { return material_.get(); }
    
    bool initialize(rhi::RHIDeviceBase* device);
    void shutdown();
    
    rhi::PipelineHandle get_pipeline(rhi::RenderPassHandle render_pass);
    
private:
    void update_blend_state();
    
    std::unique_ptr<Material> material_;
    rhi::ResourceHandle base_texture_{ rhi::handles::INVALID_RESOURCE };
    
    particles::blend_mode blend_mode_{ particles::blend_mode::additive };
    
    u32 texture_frames_x_{ 1 };
    u32 texture_frames_y_{ 1 };
    f32 texture_frame_rate_{ 0.0f };
    
    bool depth_write_enabled_{ false };
    bool receive_shadows_{ false };
    bool soft_particles_{ false };
    f32 soft_particle_distance_{ 1.0f };
    
    rhi::RHIDeviceBase* device_{ nullptr };
};

namespace particle_material {

using material_id = u32;

material_id create(const particles::emitter_config& config);
void destroy(material_id id);

ParticleMaterial* get(material_id id);
const ParticleMaterial* get(material_id id) const;

void set_blend_mode(material_id id, particles::blend_mode mode);
void set_texture(material_id id, rhi::ResourceHandle texture);
void set_texture_sheet(material_id id, u32 frames_x, u32 frames_y, f32 frame_rate);

u32 get_cached_count();

} // namespace particle_material

} // namespace primal::graphics

#else

namespace primal::graphics {

class ParticleMaterial {
public:
    ParticleMaterial() = default;
    ~ParticleMaterial() = default;
    
    void set_blend_mode(particles::blend_mode) {}
    void set_base_texture(rhi::ResourceHandle) {}
    void set_texture_sheet(u32, u32, f32) {}
    void set_depth_write_enabled(bool) {}
    void set_receive_shadows(bool) {}
    void set_soft_particles(bool, f32 = 1.0f) {}
    
    bool initialize(rhi::RHIDeviceBase*) { return true; }
    void shutdown() {}
};

namespace particle_material {
    using material_id = u32;
    inline material_id create(const particles::emitter_config&) { return 0; }
    inline void destroy(material_id) {}
    inline ParticleMaterial* get(material_id) { return nullptr; }
}

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
