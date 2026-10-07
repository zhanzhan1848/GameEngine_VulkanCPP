#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::PostProcess {

using namespace rendergraph;

struct ToneMappingPassData {
    RGResourceHandle output;
};

// P0（sun-audit §7）：解析后的有效调色/曝光参数快照——供 provenance 导出
// （[SunContract] → capture.json），数值与 ToneParamsUBO 实际写入值同源。
struct ToneProvenance {
    float exposure_ev;        ///< PRIMAL_TONE_EV（默认 0）
    float base_exposure;      ///< 路径标定基值（passthrough 0.26 / ACES 0.10）
    float exposure;           ///< base × 2^EV = UBO 实际曝光
    bool  adapt_enabled;      ///< PRIMAL_TONE_ADAPT——存在即开（=0 不关！audit P3）
    bool  force_aces;         ///< PRIMAL_TONE_ACES 存在即强制 ACES 路径
    bool  force_passthrough;  ///< PRIMAL_TONE_PASSTHROUGH 存在即强制透传
    float contrast;
    float saturation;
    float vignette;
    float key;                ///< 适应目标（PRIMAL_TONE_KEY）
    float bloom_intensity;    ///< PRIMAL_BLOOM_INTENSITY（默认 4.0）
};

/// 当前进程的有效调色参数（供采集 provenance；不动渲染状态）。
ToneProvenance GetToneProvenance();

/**
 * @brief Adds a Tone Mapping Pass
 * @details Applies tone mapping (ACES, Reinhard, etc.) and gamma correction. 
 *          Can optionally composite Bloom and other effects.
 */
const ToneMappingPassData& AddToneMappingPass(RenderGraph& graph, RGResourceHandle inputHDR, RGResourceHandle bloomTexture = {}, RGResourceHandle aoTexture = {}, RGResourceHandle ssgiTexture = {}, RGResourceHandle velocityTexture = {}, u32 frameIndex = 0);

} // namespace primal::graphics::PostProcess
