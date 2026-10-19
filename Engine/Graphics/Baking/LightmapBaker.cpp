// Lightmap 烘焙器（2026-10-19 计划）。
// M1：契约默认值 + 未实现桩（BakeLightmaps 报错返回，M2 落地 CPU BVH
// 积分器时替换）。

#include "LightmapBaker.h"

namespace primal::graphics::baking {

LightmapBakeSettings DefaultLightmapBakeSettings() {
    LightmapBakeSettings s{};
    s.atlas_size = 2048;
    s.samples_per_texel = 256;
    s.bounces = 3;
    s.texel_density = 16.0f;
    s.denoise = 1;
    s.dilation_px = 8;
    s.output_dir[0] = '\0';
    return s;
}

LightmapBakeResult BakeLightmaps(const BakeScene& scene,
                                 const LightmapBakeSettings& settings,
                                 LightmapProgressCallback cb, void* cb_user) {
    (void)scene;
    (void)settings;
    (void)cb;
    (void)cb_user;
    LightmapBakeResult r;
    r.ok = false;
    r.error = "baker not implemented until M2 (contract frozen in M1)";
    return r;
}

} // namespace primal::graphics::baking
