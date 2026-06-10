#pragma once
#include "CommonHeaders.h"

namespace primal {

// Tag types for template-based component access.
// Usage: entity.Has<component::Transform>(), entity.Get<component::Mesh>(), etc.
namespace component {
    struct Transform {};
    struct Script {};
    struct Mesh {};
    struct Particle {};
    struct Cluster {};
    struct Pipeline {};
    struct CommandBuffer {};
    struct Geometry {};
}

enum class component_bit : u8 {
    Transform = 0,
    Script = 1,
    Mesh = 2,
    Particle = 3,
    Cluster = 4,
    Pipeline = 5,
    CommandBuffer = 6,
    Geometry = 7,
    Count
};

using component_mask = u64;

constexpr component_mask bit_mask(component_bit b) {
    return component_mask{1} << static_cast<u8>(b);
}

template<typename T>
constexpr component_bit component_bit_of() {
    if constexpr (std::is_same_v<T, component::Transform>)          return component_bit::Transform;
    else if constexpr (std::is_same_v<T, component::Script>)       return component_bit::Script;
    else if constexpr (std::is_same_v<T, component::Mesh>)         return component_bit::Mesh;
    else if constexpr (std::is_same_v<T, component::Particle>)     return component_bit::Particle;
    else if constexpr (std::is_same_v<T, component::Cluster>)      return component_bit::Cluster;
    else if constexpr (std::is_same_v<T, component::Pipeline>)     return component_bit::Pipeline;
    else if constexpr (std::is_same_v<T, component::CommandBuffer>) return component_bit::CommandBuffer;
    else if constexpr (std::is_same_v<T, component::Geometry>)      return component_bit::Geometry;
    else static_assert(!std::is_same_v<T, T>, "Unknown component type");
}

} // namespace primal

// Primary template for component_init_info<T> — maps tag type to init_info type.
// Specializations are in GameEntity_impl.h (after all component headers are available).
namespace primal {
template<typename T> struct component_init_info;
} // namespace primal
