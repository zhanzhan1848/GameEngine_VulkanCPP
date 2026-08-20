#pragma once

namespace primal::graphics::rhi {

// Live-tunable debug parameters exposed via the WASM HTML sidebar.
// Sliders in shell.html call setDebugParam(idx, val) which routes here.
//
// Indices are stable — JS side uses the same index to address each field:
//   0: directLightBoost    (shader-side DIRECT_LIGHT_BOOST, default 3.0)
//   1: iblStrength         (shader-side iblStrength, default 0.2)
//   2: ddgiIndirectWeight  (shader-side DDGI_INDIRECT_WEIGHT, default 0.45)
//   3: exposure            (shader-side exposure, default 0.1)
//   4: skyColorIntensity   (LumenDDGIPass SkyColor magnitude, default 0.2)
//   5: albedoIntensity     (LumenDDGIPass Albedo magnitude, default 0.4)
//   6: probeHysteresis     (LumenDDGIPass ProbeHysteresis, default 0.08)
// Tuned for desktop Vulkan Sponza (not WASM defaults).
// skyColorIntensity 0.4: warm sky tint (0.85,0.85,1.0) × 0.4 gives soft blue ambient.
// albedoIntensity 0.5: moderate surface reflectance for multi-bounce.
// probeHysteresis 0.2: faster convergence (was 0.08 for WASM perf).
struct DawnDebugParams {
    float directLightBoost   = 3.0f;
    float iblStrength        = 0.2f;
    float ddgiIndirectWeight = 0.45f;
    float exposure           = 0.1f;
    float skyColorIntensity  = 0.4f;
    float albedoIntensity    = 0.5f;
    float probeHysteresis    = 0.2f;
};

} // namespace primal::graphics::rhi
