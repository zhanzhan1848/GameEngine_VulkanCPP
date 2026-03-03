#include "ParticleCurve.h"

#ifndef DISABLE_PARTICLE_SYSTEM

#include <cmath>

namespace primal::particles {

// -----------------------------------------------------------------------------
// Float Curve Implementation
// -----------------------------------------------------------------------------

float_curve::float_curve(std::initializer_list<curve_keyframe<f32>> keys) {
    for (const auto& key : keys) {
        keyframes_.push_back(key);
    }
    sort_keyframes();
}

void float_curve::add_keyframe(f32 time, f32 value) {
    curve_keyframe<f32> key{ time, value };
    add_keyframe(key);
}

void float_curve::add_keyframe(f32 time, f32 value, f32 in_tangent, f32 out_tangent) {
    curve_keyframe<f32> key{ time, value, in_tangent, out_tangent };
    add_keyframe(key);
}

void float_curve::add_keyframe(const curve_keyframe<f32>& key) {
    // Insert sorted by time
    auto it = std::lower_bound(keyframes_.begin(), keyframes_.end(), key,
        [](const auto& a, const auto& b) { return a.time < b.time; });
    keyframes_.insert(it, key);
}

void float_curve::remove_keyframe(size_t index) {
    if (index < keyframes_.size()) {
        keyframes_.erase(keyframes_.begin() + index);
    }
}

void float_curve::clear() {
    keyframes_.clear();
}

void float_curve::sort_keyframes() {
    std::sort(keyframes_.begin(), keyframes_.end(),
        [](const auto& a, const auto& b) { return a.time < b.time; });
}

f32 float_curve::evaluate(f32 time) const {
    if (keyframes_.empty()) return 1.0f;
    if (keyframes_.size() == 1) return keyframes_[0].value;
    
    // Clamp time to [0, 1]
    time = std::clamp(time, 0.0f, 1.0f);
    
    switch (interpolation_) {
        case curve_interpolation::constant:
            return evaluate_constant(time);
        case curve_interpolation::linear:
            return evaluate_linear(time);
        case curve_interpolation::smooth:
            return evaluate_smooth(time);
        default:
            return evaluate_linear(time);
    }
}

size_t float_curve::find_keyframe_index(f32 time) const {
    for (size_t i = 0; i < keyframes_.size() - 1; ++i) {
        if (time < keyframes_[i + 1].time) {
            return i;
        }
    }
    return keyframes_.size() - 1;
}

f32 float_curve::evaluate_constant(f32 time) const {
    return keyframes_[find_keyframe_index(time)].value;
}

f32 float_curve::evaluate_linear(f32 time) const {
    size_t idx = find_keyframe_index(time);
    
    if (idx >= keyframes_.size() - 1) {
        return keyframes_.back().value;
    }
    
    const auto& k0 = keyframes_[idx];
    const auto& k1 = keyframes_[idx + 1];
    
    f32 t = (time - k0.time) / (k1.time - k0.time);
    return k0.value + t * (k1.value - k0.value);
}

f32 float_curve::evaluate_smooth(f32 time) const {
    size_t idx = find_keyframe_index(time);
    
    if (idx >= keyframes_.size() - 1) {
        return keyframes_.back().value;
    }
    
    const auto& k0 = keyframes_[idx];
    const auto& k1 = keyframes_[idx + 1];
    
    f32 t = (time - k0.time) / (k1.time - k0.time);
    
    // Hermite interpolation
    f32 t2 = t * t;
    f32 t3 = t2 * t;
    
    f32 h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    f32 h10 = t3 - 2.0f * t2 + t;
    f32 h01 = -2.0f * t3 + 3.0f * t2;
    f32 h11 = t3 - t2;
    
    f32 dt = k1.time - k0.time;
    return h00 * k0.value + h10 * k0.out_tangent * dt +
           h01 * k1.value + h11 * k1.in_tangent * dt;
}

// Preset curves
float_curve float_curve::constant(f32 value) {
    float_curve curve;
    curve.add_keyframe(0.0f, value);
    return curve;
}

float_curve float_curve::linear(f32 start, f32 end) {
    float_curve curve;
    curve.add_keyframe(0.0f, start);
    curve.add_keyframe(1.0f, end);
    return curve;
}

float_curve float_curve::fade_in() {
    float_curve curve;
    curve.add_keyframe(0.0f, 0.0f);
    curve.add_keyframe(1.0f, 1.0f);
    return curve;
}

float_curve float_curve::fade_out() {
    float_curve curve;
    curve.add_keyframe(0.0f, 1.0f);
    curve.add_keyframe(1.0f, 0.0f);
    return curve;
}

float_curve float_curve::fade_in_out() {
    float_curve curve;
    curve.add_keyframe(0.0f, 0.0f);
    curve.add_keyframe(0.2f, 1.0f);
    curve.add_keyframe(0.8f, 1.0f);
    curve.add_keyframe(1.0f, 0.0f);
    return curve;
}

float_curve float_curve::ease_in() {
    float_curve curve;
    curve.add_keyframe(0.0f, 0.0f, 0.0f, 0.0f);
    curve.add_keyframe(1.0f, 1.0f, 1.0f, 1.0f);
    curve.set_interpolation(curve_interpolation::smooth);
    return curve;
}

float_curve float_curve::ease_out() {
    float_curve curve;
    curve.add_keyframe(0.0f, 0.0f, 1.0f, 1.0f);
    curve.add_keyframe(1.0f, 1.0f, 0.0f, 0.0f);
    curve.set_interpolation(curve_interpolation::smooth);
    return curve;
}

float_curve float_curve::ease_in_out() {
    float_curve curve;
    curve.add_keyframe(0.0f, 0.0f, 0.0f, 0.0f);
    curve.add_keyframe(0.5f, 0.5f, 1.0f, 1.0f);
    curve.add_keyframe(1.0f, 1.0f, 0.0f, 0.0f);
    curve.set_interpolation(curve_interpolation::smooth);
    return curve;
}

float_curve float_curve::bounce() {
    float_curve curve;
    curve.add_keyframe(0.0f, 0.0f);
    curve.add_keyframe(0.3f, 1.0f);
    curve.add_keyframe(0.5f, 0.5f);
    curve.add_keyframe(0.7f, 0.75f);
    curve.add_keyframe(0.85f, 0.6f);
    curve.add_keyframe(1.0f, 0.65f);
    return curve;
}

float_curve float_curve::elastic() {
    float_curve curve;
    curve.add_keyframe(0.0f, 0.0f);
    curve.add_keyframe(0.15f, 1.2f);
    curve.add_keyframe(0.3f, 0.8f);
    curve.add_keyframe(0.45f, 1.1f);
    curve.add_keyframe(0.6f, 0.9f);
    curve.add_keyframe(0.75f, 1.05f);
    curve.add_keyframe(0.9f, 0.98f);
    curve.add_keyframe(1.0f, 1.0f);
    return curve;
}

// -----------------------------------------------------------------------------
// Color Gradient Implementation
// -----------------------------------------------------------------------------

color_gradient::color_gradient(std::initializer_list<color_key> keys) {
    for (const auto& key : keys) {
        keys_.push_back(key);
    }
    sort_keys();
}

void color_gradient::add_color(f32 time, const math::v4& color) {
    color_key key{ time, color };
    auto it = std::lower_bound(keys_.begin(), keys_.end(), key,
        [](const auto& a, const auto& b) { return a.time < b.time; });
    keys_.insert(it, key);
}

void color_gradient::add_color(f32 time, f32 r, f32 g, f32 b, f32 a) {
    add_color(time, math::v4{ r, g, b, a });
}

void color_gradient::remove_key(size_t index) {
    if (index < keys_.size()) {
        keys_.erase(keys_.begin() + index);
    }
}

void color_gradient::clear() {
    keys_.clear();
}

void color_gradient::sort_keys() {
    std::sort(keys_.begin(), keys_.end(),
        [](const auto& a, const auto& b) { return a.time < b.time; });
}

math::v4 color_gradient::evaluate(f32 time) const {
    if (keys_.empty()) return { 1.0f, 1.0f, 1.0f, 1.0f };
    if (keys_.size() == 1) return keys_[0].color;
    
    time = std::clamp(time, 0.0f, 1.0f);
    
    size_t idx = find_key_index(time);
    
    if (idx >= keys_.size() - 1) {
        return keys_.back().color;
    }
    
    const auto& c0 = keys_[idx];
    const auto& c1 = keys_[idx + 1];
    
    f32 t = (time - c0.time) / (c1.time - c0.time);
    
    return {
        c0.color.x + t * (c1.color.x - c0.color.x),
        c0.color.y + t * (c1.color.y - c0.color.y),
        c0.color.z + t * (c1.color.z - c0.color.z),
        c0.color.w + t * (c1.color.w - c0.color.w)
    };
}

size_t color_gradient::find_key_index(f32 time) const {
    for (size_t i = 0; i < keys_.size() - 1; ++i) {
        if (time < keys_[i + 1].time) {
            return i;
        }
    }
    return keys_.size() - 1;
}

// Preset gradients
color_gradient color_gradient::solid(const math::v4& color) {
    color_gradient grad;
    grad.add_color(0.0f, color);
    return grad;
}

color_gradient color_gradient::fade(const math::v4& color, f32 start_alpha, f32 end_alpha) {
    color_gradient grad;
    grad.add_color(0.0f, math::v4{ color.x, color.y, color.z, start_alpha });
    grad.add_color(1.0f, math::v4{ color.x, color.y, color.z, end_alpha });
    return grad;
}

color_gradient color_gradient::fire() {
    color_gradient grad;
    grad.add_color(0.0f, math::v4{ 1.0f, 1.0f, 0.8f, 1.0f });    // White-yellow
    grad.add_color(0.2f, math::v4{ 1.0f, 0.9f, 0.3f, 0.95f });   // Yellow
    grad.add_color(0.4f, math::v4{ 1.0f, 0.6f, 0.1f, 0.9f });     // Orange
    grad.add_color(0.7f, math::v4{ 1.0f, 0.2f, 0.05f, 0.6f });    // Red-orange
    grad.add_color(1.0f, math::v4{ 0.2f, 0.05f, 0.02f, 0.0f });   // Dark, transparent
    return grad;
}

color_gradient color_gradient::smoke() {
    color_gradient grad;
    grad.add_color(0.0f, math::v4{ 0.9f, 0.9f, 0.9f, 0.7f });
    grad.add_color(0.5f, math::v4{ 0.6f, 0.6f, 0.6f, 0.4f });
    grad.add_color(1.0f, math::v4{ 0.3f, 0.3f, 0.3f, 0.0f });
    return grad;
}

color_gradient color_gradient::magic() {
    color_gradient grad;
    grad.add_color(0.0f, math::v4{ 0.5f, 0.2f, 1.0f, 1.0f });     // Purple
    grad.add_color(0.33f, math::v4{ 0.2f, 0.5f, 1.0f, 0.9f });    // Blue
    grad.add_color(0.66f, math::v4{ 0.8f, 0.2f, 1.0f, 0.7f });    // Pink
    grad.add_color(1.0f, math::v4{ 0.2f, 0.8f, 1.0f, 0.0f });     // Cyan, transparent
    return grad;
}

color_gradient color_gradient::explosion() {
    color_gradient grad;
    grad.add_color(0.0f, math::v4{ 1.0f, 1.0f, 1.0f, 1.0f });      // White core
    grad.add_color(0.1f, math::v4{ 1.0f, 0.95f, 0.5f, 1.0f });    // Yellow
    grad.add_color(0.3f, math::v4{ 1.0f, 0.5f, 0.1f, 0.9f });     // Orange
    grad.add_color(0.6f, math::v4{ 0.8f, 0.2f, 0.1f, 0.5f });     // Red
    grad.add_color(1.0f, math::v4{ 0.1f, 0.1f, 0.1f, 0.0f });     // Black smoke
    return grad;
}

color_gradient color_gradient::rainbow() {
    color_gradient grad;
    grad.add_color(0.0f, math::v4{ 1.0f, 0.0f, 0.0f, 1.0f });      // Red
    grad.add_color(0.17f, math::v4{ 1.0f, 0.5f, 0.0f, 1.0f });    // Orange
    grad.add_color(0.33f, math::v4{ 1.0f, 1.0f, 0.0f, 1.0f });     // Yellow
    grad.add_color(0.5f, math::v4{ 0.0f, 1.0f, 0.0f, 1.0f });      // Green
    grad.add_color(0.67f, math::v4{ 0.0f, 0.5f, 1.0f, 1.0f });     // Blue
    grad.add_color(0.83f, math::v4{ 0.3f, 0.0f, 1.0f, 1.0f });     // Indigo
    grad.add_color(1.0f, math::v4{ 0.5f, 0.0f, 1.0f, 1.0f });      // Violet
    return grad;
}

// -----------------------------------------------------------------------------
// Vector Curve Implementation
// -----------------------------------------------------------------------------
vector_curve::vector_curve(std::initializer_list<vector_key> keys) {
    for (const auto& key : keys) {
        keys_.push_back(key);
    }
    sort_keys();
}

void vector_curve::add_key(f32 time, const math::v3& value) {
    vector_key key{ time, value };
    auto it = std::lower_bound(keys_.begin(), keys_.end(), key,
        [](const auto& a, const auto& b) { return a.time < b.time; });
    keys_.insert(it, key);
}

void vector_curve::add_key(f32 time, f32 x, f32 y, f32 z) {
    add_key(time, math::v3{ x, y, z });
}

void vector_curve::remove_key(size_t index) {
    if (index < keys_.size()) {
        keys_.erase(keys_.begin() + index);
    }
}

void vector_curve::clear() {
    keys_.clear();
}

void vector_curve::sort_keys() {
    std::sort(keys_.begin(), keys_.end(),
        [](const auto& a, const auto& b) { return a.time < b.time; });
}

math::v3 vector_curve::evaluate(f32 time) const {
    if (keys_.empty()) return { 0.0f, 0.0f, 0.0f };
    if (keys_.size() == 1) return keys_[0].value;
    
    time = std::clamp(time, 0.0f, 1.0f);
    
    size_t idx = 0;
    for (size_t i = 0; i < keys_.size() - 1; ++i) {
        if (time < keys_[i + 1].time) {
            idx = i;
            break;
        }
        idx = i;
    }
    
    if (idx >= keys_.size() - 1) {
        return keys_.back().value;
    }
    
    const auto& k0 = keys_[idx];
    const auto& k1 = keys_[idx + 1];
    
    f32 t = (time - k0.time) / (k1.time - k0.time);
    
    return {
        k0.value.x + t * (k1.value.x - k0.value.x),
        k0.value.y + t * (k1.value.y - k0.value.y),
        k0.value.z + t * (k1.value.z - k0.value.z)
    };
}

// Preset vector curves
vector_curve vector_curve::constant(const math::v3& value) {
    vector_curve curve;
    curve.add_key(0.0f, value);
    return curve;
}

vector_curve vector_curve::linear(const math::v3& start, const math::v3& end) {
    vector_curve curve;
    curve.add_key(0.0f, start);
    curve.add_key(1.0f, end);
    return curve;
}

vector_curve vector_curve::gravity(f32 strength) {
    vector_curve curve;
    curve.add_key(0.0f, math::v3{ 0.0f, 0.0f, 0.0f });
    curve.add_key(1.0f, math::v3{ 0.0f, -strength, 0.0f });
    return curve;
}

vector_curve vector_curve::wind(const math::v3& direction, f32 strength) {
    math::v3 normalized = direction;
    f32 len = std::sqrt(direction.x * direction.x + 
                        direction.y * direction.y + 
                        direction.z * direction.z);
    if (len > 0.001f) {
        normalized.x /= len;
        normalized.y /= len;
        normalized.z /= len;
    }
    
    vector_curve curve;
    curve.add_key(0.0f, math::v3{ 0.0f, 0.0f, 0.0f });
    curve.add_key(1.0f, math::v3{
        normalized.x * strength,
        normalized.y * strength,
        normalized.z * strength
    });
    return curve;
}

// -----------------------------------------------------------------------------
// Curve Presets Namespace Implementation
// -----------------------------------------------------------------------------

namespace curve_presets {

float_curve alpha_fade_in() {
    return float_curve::fade_in();
}

float_curve alpha_fade_out() {
    return float_curve::fade_out();
}

float_curve alpha_fade_in_out() {
    return float_curve::fade_in_out();
}

float_curve alpha_pulse() {
    float_curve curve;
    curve.add_keyframe(0.0f, 0.0f);
    curve.add_keyframe(0.3f, 1.0f);
    curve.add_keyframe(0.5f, 0.8f);
    curve.add_keyframe(0.7f, 1.0f);
    curve.add_keyframe(1.0f, 0.0f);
    return curve;
}

float_curve scale_grow() {
    float_curve curve;
    curve.add_keyframe(0.0f, 0.1f);
    curve.add_keyframe(1.0f, 2.0f);
    return curve;
}

float_curve scale_shrink() {
    float_curve curve;
    curve.add_keyframe(0.0f, 2.0f);
    curve.add_keyframe(1.0f, 0.1f);
    return curve;
}

float_curve scale_grow_shrink() {
    float_curve curve;
    curve.add_keyframe(0.0f, 0.2f);
    curve.add_keyframe(0.3f, 1.5f);
    curve.add_keyframe(1.0f, 0.1f);
    return curve;
}

float_curve scale_bounce() {
    return float_curve::bounce();
}

float_curve speed_decelerate() {
    float_curve curve;
    curve.add_keyframe(0.0f, 1.0f);
    curve.add_keyframe(1.0f, 0.1f);
    return curve;
}

float_curve speed_accelerate() {
    float_curve curve;
    curve.add_keyframe(0.0f, 0.1f);
    curve.add_keyframe(1.0f, 1.0f);
    return curve;
}

float_curve speed_ease_in_out() {
    return float_curve::ease_in_out();
}

color_gradient fire_gradient() {
    return color_gradient::fire();
}

color_gradient smoke_gradient() {
    return color_gradient::smoke();
}

color_gradient magic_gradient() {
    return color_gradient::magic();
}

color_gradient water_gradient() {
    color_gradient grad;
    grad.add_color(0.0f, math::v4{ 0.7f, 0.9f, 1.0f, 0.8f });
    grad.add_color(0.5f, math::v4{ 0.3f, 0.6f, 0.9f, 0.5f });
    grad.add_color(1.0f, math::v4{ 0.1f, 0.3f, 0.7f, 0.0f });
    return grad;
}

color_gradient explosion_gradient() {
    return color_gradient::explosion();
}

} // namespace curve_presets

} // namespace primal::particles

#endif // !DISABLE_PARTICLE_SYSTEM
