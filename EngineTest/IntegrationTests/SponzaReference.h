#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>

// Test-scene art direction, shared by Metal and Vulkan. This is deliberately
// opt-in: existing numerical GI regression viewpoints remain reproducible.
namespace sponza_reference {
inline bool Enabled() {
    const char* value = std::getenv("SPONZA_REFERENCE");
    return value && value[0] == '1';
}

inline void Camera(float& x, float& y, float& z, float& pitch, float& yaw) {
    if (!Enabled()) return;
    x = -12.5f; y = 1.7f; z = -0.7f; pitch = 14.0f; yaw = 2.0f;
}

template<class Vector>
inline void ReadRGB(const char* name, Vector& target) {
    if (const char* value = std::getenv(name)) {
        float x, y, z;
        char trailing;
        if (std::sscanf(value, "%f,%f,%f %c", &x, &y, &z, &trailing) == 3 &&
            std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
            x >= 0 && y >= 0 && z >= 0) {
            target.x = x; target.y = y; target.z = z;
        }
    }
}

template<class Lighting>
inline void LightingSettings(Lighting& lighting) {
    if (Enabled()) {
        // Light travels downwards across the courtyard, giving the right
        // arcade a warm key while leaving the entrance in shade.
        lighting.light_direction = {0.15f, -0.8f, 0.6f};
        lighting.light_color = {3.0f, 2.64f, 2.16f, 1.0f};
        // 2026-09-06 GI 对齐 UE 参考（第 8-13 轮标定）：解析探针驱动取
        // 1.5×直射档，同机位实测 mean 40.9 / 深暗 61.0% / 贴白 0.31%，
        // 对齐参考 42.8 / 61.3% / 0.7%。注意是绝对值——同时消除两测试
        // 的历史基座差（Metal 3.0 vs Vulkan 5.0），参考模式下平台同参。
        lighting.ddgi_light_color = {4.5f, 3.96f, 3.24f, 1.0f};
    }
    if (const char* value = std::getenv("PRIMAL_LIGHT_DIR")) {
        float x, y, z;
        char trailing;
        if (std::sscanf(value, "%f,%f,%f %c", &x, &y, &z, &trailing) == 3 &&
            std::isfinite(x) && std::isfinite(y) && std::isfinite(z) &&
            x*x + y*y + z*z > 1e-8f) {
            lighting.light_direction = {x, y, z};
        }
    }
    ReadRGB("PRIMAL_DIRECT_LIGHT", lighting.light_color);
    ReadRGB("PRIMAL_DDGI_LIGHT", lighting.ddgi_light_color);
}
} // namespace sponza_reference
