#pragma once
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace primal::graphics::lumen {
// SCS1 depth is a float plane, while the GPU pool is addressed in uint words.
// Keep the element/byte conversion here so all four quarters reach the GPU.
inline uint32_t AppendStaticDepthPlane(std::vector<uint32_t>& pool,
                                       std::span<const float> depth) {
    static_assert(sizeof(float) == sizeof(uint32_t));
    const auto offset = static_cast<uint32_t>(pool.size());
    pool.resize(pool.size() + depth.size());
    if (!depth.empty()) std::memcpy(pool.data() + offset, depth.data(), depth.size_bytes());
    return offset;
}
} // namespace primal::graphics::lumen
