#pragma once

#include "Common/CommonHeaders.h"
#include "Particles/ParticlePreset.h"

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles {

// -----------------------------------------------------------------------------
// Preset Browser Widget (ImGui-based)
// Provides UI for browsing and selecting particle presets
// -----------------------------------------------------------------------------

namespace preset_browser {

// Initialize preset browser (call once at startup)
void initialize();

// Shutdown preset browser (call once at shutdown)
void shutdown();

// Draw preset browser window
// Returns selected preset name (empty string if none selected)
// apply_clicked is set to true if user clicked "Apply"
std::string draw_preset_browser(
    bool* apply_clicked = nullptr,
    bool* window_open = nullptr,
    const char* window_name = "Particle Presets"
);

std::string draw_preset_panel(
    const math::v2& size = math::v2(),
    bool* apply_clicked = nullptr
);

// Draw compact preset dropdown (for toolbar use)
// Returns selected preset name (empty string if none selected)
std::string draw_preset_dropdown(
    const char* label = "##PresetDropdown",
    const char* preview_value = "Select preset..."
);

// Set current category filter (empty string shows all)
void set_category_filter(const std::string& category);

// Get current category filter
const std::string& get_category_filter();

// Set search filter
void set_search_filter(const std::string& filter);

// Get current search filter
const std::string& get_search_filter();

// Draw preset preview card
void draw_preset_preview(const particle_preset& preset);

// Draw preset thumbnail (placeholder for now)
void draw_preset_thumbnail(const std::string& preset_name, const math::v2& size);

// Get selected preset name (persists between frames)
const std::string& get_selected_preset();

// Set selected preset programmatically
void set_selected_preset(const std::string& preset_name);

// Clear selection
void clear_selection();

// Check if a preset is currently selected
bool has_selection();

} // namespace preset_browser

} // namespace primal::particles

#else

// Stub
namespace primal::particles {

namespace preset_browser {

inline void initialize() {}
inline void shutdown() {}
inline std::string draw_preset_browser(bool* = nullptr, bool* = nullptr, const char* = "") { return ""; }
inline std::string draw_preset_panel(const math::v2& = math::v2(), bool* = nullptr) { return ""; }
inline std::string draw_preset_dropdown(const char* = "", const char* = "") { return ""; }
inline void set_category_filter(const std::string&) {}
inline const std::string& get_category_filter() { static std::string s; return s; }
inline void set_search_filter(const std::string&) {}
inline const std::string& get_search_filter() { static std::string s; return s; }
inline void draw_preset_preview(const particle_preset&) {}
inline void draw_preset_thumbnail(const std::string&, const math::v2&) {}
inline const std::string& get_selected_preset() { static std::string s; return s; }
inline void set_selected_preset(const std::string&) {}
inline void clear_selection() {}
inline bool has_selection() { return false; }

} // namespace preset_browser

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
