#pragma once
#include <cstdint>
#include <cstddef>
#include <algorithm>
#include <cmath>

namespace primal::graphics::rhi::card_surface_identity {
inline constexpr float CaptureNear = .05f;
inline constexpr float CaptureFarScale = 1.f;
inline constexpr float CaptureFarBias = .5f;
inline uint64_t Hash(const void* data, size_t bytes, uint64_t h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
// Transform the actual RGBA8 sRGB upload bytes; alpha is untouched. Offline
// material variants and runtime texture edits must share this rounding contract.
inline void ScaleTextureRgb(uint8_t* rgba, size_t pixels, float scale) {
    if (!rgba || scale == 1.f) return;
    uint8_t lut[256];
    for (unsigned i = 0; i < 256; ++i) {
        const float srgb = i / 255.f;
        float linear = srgb <= .04045f ? srgb / 12.92f
            : std::pow((srgb + .055f) / 1.055f, 2.4f);
        linear = std::clamp(linear * scale, 0.f, 1.f);
        const float encoded = linear <= .0031308f ? linear * 12.92f
            : 1.055f * std::pow(linear, 1.f / 2.4f) - .055f;
        lut[i] = static_cast<uint8_t>(std::clamp(encoded * 255.f + .5f, 0.f, 255.f));
    }
    for (size_t p = 0; p < pixels; ++p)
        for (unsigned c = 0; c < 3; ++c) rgba[p * 4 + c] = lut[rgba[p * 4 + c]];
}
// Decoded source pixels, before GPU resampling/mips. Unknown source => 0.
inline uint64_t Texture(uint32_t w, uint32_t h, const void* rgba) {
    if (!rgba || !w || !h) return 0;
    uint64_t d = Hash(&w, sizeof(w)); d = Hash(&h, sizeof(h), d);
    return Hash(rgba, size_t(w) * h * 4u, d);
}
inline uint64_t Material(uint64_t texture, const float tint[3], uint8_t roughness, uint8_t metallic) {
    if (!texture) return 0;
    uint64_t d = Hash(&texture, sizeof(texture));
    d = Hash(tint, sizeof(float) * 3u, d);
    d = Hash(&roughness, 1, d); return Hash(&metallic, 1, d);
}
}
