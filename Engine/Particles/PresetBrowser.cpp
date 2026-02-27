#include "PresetBrowser.h"

#ifndef DISABLE_PARTICLE_SYSTEM

// NOTE: ImGui-based preset browser implementation is not available in the Engine build.
// These stub implementations allow the Engine to compile without ImGui.
// The actual UI should be provided by the Editor application.

namespace primal::particles {
namespace preset_browser {

namespace {
    std::string g_selected_preset;
    std::string g_category_filter;
    std::string g_search_filter;
}

void initialize() {
    g_selected_preset.clear();
    g_category_filter.clear();
    g_search_filter.clear();
}

void shutdown() {
    g_selected_preset.clear();
    g_category_filter.clear();
    g_search_filter.clear();
}

std::string draw_preset_browser(bool* apply_clicked, bool* window_open, const char* window_name) {
    // Stub - ImGui not available in Engine build
    if (apply_clicked) *apply_clicked = false;
    if (window_open) *window_open = false;
    (void)window_name;
    return "";
}

std::string draw_preset_panel(const math::v2& size, bool* apply_clicked) {
    // Stub - ImGui not available in Engine build
    if (apply_clicked) *apply_clicked = false;
    (void)size;
    return "";
}

std::string draw_preset_dropdown(const char* label, const char* preview_value) {
    // Stub - ImGui not available in Engine build
    (void)label;
    (void)preview_value;
    return "";
}

void set_category_filter(const std::string& category) {
    g_category_filter = category;
}

const std::string& get_category_filter() {
    return g_category_filter;
}

void set_search_filter(const std::string& filter) {
    g_search_filter = filter;
}

const std::string& get_search_filter() {
    return g_search_filter;
}

void draw_preset_preview(const particle_preset& preset) {
    // Stub - ImGui not available in Engine build
    (void)preset;
}

void draw_preset_thumbnail(const std::string& preset_name, const math::v2& size) {
    // Stub - ImGui not available in Engine build
    (void)preset_name;
    (void)size;
}

const std::string& get_selected_preset() {
    return g_selected_preset;
}

void set_selected_preset(const std::string& preset_name) {
    g_selected_preset = preset_name;
}

void clear_selection() {
    g_selected_preset.clear();
}

bool has_selection() {
    return !g_selected_preset.empty();
}

} // namespace preset_browser
} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
