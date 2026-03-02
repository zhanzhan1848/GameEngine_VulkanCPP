#pragma once

#include "Common/CommonHeaders.h"
#include "ParticleTypes.h"
#include "ParticleCurve.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

namespace primal::particles {

// -----------------------------------------------------------------------------
// Particle Preset
// Serializable emitter configuration with optional curves
// -----------------------------------------------------------------------------

struct particle_preset {
    // Metadata
    std::string name;
    std::string category;
    std::string description;
    std::string author;
    u32 version{ 1 };
    
    // Emitter configuration
    emitter_config config;
    
    // Curve data (serialized separately from pointers)
    struct curve_data {
        std::vector<std::pair<f32, f32>> keyframes; // time, value
        curve_interpolation interpolation{ curve_interpolation::linear };
    };
    
    struct gradient_data {
        std::vector<std::pair<f32, math::v4>> keys; // time, color
    };
    
    struct vector_curve_data {
        std::vector<std::pair<f32, math::v3>> keys; // time, value
        curve_interpolation interpolation{ curve_interpolation::linear };
    };
    
    // Serialized curve data
    curve_data scale_curve;
    curve_data alpha_curve;
    curve_data velocity_curve;
    curve_data rotation_curve;
    gradient_data color_gradient;
    vector_curve_data force_curve;
    
    // Curve enable flags
    bool use_scale_curve{ false };
    bool use_alpha_curve{ false };
    bool use_color_gradient{ false };
    bool use_velocity_curve{ false };
    bool use_force_curve{ false };
    bool use_rotation_curve{ false };
    
    // Default constructor
    particle_preset() = default;
    
    // Construct from emitter config
    explicit particle_preset(const std::string& preset_name, const emitter_config& cfg);
    
    // Apply preset to emitter config (creates curve objects)
    void apply_to(emitter_config& cfg) const;
    
    // Extract curve data from emitter config
    void extract_curves_from(const emitter_config& cfg);
};

// -----------------------------------------------------------------------------
// Preset Manager
// Manages loading, saving, and organizing particle presets
// -----------------------------------------------------------------------------

class preset_manager {
public:
    using preset_callback = std::function<void(const std::string& preset_name)>;
    
    preset_manager() = default;
    ~preset_manager() = default;
    
    // Non-copyable
    preset_manager(const preset_manager&) = delete;
    preset_manager& operator=(const preset_manager&) = delete;
    
    // Preset management
    bool load_preset(const std::string& filepath);
    bool save_preset(const std::string& filepath, const particle_preset& preset);
    
    bool add_preset(const particle_preset& preset);
    bool remove_preset(const std::string& name);
    const particle_preset* get_preset(const std::string& name) const;
    
    // Category management
    std::vector<std::string> get_categories() const;
    std::vector<std::string> get_presets_in_category(const std::string& category) const;
    
    // Get all preset names
    std::vector<std::string> get_all_preset_names() const;
    
    // Load all presets from directory
    u32 load_presets_from_directory(const std::string& directory);
    
    // Save all presets to directory
    bool save_all_presets_to_directory(const std::string& directory);
    
    // Callbacks for UI updates
    void set_on_preset_added(preset_callback callback) { on_preset_added_ = std::move(callback); }
    void set_on_preset_removed(preset_callback callback) { on_preset_removed_ = std::move(callback); }
    
    // Preset count
    u32 preset_count() const { return static_cast<u32>(presets_.size()); }
    
    // Check if preset exists
    bool has_preset(const std::string& name) const;
    
private:
    std::unordered_map<std::string, particle_preset> presets_;
    std::unordered_map<std::string, std::vector<std::string>> categories_;
    
    preset_callback on_preset_added_;
    preset_callback on_preset_removed_;
};

// -----------------------------------------------------------------------------
// JSON Serialization Functions
// -----------------------------------------------------------------------------

namespace preset_json {

// Serialize preset to JSON string
std::string to_json(const particle_preset& preset);

// Deserialize preset from JSON string
bool from_json(const std::string& json_str, particle_preset& preset);

// Save preset to file
bool save_to_file(const std::string& filepath, const particle_preset& preset);

// Load preset from file
bool load_from_file(const std::string& filepath, particle_preset& preset);

} // namespace preset_json

// -----------------------------------------------------------------------------
// Default Presets
// -----------------------------------------------------------------------------

namespace default_presets {

// Fire effect preset
particle_preset fire();

// Smoke effect preset
particle_preset smoke();

// Explosion effect preset
particle_preset explosion();

// Magic/sparkle effect preset
particle_preset magic_sparkle();

// Rain effect preset
particle_preset rain();

// Snow effect preset
particle_preset snow();

// Dust/debris effect preset
particle_preset dust();

// Bubble effect preset
particle_preset bubbles();

// Register all default presets with manager
void register_defaults(preset_manager& manager);

} // namespace default_presets

// -----------------------------------------------------------------------------
// Global Preset Manager
// -----------------------------------------------------------------------------

namespace preset_system {

bool initialize();
void shutdown();

preset_manager* get();
bool is_initialized();

} // namespace preset_system

} // namespace primal::particles

#else

// Stub implementation when particle system is disabled
namespace primal::particles {

struct particle_preset {};
class preset_manager {};

namespace preset_system {
    inline bool initialize() { return true; }
    inline void shutdown() {}
    inline preset_manager* get() { return nullptr; }
    inline bool is_initialized() { return false; }
}

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
