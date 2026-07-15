#pragma once
#include "Engine/Graphics/Lumen/StaticProbe/StaticProbeVolume.h"
#include "Engine/Graphics/Utilities/BVH.hpp"

namespace primal::graphics::lumen {

struct ProbeBakingScene {
    const math::v3* vertices{nullptr};
    const u32* indices{nullptr};
    u32 vertex_count{0};
    u32 index_count{0};
    math::v3 light_direction{0.0f, -1.0f, 0.0f};
    math::v3 light_color{1.0f};
    float light_intensity{1.0f};
    math::v3 sky_color{0.5f, 0.7f, 1.0f};
    math::v3 albedo{0.5f, 0.5f, 0.5f};  // TODO: per-vertex albedo from RHIMeshAsset
};

struct ProbeBakingParams {
    u32 rays_per_probe{512};
    u32 bounce_count{3};
    float convergence_threshold{0.01f};
    float ray_max_distance{50.0f};
};

// Progress callback invoked between probe chunks during Bake().
//   user_data: opaque pointer passed through untouched
//   phase: "radiance" or "visibility" (which ParallelFor we're inside)
//   done/total: probes completed in current phase
// Used by WASM builds to pump the browser event loop (emscripten_sleep)
// inside the callback so the page doesn't freeze during the multi-second bake.
// Native builds pass nullptr and see no behavior change.
using BakeProgressFn = void (*)(void* user_data, const char* phase, u32 done, u32 total);

class StaticProbeBaker {
public:
    static bool Bake(StaticProbeVolume& volume,
                     const ProbeBakingScene& scene,
                     const ProbeBakingParams& params = {},
                     BakeProgressFn on_progress = nullptr,
                     void* progress_user_data = nullptr);

private:
    static void BakeProbeRadiance(StaticProbeVolume& volume,
                                  const utl::BVH& bvh,
                                  const ProbeBakingScene& scene,
                                  const ProbeBakingParams& params,
                                  u32 probe_index);

    static void BakeVisibility(StaticProbeVolume& volume,
                               const utl::BVH& bvh,
                               const ProbeBakingScene& scene,
                               const ProbeBakingParams& params,
                               u32 probe_index);

    static math::v3 ProbeWorldPos(u32 probe_index, u32 dim_x, u32 dim_y, u32 dim_z,
                                   math::v3 origin, float spacing);
    static math::v3 OctahedralDirection(u32 oct_index);
};

} // namespace primal::graphics::lumen
