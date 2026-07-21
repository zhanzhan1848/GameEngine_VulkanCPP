#pragma once

namespace primal::graphics::rhi {

// Live-tunable debug parameters exposed via the WASM HTML sidebar.
// Sliders in shell.html call setDebugParam(idx, val) which routes here.
//
// Indices are stable — JS side uses the same index to address each field:
//   0: directLightBoost    (shader-side DIRECT_LIGHT_BOOST, default 2.0)
//   1: iblStrength         (shader-side iblStrength, default 0.2)
//   2: ddgiIndirectWeight  (shader-side DDGI_INDIRECT_WEIGHT, default 1.0)
//   3: exposure            (shader-side exposure, default 1.8)
//   4: skyColorIntensity   (LumenDDGIPass SkyColor magnitude, default 0.6)
//   5: albedoIntensity     (LumenDDGIPass Albedo magnitude, default 0.6)
//   6: probeHysteresis     (LumenDDGIPass ProbeHysteresis, default 0.08)
struct DawnDebugParams {
    float directLightBoost   = 2.0f;
    float iblStrength        = 0.2f;
    float ddgiIndirectWeight = 1.0f;
    float exposure           = 1.8f;
    float skyColorIntensity  = 0.6f;
    float albedoIntensity    = 0.6f;
    float probeHysteresis    = 0.08f;
};

} // namespace primal::graphics::rhi
