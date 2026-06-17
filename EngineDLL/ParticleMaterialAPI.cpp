// ParticleMaterial C API: thin wrapper around particle_material:: free functions.
// material_id is passed through directly (no slot map needed since particle_material
// already manages its own id->material cache internally).
// All functions are no-throw and safe to call with pm_id=0.

#include "Common.h"
#include "CommonHeaders.h"
#include "../Engine/Graphics/ParticleMaterial.h"

#include <iostream>

using namespace primal;
using namespace primal::graphics;

extern "C" {

// --- Lifecycle ---

EDITOR_INTERFACE u32 CreateParticleMaterial() {
#ifndef DISABLE_PARTICLE_SYSTEM
    // Default emitter_config{} has sensible defaults (additive blend, etc.)
    particles::emitter_config config{};
    particle_material::material_id id = particle_material::create(config);
    if (id == 0) {
        std::cerr << "[ParticleMaterialAPI] CreateParticleMaterial: create returned 0\n";
    }
    return id;
#else
    return 0;
#endif
}

EDITOR_INTERFACE void DestroyParticleMaterial(u32 pm_id) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return;
    particle_material::destroy(pm_id);
#endif
}

// --- Blend Mode ---

EDITOR_INTERFACE u32 GetParticleMaterialBlendMode(u32 pm_id) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return 0;
    const ParticleMaterial* mat = particle_material::get(pm_id);
    if (!mat) return 0;
    return static_cast<u32>(mat->get_blend_mode());
#else
    return 0;
#endif
}

EDITOR_INTERFACE void SetParticleMaterialBlendMode(u32 pm_id, u32 mode) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return;
    // blend_mode is an enum class with 4 values (0-3); clamp to valid range
    if (mode > static_cast<u32>(particles::blend_mode::premultiplied)) return;
    particle_material::set_blend_mode(pm_id, static_cast<particles::blend_mode>(mode));
#endif
}

// --- Base Texture ---

EDITOR_INTERFACE u64 GetParticleMaterialBaseTexture(u32 pm_id) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return 0;
    const ParticleMaterial* mat = particle_material::get(pm_id);
    if (!mat) return 0;
    return static_cast<u64>(mat->get_base_texture());
#else
    return 0;
#endif
}

EDITOR_INTERFACE void SetParticleMaterialBaseTexture(u32 pm_id, u64 tex_handle) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return;
    particle_material::set_texture(pm_id, static_cast<rhi::ResourceHandle>(tex_handle));
#endif
}

// --- Texture Sheet (sprite atlas) ---

EDITOR_INTERFACE void GetParticleMaterialTextureSheet(u32 pm_id,
                                                       u32* fx, u32* fy, f32* fps) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) {
        if (fx) *fx = 0;
        if (fy) *fy = 0;
        if (fps) *fps = 0.0f;
        return;
    }
    const ParticleMaterial* mat = particle_material::get(pm_id);
    if (!mat) {
        if (fx) *fx = 0;
        if (fy) *fy = 0;
        if (fps) *fps = 0.0f;
        return;
    }
    if (fx)  *fx  = mat->get_texture_frames_x();
    if (fy)  *fy  = mat->get_texture_frames_y();
    if (fps) *fps = mat->get_texture_frame_rate();
#endif
}

EDITOR_INTERFACE void SetParticleMaterialTextureSheet(u32 pm_id, u32 fx, u32 fy, f32 fps) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return;
    particle_material::set_texture_sheet(pm_id, fx, fy, fps);
#endif
}

// --- Depth Write ---

EDITOR_INTERFACE u32 IsParticleMaterialDepthWriteEnabled(u32 pm_id) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return 0;
    const ParticleMaterial* mat = particle_material::get(pm_id);
    if (!mat) return 0;
    return mat->is_depth_write_enabled() ? 1u : 0u;
#else
    return 0;
#endif
}

EDITOR_INTERFACE void SetParticleMaterialDepthWriteEnabled(u32 pm_id, u32 enable) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return;
    ParticleMaterial* mat = particle_material::get(pm_id);
    if (!mat) return;
    mat->set_depth_write_enabled(enable != 0);
#endif
}

// --- Shadows ---

EDITOR_INTERFACE u32 ParticleMaterialReceivesShadows(u32 pm_id) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return 0;
    const ParticleMaterial* mat = particle_material::get(pm_id);
    if (!mat) return 0;
    return mat->receives_shadows() ? 1u : 0u;
#else
    return 0;
#endif
}

EDITOR_INTERFACE void SetParticleMaterialReceiveShadows(u32 pm_id, u32 enable) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return;
    ParticleMaterial* mat = particle_material::get(pm_id);
    if (!mat) return;
    mat->set_receive_shadows(enable != 0);
#endif
}

// --- Soft Particles ---

EDITOR_INTERFACE u32 ParticleMaterialUsesSoftParticles(u32 pm_id) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return 0;
    const ParticleMaterial* mat = particle_material::get(pm_id);
    if (!mat) return 0;
    return mat->uses_soft_particles() ? 1u : 0u;
#else
    return 0;
#endif
}

EDITOR_INTERFACE void SetParticleMaterialSoftParticles(u32 pm_id, u32 enable, f32 distance) {
#ifndef DISABLE_PARTICLE_SYSTEM
    if (pm_id == 0) return;
    ParticleMaterial* mat = particle_material::get(pm_id);
    if (!mat) return;
    mat->set_soft_particles(enable != 0, distance);
#endif
}

} // extern "C"
