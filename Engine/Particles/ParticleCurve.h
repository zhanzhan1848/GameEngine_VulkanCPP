#pragma once
#include "Common/CommonHeaders.h"
#include <vector>
#include <algorithm>
#include <cmath>

#ifndef DISABLE_PARTICLE_SYSTEM

namespace primal::particles {

// -----------------------------------------------------------------------------
// Curve Keyframe
// Generic keyframe for animation curves
// -----------------------------------------------------------------------------

template<typename T>
struct curve_keyframe {
    f32 time{ 0.0f };      // Normalized time [0, 1]
    T value{};             // Value at this keyframe
    f32 in_tangent{ 0.0f };  // Tangent for smooth interpolation
    f32 out_tangent{ 0.0f }; // Tangent for smooth interpolation
    
    curve_keyframe() = default;
    curve_keyframe(f32 t, const T& v) : time(t), value(v) {}
    curve_keyframe(f32 t, const T& v, f32 in_tan, f32 out_tan) 
        : time(t), value(v), in_tangent(in_tan), out_tangent(out_tan) {}
};

// -----------------------------------------------------------------------------
// Interpolation Mode
// -----------------------------------------------------------------------------

enum class curve_interpolation : u8 {
    constant = 0,    // Step function (no interpolation)
    linear = 1,      // Linear interpolation
    smooth = 2,      // Hermite spline interpolation
    bezier = 3       // Cubic bezier (for advanced curves)
};

// -----------------------------------------------------------------------------
// Float Curve
// Single float value over normalized time [0, 1]
// -----------------------------------------------------------------------------

class float_curve {
public:
    float_curve() = default;
    explicit float_curve(std::initializer_list<curve_keyframe<f32>> keys);
    
    // Add keyframes
    void add_keyframe(f32 time, f32 value);
    void add_keyframe(const curve_keyframe<f32>& key);
    void remove_keyframe(size_t index);
    void clear();
    
    // Evaluate curve at normalized time [0, 1]
    f32 evaluate(f32 time) const;
    
    // Accessors
    size_t keyframe_count() const { return keyframes_.size(); }
    const curve_keyframe<f32>& get_keyframe(size_t index) const { return keyframes_[index]; }
    void set_interpolation(curve_interpolation mode) { interpolation_ = mode; }
    curve_interpolation get_interpolation() const { return interpolation_; }
    
    // Utility
    bool empty() const { return keyframes_.empty(); }
    void sort_keyframes();
    
    // Preset curves
    static float_curve constant(f32 value);
    static float_curve linear(f32 start, f32 end);
    static float_curve fade_in();
    static float_curve fade_out();
    static float_curve fade_in_out();
    static float_curve ease_in();
    static float_curve ease_out();
    static float_curve ease_in_out();
    static float_curve bounce();
    static float_curve elastic();
    
private:
    std::vector<curve_keyframe<f32>> keyframes_;
    curve_interpolation interpolation_{ curve_interpolation::linear };
    
    size_t find_keyframe_index(f32 time) const;
    f32 evaluate_constant(f32 time) const;
    f32 evaluate_linear(f32 time) const;
    f32 evaluate_smooth(f32 time) const;
};

// -----------------------------------------------------------------------------
// Color Gradient
// RGBA color over normalized time [0, 1]
// -----------------------------------------------------------------------------

class color_gradient {
public:
    struct color_key {
        f32 time{ 0.0f };
        math::v4 color{ 1.0f, 1.0f, 1.0f, 1.0f };
        
        color_key() = default;
        color_key(f32 t, const math::v4& c) : time(t), color(c) {}
        color_key(f32 t, f32 r, f32 g, f32 b, f32 a = 1.0f) 
            : time(t), color{ r, g, b, a } {}
    };
    
    color_gradient() = default;
    explicit color_gradient(std::initializer_list<color_key> keys);
    
    // Add color keys
    void add_color(f32 time, const math::v4& color);
    void add_color(f32 time, f32 r, f32 g, f32 b, f32 a = 1.0f);
    void remove_key(size_t index);
    void clear();
    
    // Evaluate gradient at normalized time [0, 1]
    math::v4 evaluate(f32 time) const;
    
    // Accessors
    size_t key_count() const { return keys_.size(); }
    const color_key& get_key(size_t index) const { return keys_[index]; }
    
    // Utility
    bool empty() const { return keys_.empty(); }
    void sort_keys();
    
    // Preset gradients
    static color_gradient solid(const math::v4& color);
    static color_gradient fade(const math::v4& color, f32 start_alpha, f32 end_alpha);
    static color_gradient fire();
    static color_gradient smoke();
    static color_gradient magic();
    static color_gradient explosion();
    static color_gradient rainbow();
    
private:
    std::vector<color_key> keys_;
    
    size_t find_key_index(f32 time) const;
};

// -----------------------------------------------------------------------------
// Vector Curve
// 3D vector over normalized time [0, 1]
// -----------------------------------------------------------------------------

class vector_curve {
public:
    struct vector_key {
        f32 time{ 0.0f };
        math::v3 value{ 0.0f, 0.0f, 0.0f };
        
        vector_key() = default;
        vector_key(f32 t, const math::v3& v) : time(t), value(v) {}
        vector_key(f32 t, f32 x, f32 y, f32 z) : time(t), value{ x, y, z } {}
    };
    
    vector_curve() = default;
    explicit vector_curve(std::initializer_list<vector_key> keys);
    
    // Add vector keys
    void add_key(f32 time, const math::v3& value);
    void add_key(f32 time, f32 x, f32 y, f32 z);
    void remove_key(size_t index);
    void clear();
    
    // Evaluate curve at normalized time [0, 1]
    math::v3 evaluate(f32 time) const;
    
    // Accessors
    size_t key_count() const { return keys_.size(); }
    const vector_key& get_key(size_t index) const { return keys_[index]; }
    void set_interpolation(curve_interpolation mode) { interpolation_ = mode; }
    
    // Utility
    bool empty() const { return keys_.empty(); }
    void sort_keys();
    
    // Preset curves
    static vector_curve constant(const math::v3& value);
    static vector_curve linear(const math::v3& start, const math::v3& end);
    static vector_curve gravity(f32 strength);
    static vector_curve wind(const math::v3& direction, f32 strength);
    
private:
    std::vector<vector_key> keys_;
    curve_interpolation interpolation_{ curve_interpolation::linear };
    
    size_t find_key_index(f32 time) const;
};

// -----------------------------------------------------------------------------
// Curve Presets Namespace
// Common curve presets for particle effects
// -----------------------------------------------------------------------------

namespace curve_presets {
    // Alpha curves
    float_curve alpha_fade_in();
    float_curve alpha_fade_out();
    float_curve alpha_fade_in_out();
    float_curve alpha_pulse();
    
    // Scale curves
    float_curve scale_grow();
    float_curve scale_shrink();
    float_curve scale_grow_shrink();
    float_curve scale_bounce();
    
    // Speed curves
    float_curve speed_decelerate();
    float_curve speed_accelerate();
    float_curve speed_ease_in_out();
    
    // Color gradients
    color_gradient fire_gradient();
    color_gradient smoke_gradient();
    color_gradient magic_gradient();
    color_gradient water_gradient();
    color_gradient explosion_gradient();
}

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
