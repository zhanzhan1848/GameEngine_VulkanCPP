#include "CurveEditor.h"

#ifndef DISABLE_PARTICLE_SYSTEM

// NOTE: ImGui-based curve editor implementation is not available in the Engine build.
// These stub implementations allow the Engine to compile without ImGui.
// The actual UI should be provided by the Editor application.

namespace primal::particles {
namespace curve_editor {

void initialize() {
    // Stub - no initialization needed without ImGui
}

void shutdown() {
    // Stub - no shutdown needed without ImGui
}

bool draw_float_curve(
    const char* /*label*/,
    float_curve& /*curve*/,
    float /*min_value*/,
    float /*max_value*/,
    const char* /*preset_button_label*/)
{
    // Stub - ImGui not available in Engine build
    return false;
}

bool draw_color_gradient(
    const char* /*label*/,
    color_gradient& /*gradient*/,
    const char* /*preset_button_label*/)
{
    // Stub - ImGui not available in Engine build
    return false;
}

bool draw_vector_curve(
    const char* /*label*/,
    vector_curve& /*curve*/,
    float /*min_value*/,
    float /*max_value*/)
{
    // Stub - ImGui not available in Engine build
    return false;
}

bool draw_emitter_curves(
    emitter_curves& /*curves*/,
    bool /*show_scale*/,
    bool /*show_alpha*/,
    bool /*show_color*/,
    bool /*show_velocity*/,
    bool /*show_force*/,
    bool /*show_rotation*/)
{
    // Stub - ImGui not available in Engine build
    return false;
}

void draw_float_curve_presets(float_curve& /*curve*/) {
    // Stub - ImGui not available in Engine build
}

void draw_color_gradient_presets(color_gradient& /*gradient*/) {
    // Stub - ImGui not available in Engine build
}

} // namespace curve_editor
} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
