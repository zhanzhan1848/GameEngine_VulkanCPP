#pragma once
#include <cstdint>

namespace primal::graphics::lumen {

// Stable SkyColor.w wire values. Captured mode 0 always means the legacy
// directional-radiance reconstruction; a new default must not reinterpret it.
enum class DDGIAnalyticHistoryMode : std::uint32_t {
    LegacyRadiance = 0,
    Disabled = 1,
    LambertDiffuse = 2
};

// Existing NO_HISTORY/IRRADIANCE controls use presence semantics. The new
// legacy diagnostic requires exactly "1"; production needs no env setting.
constexpr DDGIAnalyticHistoryMode ResolveDDGIAnalyticHistoryMode(
    const char* noHistory = nullptr, const char* legacyRadiance = nullptr,
    const char* /*irradianceCompatibilityAlias*/ = nullptr) noexcept {
    if (noHistory) return DDGIAnalyticHistoryMode::Disabled;
    if (legacyRadiance && legacyRadiance[0] == '1' && legacyRadiance[1] == '\0')
        return DDGIAnalyticHistoryMode::LegacyRadiance;
    return DDGIAnalyticHistoryMode::LambertDiffuse;
}

constexpr const char* DDGIAnalyticHistoryOperatorName(DDGIAnalyticHistoryMode mode) noexcept {
    switch (mode) {
    case DDGIAnalyticHistoryMode::LegacyRadiance: return "legacy_directional_radiance";
    case DDGIAnalyticHistoryMode::Disabled: return "disabled";
    case DDGIAnalyticHistoryMode::LambertDiffuse: return "lambert_diffuse_E_over_pi";
    }
    return "unknown";
}

} // namespace primal::graphics::lumen
