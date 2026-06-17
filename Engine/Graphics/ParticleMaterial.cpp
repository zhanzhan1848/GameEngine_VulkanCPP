#include "ParticleMaterial.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <unordered_map>
#include <mutex>

namespace primal::graphics {

namespace {

std::unordered_map<particle_material::material_id, std::unique_ptr<ParticleMaterial>> material_cache;
std::mutex material_cache_mutex;
u32 next_material_id = 1;

rhi::BlendState get_blend_state_for_mode(particles::blend_mode mode) {
    rhi::BlendState state{};
    state.enableBlend = true;

    switch (mode) {
        case particles::blend_mode::additive:
            state.srcColorBlendFactor = rhi::BlendFactor::SrcAlpha;
            state.dstColorBlendFactor = rhi::BlendFactor::One;
            state.colorBlendOp = rhi::BlendOp::Add;
            state.srcAlphaBlendFactor = rhi::BlendFactor::One;
            state.dstAlphaBlendFactor = rhi::BlendFactor::One;
            state.alphaBlendOp = rhi::BlendOp::Add;
            break;

        case particles::blend_mode::alpha:
            state.srcColorBlendFactor = rhi::BlendFactor::SrcAlpha;
            state.dstColorBlendFactor = rhi::BlendFactor::InvSrcAlpha;
            state.colorBlendOp = rhi::BlendOp::Add;
            state.srcAlphaBlendFactor = rhi::BlendFactor::One;
            state.dstAlphaBlendFactor = rhi::BlendFactor::InvSrcAlpha;
            state.alphaBlendOp = rhi::BlendOp::Add;
            break;

        case particles::blend_mode::multiply:
            state.srcColorBlendFactor = rhi::BlendFactor::DestColor;
            state.dstColorBlendFactor = rhi::BlendFactor::InvSrcAlpha;
            state.colorBlendOp = rhi::BlendOp::Add;
            state.srcAlphaBlendFactor = rhi::BlendFactor::One;
            state.dstAlphaBlendFactor = rhi::BlendFactor::InvSrcAlpha;
            state.alphaBlendOp = rhi::BlendOp::Add;
            break;

        case particles::blend_mode::premultiplied:
            state.srcColorBlendFactor = rhi::BlendFactor::One;
            state.dstColorBlendFactor = rhi::BlendFactor::InvSrcAlpha;
            state.colorBlendOp = rhi::BlendOp::Add;
            state.srcAlphaBlendFactor = rhi::BlendFactor::One;
            state.dstAlphaBlendFactor = rhi::BlendFactor::InvSrcAlpha;
            state.alphaBlendOp = rhi::BlendOp::Add;
            break;
    }

    return state;
}

}

ParticleMaterial::ParticleMaterial() = default;
ParticleMaterial::~ParticleMaterial() = default;

void ParticleMaterial::set_blend_mode(particles::blend_mode mode) {
    blend_mode_ = mode;
    update_blend_state();
}

void ParticleMaterial::set_base_texture(rhi::ResourceHandle texture) {
    base_texture_ = texture;
}

void ParticleMaterial::set_texture_sheet(u32 frames_x, u32 frames_y, f32 frame_rate) {
    texture_frames_x_ = frames_x;
    texture_frames_y_ = frames_y;
    texture_frame_rate_ = frame_rate;
}

void ParticleMaterial::set_depth_write_enabled(bool enabled) {
    depth_write_enabled_ = enabled;
    if (material_) {
        rhi::DepthStencilState depth = material_->GetDepthStencilState();
        depth.enableDepthWrite = enabled;
        material_->SetDepthStencilState(depth);
    }
}

void ParticleMaterial::set_receive_shadows(bool enabled) {
    receive_shadows_ = enabled;
}

void ParticleMaterial::set_soft_particles(bool enabled, f32 distance) {
    soft_particles_ = enabled;
    soft_particle_distance_ = distance;
}

bool ParticleMaterial::initialize(rhi::RHIDeviceBase* device) {
    if (!device) {
        return false;
    }

    device_ = device;
    material_ = std::make_unique<Material>();

    update_blend_state();

    rhi::DepthStencilState depth{};
    depth.enableDepthTest = true;
    depth.enableDepthWrite = depth_write_enabled_;
    depth.depthFunc = rhi::ComparisonFunc::LessEqual;
    material_->SetDepthStencilState(depth);

    rhi::RasterizerState raster{};
    raster.cullMode = rhi::CullMode::None;
    material_->SetRasterizerState(raster);

    material_->SetTopology(rhi::PrimitiveTopology::TriangleList);

    return true;
}

void ParticleMaterial::shutdown() {
    material_.reset();
    device_ = nullptr;
}

rhi::PipelineHandle ParticleMaterial::get_pipeline(rhi::RenderPassHandle render_pass) {
    if (!material_ || !device_) {
        return rhi::handles::INVALID_PIPELINE;
    }
    
    return material_->GetPipeline(device_, render_pass, 0, PipelineFlags::None);
}

void ParticleMaterial::update_blend_state() {
    if (material_) {
        material_->SetBlendState(get_blend_state_for_mode(blend_mode_));
    }
}

namespace particle_material {

material_id create(const particles::emitter_config& config) {
    std::lock_guard<std::mutex> lock(material_cache_mutex);
    
    material_id id = next_material_id++;
    auto material = std::make_unique<ParticleMaterial>();
    
    material->set_blend_mode(config.blending);
    material->set_texture_sheet(config.texture_frames_x, config.texture_frames_y, config.texture_frame_rate);
    material->set_depth_write_enabled(config.depth_write);
    material->set_receive_shadows(config.receive_shadows);
    
    material_cache[id] = std::move(material);
    return id;
}

void destroy(material_id id) {
    std::lock_guard<std::mutex> lock(material_cache_mutex);
    
    auto it = material_cache.find(id);
    if (it != material_cache.end()) {
        it->second->shutdown();
        material_cache.erase(it);
    }
}

ParticleMaterial* get(material_id id) {
    std::lock_guard<std::mutex> lock(material_cache_mutex);

    auto it = material_cache.find(id);
    if (it != material_cache.end()) {
        return it->second.get();
    }
    return nullptr;
}

void set_blend_mode(material_id id, particles::blend_mode mode) {
    ParticleMaterial* material = get(id);
    if (material) {
        material->set_blend_mode(mode);
    }
}

void set_texture(material_id id, rhi::ResourceHandle texture) {
    ParticleMaterial* material = get(id);
    if (material) {
        material->set_base_texture(texture);
    }
}

void set_texture_sheet(material_id id, u32 frames_x, u32 frames_y, f32 frame_rate) {
    ParticleMaterial* material = get(id);
    if (material) {
        material->set_texture_sheet(frames_x, frames_y, frame_rate);
    }
}

u32 get_cached_count() {
    std::lock_guard<std::mutex> lock(material_cache_mutex);
    return static_cast<u32>(material_cache.size());
}

} // namespace particle_material

} // namespace primal::graphics

#endif // !DISABLE_PARTICLE_SYSTEM
