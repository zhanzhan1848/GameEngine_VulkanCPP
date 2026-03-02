#pragma once

#include "Common/CommonHeaders.h"
#include "Particles/ParticleCurve.h"
#include "Particles/ParticleTypes.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles {

// -----------------------------------------------------------------------------
// Curve Editor Widget (ImGui-based)
// Provides inline editing for float_curve and color_gradient
// NOTE: Implementation requires ImGui. Stubs provided when ImGui unavailable.
// -----------------------------------------------------------------------------

namespace curve_editor {

// Initialize curve editor (call once at startup)
void initialize();

// Shutdown curve editor (call once at shutdown)
void shutdown();

// Draw float curve editor widget
// Returns true if curve was modified
bool draw_float_curve(
    const char* label,
    float_curve& curve,
    float min_value = 0.0f,
    float max_value = 1.0f,
    const char* preset_button_label = "Presets"
);

// Draw color gradient editor widget
// Returns true if gradient was modified
bool draw_color_gradient(
    const char* label,
    color_gradient& gradient,
    const char* preset_button_label = "Presets"
);

// Draw vector curve editor widget
// Returns true if curve was modified
bool draw_vector_curve(
    const char* label,
    vector_curve& curve,
    float min_value = -10.0f,
    float max_value = 10.0f
);

// Draw emitter curves editor (all curves in one panel)
// Returns true if any curve was modified
bool draw_emitter_curves(
    emitter_curves& curves,
    bool show_scale = true,
    bool show_alpha = true,
    bool show_color = true,
    bool show_velocity = true,
    bool show_force = true,
    bool show_rotation = true
);

// Helper: Draw curve presets popup
void draw_float_curve_presets(float_curve& curve);

// Helper: Draw gradient presets popup
void draw_color_gradient_presets(color_gradient& gradient);

} // namespace curve_editor

} // namespace primal::particles

#else

// Stub implementation when particle system is disabled
namespace primal::particles {

namespace curve_editor {

inline void initialize() {}
inline void shutdown() {}
inline bool draw_float_curve(const char*, float_curve&, float, float, const char*) { return false; }
inline bool draw_color_gradient(const char*, color_gradient&, const char*) { return false; }
inline bool draw_vector_curve(const char*, vector_curve&, float, float) { return false; }
inline bool draw_emitter_curves(emitter_curves&, bool, bool, bool, bool, bool, bool) { return false; }
inline void draw_float_curve_presets(float_curve&) {}
inline void draw_color_gradient_presets(color_gradient&) {}

} // namespace curve_editor

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
