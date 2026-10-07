#pragma once

#include "Graphics/RenderGraph/RenderGraphDefinitions.h"

namespace primal::graphics::rendergraph {
    class RenderGraph;
}

namespace primal::graphics::PostProcess {

using namespace rendergraph;

struct BloomPassData {
    RGResourceHandle bloomOutput;
};

// P3（sponza-gap-audit §4.9）: bright-pass 参数——由编译期常数改为显式
// 输入并贯穿到 Bloom.frag 的 UBO。
// 2026-09-16 参考图重标定（用户：参考图直射光靠大 bloom 表达）：旧默认
// threshold 4.0 / energy 0.20 + 单次 σ2 blur 在 Sponza 参考机位几乎不可见
// （天窗高光死白、无辉光）。新默认按 env A/B 实测选定（E=1.0/I=4.0/
// σ×3 = "变体 D"）：光池/天窗出现 ~50/255 级光晕、暗部 ROI 不动、
// 全图 meanY 36.8→39.0（参考 42.7）。合成强度默认同步见
// ToneMappingPass 的 PRIMAL_BLOOM_INTENSITY（默认 4.0）。
struct BloomParams {
    float threshold = 3.0f; // HDR 亮度阈值。低于天空基色下限，直射/天空进亮通
    float knee      = 1.5f; // 平滑过渡半宽（避免阈值硬轮廓）
    float energy    = 1.0f; // 提亮能量乘数（亮通增益）
    // 渐进模糊链（不进 GPU UBO）：pass 数 1=旧单次 σ2；默认 4 级
    // σ={3,4.5,7.5,12}（半分辨率，联合 σ≈40 全res 像素），参考图级大光晕。
    int   blurPasses    = 4;
    float blurSigmaBase = 3.0f;
};

/**
 * @brief Adds a Bloom Pass
 * @details Extracts bright areas, blurs them, and prepares for composition
 */
const BloomPassData& AddBloomPass(RenderGraph& graph, RGResourceHandle inputColor, u32 frameIndex,
                                  const BloomParams& params = {});

void ShutdownBloomPass();

} // namespace primal::graphics::PostProcess
