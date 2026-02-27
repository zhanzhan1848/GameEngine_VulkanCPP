#include "CurveEditor.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <imgui.h>

namespace primal::particles {
namespace curve_editor {

// Internal state for curve editing
static struct {
    int selected_keyframe = -1;
    bool is_dragging = false;
    char preset_popup_id[64] = "";
} g_state;

void initialize() {
    // Nothing to initialize for now
}

void shutdown() {
    // Nothing to shutdown
}

bool draw_float_curve(
    const char* label,
    float_curve& curve,
    float min_value,
    float max_value,
    const char* preset_button_label)
{
    bool modified = false;
    
    ImGui::PushID(label);
    
    // Interpolation mode selector
    const char* interp_modes[] = { "Constant", "Linear", "Smooth" };
    int interp_idx = static_cast<int>(curve.get_interpolation());
    if (ImGui::Combo("Interpolation", &interp_idx, interp_modes, IM_ARRAYSIZE(interp_modes))) {
        curve.set_interpolation(static_cast<curve_interpolation>(interp_idx));
        modified = true;
    }
    
    // Preset button
    if (ImGui::Button(preset_button_label)) {
        snprintf(g_state.preset_popup_id, sizeof(g_state.preset_popup_id), "##curves_presets_%s", label);
        ImGui::OpenPopup(g_state.preset_popup_id);
    }
    
    // Preset popup
    if (ImGui::BeginPopup(g_state.preset_popup_id)) {
        ImGui::Text("Curve Presets");
        ImGui::Separator();
        
        if (ImGui::Selectable("Constant (1.0)")) {
            curve = float_curve::constant(1.0f);
            modified = true;
        }
        if (ImGui::Selectable("Linear (0->1)")) {
            curve = float_curve::linear(0.0f, 1.0f);
            modified = true;
        }
        if (ImGui::Selectable("Linear (1->0)")) {
            curve = float_curve::linear(1.0f, 0.0f);
            modified = true;
        }
        if (ImGui::Selectable("Fade In")) {
            curve = float_curve::fade_in();
            modified = true;
        }
        if (ImGui::Selectable("Fade Out")) {
            curve = float_curve::fade_out();
            modified = true;
        }
        if (ImGui::Selectable("Fade In/Out")) {
            curve = float_curve::fade_in_out();
            modified = true;
        }
        if (ImGui::Selectable("Ease In")) {
            curve = float_curve::ease_in();
            modified = true;
        }
        if (ImGui::Selectable("Ease Out")) {
            curve = float_curve::ease_out();
            modified = true;
        }
        if (ImGui::Selectable("Ease In/Out")) {
            curve = float_curve::ease_in_out();
            modified = true;
        }
        if (ImGui::Selectable("Bounce")) {
            curve = float_curve::bounce();
            modified = true;
        }
        if (ImGui::Selectable("Elastic")) {
            curve = float_curve::elastic();
            modified = true;
        }
        
        ImGui::EndPopup();
    }
    
    // Curve canvas
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    canvas_size.y = 150.0f;
    
    ImGui::BeginChild("curve_canvas", canvas_size, true, ImGuiWindowFlags_NoMove);
    
    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvas_size.x, canvas_p0.y + canvas_size.y);
    
    // Draw grid
    draw_list->AddRectFilled(canvas_p0, canvas_p1, IM_COL32(30, 30, 30, 255));
    
    // Grid lines
    const int grid_divs = 10;
    for (int i = 0; i <= grid_divs; ++i) {
        float t = (float)i / grid_divs;
        ImVec2 p1 = ImVec2(canvas_p0.x + t * canvas_size.x, canvas_p0.y);
        ImVec2 p2 = ImVec2(canvas_p0.x + t * canvas_size.x, canvas_p1.y);
        draw_list->AddLine(p1, p2, IM_COL32(50, 50, 50, 255));
        
        p1 = ImVec2(canvas_p0.x, canvas_p0.y + t * canvas_size.y);
        p2 = ImVec2(canvas_p1.x, canvas_p0.y + t * canvas_size.y);
        draw_list->AddLine(p1, p2, IM_COL32(50, 50, 50, 255));
    }
    
    // Draw curve
    if (!curve.empty()) {
        const int num_samples = 100;
        ImVec2 prev_point;
        
        for (int i = 0; i <= num_samples; ++i) {
            float t = (float)i / num_samples;
            float value = curve.evaluate(t);
            value = (value - min_value) / (max_value - min_value);
            value = 1.0f - value; // Flip Y
            
            ImVec2 point(
                canvas_p0.x + t * canvas_size.x,
                canvas_p0.y + value * canvas_size.y
            );
            
            if (i > 0) {
                draw_list->AddLine(prev_point, point, IM_COL32(100, 200, 100, 255), 2.0f);
            }
            prev_point = point;
        }
        
        // Draw keyframes
        for (size_t i = 0; i < curve.keyframe_count(); ++i) {
            const auto& kf = curve.get_keyframe(i);
            float value = (kf.value - min_value) / (max_value - min_value);
            value = 1.0f - value;
            
            ImVec2 point(
                canvas_p0.x + kf.time * canvas_size.x,
                canvas_p0.y + value * canvas_size.y
            );
            
            ImU32 color = (g_state.selected_keyframe == (int)i) 
                ? IM_COL32(255, 200, 100, 255) 
                : IM_COL32(200, 200, 200, 255);
            
            draw_list->AddCircleFilled(point, 6.0f, color);
            draw_list->AddCircle(point, 6.0f, IM_COL32(255, 255, 255, 255));
        }
    }
    
    // Handle mouse interaction
    if (ImGui::IsWindowHovered()) {
        ImVec2 mouse_pos = ImGui::GetMousePos();
        
        // Convert to normalized coordinates
        float norm_x = (mouse_pos.x - canvas_p0.x) / canvas_size.x;
        float norm_y = 1.0f - (mouse_pos.y - canvas_p0.y) / canvas_size.y;
        
        if (ImGui::IsMouseClicked(0)) {
            // Check if clicking on existing keyframe
            int clicked_keyframe = -1;
            
            for (size_t i = 0; i < curve.keyframe_count(); ++i) {
                const auto& kf = curve.get_keyframe(i);
                float value = (kf.value - min_value) / (max_value - min_value);
                
                float dx = norm_x - kf.time;
                float dy = norm_y - value;
                float dist = sqrtf(dx * dx + dy * dy);
                
                if (dist < 0.05f) {
                    clicked_keyframe = (int)i;
                    break;
                }
            }
            
            if (clicked_keyframe >= 0) {
                g_state.selected_keyframe = clicked_keyframe;
                g_state.is_dragging = true;
            } else {
                // Add new keyframe
                float value = min_value + norm_y * (max_value - min_value);
                curve.add_keyframe(norm_x, value);
                g_state.selected_keyframe = (int)curve.keyframe_count() - 1;
                g_state.is_dragging = true;
                modified = true;
            }
        }
        
        if (ImGui::IsMouseReleased(0)) {
            g_state.is_dragging = false;
        }
        
        // Drag selected keyframe
        if (g_state.is_dragging && g_state.selected_keyframe >= 0 && 
            (int)curve.keyframe_count() > g_state.selected_keyframe) {
            // Note: For simplicity, we'd need mutable access to keyframes
            // This would require adding a set_keyframe method to float_curve
        }
        
        // Delete selected keyframe with right-click
        if (ImGui::IsMouseClicked(1) && g_state.selected_keyframe >= 0) {
            curve.remove_keyframe(g_state.selected_keyframe);
            g_state.selected_keyframe = -1;
            modified = true;
        }
    }
    
    ImGui::EndChild();
    
    // Show selected keyframe info
    if (g_state.selected_keyframe >= 0 && g_state.selected_keyframe < (int)curve.keyframe_count()) {
        const auto& kf = curve.get_keyframe(g_state.selected_keyframe);
        ImGui::Text("Keyframe %d: Time=%.2f, Value=%.2f", 
            g_state.selected_keyframe, kf.time, kf.value);
        
        // Edit selected keyframe value
        float value = kf.value;
        if (ImGui::SliderFloat("Value", &value, min_value, max_value)) {
            // Would need set_keyframe method
            modified = true;
        }
    }
    
    // Keyframe count
    ImGui::Text("Keyframes: %zu", curve.keyframe_count());
    
    // Add/Remove buttons
    if (ImGui::Button("Add")) {
        curve.add_keyframe(0.5f, (min_value + max_value) * 0.5f);
        modified = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        curve.clear();
        g_state.selected_keyframe = -1;
        modified = true;
    }
    
    ImGui::PopID();
    
    return modified;
}

bool draw_color_gradient(
    const char* label,
    color_gradient& gradient,
    const char* preset_button_label)
{
    bool modified = false;
    
    ImGui::PushID(label);
    
    // Preset button
    if (ImGui::Button(preset_button_label)) {
        snprintf(g_state.preset_popup_id, sizeof(g_state.preset_popup_id), "##gradient_presets_%s", label);
        ImGui::OpenPopup(g_state.preset_popup_id);
    }
    
    // Preset popup
    if (ImGui::BeginPopup(g_state.preset_popup_id)) {
        ImGui::Text("Gradient Presets");
        ImGui::Separator();
        
        if (ImGui::Selectable("Fire")) {
            gradient = color_gradient::fire();
            modified = true;
        }
        if (ImGui::Selectable("Smoke")) {
            gradient = color_gradient::smoke();
            modified = true;
        }
        if (ImGui::Selectable("Magic")) {
            gradient = color_gradient::magic();
            modified = true;
        }
        if (ImGui::Selectable("Explosion")) {
            gradient = color_gradient::explosion();
            modified = true;
        }
        if (ImGui::Selectable("Rainbow")) {
            gradient = color_gradient::rainbow();
            modified = true;
        }
        
        ImGui::EndPopup();
    }
    
    // Gradient preview
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 canvas_size = ImGui::GetContentRegionAvail();
    canvas_size.y = 30.0f;
    
    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvas_size.x, canvas_p0.y + canvas_size.y);
    
    // Draw gradient
    if (!gradient.empty()) {
        const int num_samples = 100;
        
        for (int i = 0; i < num_samples; ++i) {
            float t1 = (float)i / num_samples;
            float t2 = (float)(i + 1) / num_samples;
            
            math::v4 c1 = gradient.evaluate(t1);
            math::v4 c2 = gradient.evaluate(t2);
            
            ImVec2 p1(canvas_p0.x + t1 * canvas_size.x, canvas_p0.y);
            ImVec2 p2(canvas_p0.x + t2 * canvas_size.x, canvas_p1.y);
            
            ImU32 col1 = IM_COL32(
                (int)(c1.x * 255), (int)(c1.y * 255), 
                (int)(c1.z * 255), (int)(c1.w * 255));
            
            draw_list->AddRectFilledMultiColor(
                p1, p2,
                col1, col1, col1, col1);
        }
    }
    
    ImGui::Dummy(canvas_size);
    
    // Draw keyframe markers
    if (!gradient.empty()) {
        ImVec2 marker_pos = canvas_p0;
        
        for (size_t i = 0; i < gradient.key_count(); ++i) {
            const auto& key = gradient.get_key(i);
            
            marker_pos.x = canvas_p0.x + key.time * canvas_size.x;
            
            ImU32 marker_color = IM_COL32(255, 255, 255, 255);
            draw_list->AddTriangleFilled(
                ImVec2(marker_pos.x, canvas_p1.y + 2),
                ImVec2(marker_pos.x - 5, canvas_p1.y + 10),
                ImVec2(marker_pos.x + 5, canvas_p1.y + 10),
                marker_color);
        }
    }
    
    // Keyframe editing
    ImGui::Spacing();
    ImGui::Text("Color Keys: %zu", gradient.key_count());
    
    // Simple color key list
    for (size_t i = 0; i < gradient.key_count(); ++i) {
        const auto& key = gradient.get_key(i);
        
        ImGui::PushID((int)i);
        
        float time = key.time;
        float color[4] = { key.color.x, key.color.y, key.color.z, key.color.w };
        
        if (ImGui::SliderFloat("Time", &time, 0.0f, 1.0f)) {
            // Would need set_key method
            modified = true;
        }
        
        if (ImGui::ColorEdit4("Color", color)) {
            // Would need set_key method
            modified = true;
        }
        
        ImGui::PopID();
    }
    
    // Add/Remove buttons
    if (ImGui::Button("Add Key")) {
        gradient.add_color(0.5f, math::v4{ 1.0f, 1.0f, 1.0f, 1.0f });
        modified = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        gradient.clear();
        modified = true;
    }
    
    ImGui::PopID();
    
    return modified;
}

bool draw_vector_curve(
    const char* label,
    vector_curve& curve,
    float min_value,
    float max_value)
{
    bool modified = false;
    
    ImGui::PushID(label);
    
    // Simple vector curve display (3 components)
    ImGui::Text("Vector Curve: %zu keys", curve.key_count());
    
    // Key list
    for (size_t i = 0; i < curve.key_count(); ++i) {
        const auto& key = curve.get_key(i);
        
        ImGui::PushID((int)i);
        
        ImGui::Text("Key %zu: t=%.2f, v=(%.2f, %.2f, %.2f)", 
            i, key.time, key.value.x, key.value.y, key.value.z);
        
        ImGui::PopID();
    }
    
    // Add/Remove buttons
    if (ImGui::Button("Add Key")) {
        curve.add_key(0.5f, math::v3{ 0.0f, 0.0f, 0.0f });
        modified = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) {
        curve.clear();
        modified = true;
    }
    
    ImGui::PopID();
    
    return modified;
}

bool draw_emitter_curves(
    emitter_curves& curves,
    bool show_scale,
    bool show_alpha,
    bool show_color,
    bool show_velocity,
    bool show_force,
    bool show_rotation)
{
    bool modified = false;
    
    ImGui::BeginChild("emitter_curves", ImVec2(0, 300), true);
    
    if (ImGui::CollapsingHeader("Scale Curve", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("Enable", &curves.use_scale_curve)) {
            modified = true;
        }
        if (curves.use_scale_curve && curves.scale_curve) {
            if (draw_float_curve("scale", *curves.scale_curve, 0.0f, 3.0f)) {
                modified = true;
            }
        }
    }
    
    if (ImGui::CollapsingHeader("Alpha Curve")) {
        if (ImGui::Checkbox("Enable", &curves.use_alpha_curve)) {
            modified = true;
        }
        if (curves.use_alpha_curve && curves.alpha_curve) {
            if (draw_float_curve("alpha", *curves.alpha_curve, 0.0f, 1.0f)) {
                modified = true;
            }
        }
    }
    
    if (ImGui::CollapsingHeader("Color Gradient")) {
        if (ImGui::Checkbox("Enable", &curves.use_color_gradient)) {
            modified = true;
        }
        if (curves.use_color_gradient && curves.color_gradient) {
            if (draw_color_gradient("color", *curves.color_gradient)) {
                modified = true;
            }
        }
    }
    
    if (ImGui::CollapsingHeader("Velocity Curve")) {
        if (ImGui::Checkbox("Enable", &curves.use_velocity_curve)) {
            modified = true;
        }
        if (curves.use_velocity_curve && curves.velocity_curve) {
            if (draw_float_curve("velocity", *curves.velocity_curve, 0.0f, 2.0f)) {
                modified = true;
            }
        }
    }
    
    if (ImGui::CollapsingHeader("Force Curve")) {
        if (ImGui::Checkbox("Enable", &curves.use_force_curve)) {
            modified = true;
        }
        if (curves.use_force_curve && curves.force_curve) {
            if (draw_vector_curve("force", *curves.force_curve)) {
                modified = true;
            }
        }
    }
    
    if (ImGui::CollapsingHeader("Rotation Curve")) {
        if (ImGui::Checkbox("Enable", &curves.use_rotation_curve)) {
            modified = true;
        }
        if (curves.use_rotation_curve && curves.rotation_curve) {
            if (draw_float_curve("rotation", *curves.rotation_curve, 0.0f, 10.0f)) {
                modified = true;
            }
        }
    }
    
    ImGui::EndChild();
    
    return modified;
}

void draw_float_curve_presets(float_curve& curve) {
    if (ImGui::BeginPopup("float_curve_presets")) {
        ImGui::Text("Curve Presets");
        ImGui::Separator();
        
        if (ImGui::Selectable("Constant (1.0)")) {
            curve = float_curve::constant(1.0f);
        }
        if (ImGui::Selectable("Fade In")) {
            curve = float_curve::fade_in();
        }
        if (ImGui::Selectable("Fade Out")) {
            curve = float_curve::fade_out();
        }
        if (ImGui::Selectable("Fade In/Out")) {
            curve = float_curve::fade_in_out();
        }
        if (ImGui::Selectable("Ease In")) {
            curve = float_curve::ease_in();
        }
        if (ImGui::Selectable("Ease Out")) {
            curve = float_curve::ease_out();
        }
        if (ImGui::Selectable("Bounce")) {
            curve = float_curve::bounce();
        }
        
        ImGui::EndPopup();
    }
}

void draw_color_gradient_presets(color_gradient& gradient) {
    if (ImGui::BeginPopup("color_gradient_presets")) {
        ImGui::Text("Gradient Presets");
        ImGui::Separator();
        
        if (ImGui::Selectable("Fire")) {
            gradient = color_gradient::fire();
        }
        if (ImGui::Selectable("Smoke")) {
            gradient = color_gradient::smoke();
        }
        if (ImGui::Selectable("Magic")) {
            gradient = color_gradient::magic();
        }
        if (ImGui::Selectable("Explosion")) {
            gradient = color_gradient::explosion();
        }
        if (ImGui::Selectable("Rainbow")) {
            gradient = color_gradient::rainbow();
        }
        
        ImGui::EndPopup();
    }
}

} // namespace curve_editor
} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
