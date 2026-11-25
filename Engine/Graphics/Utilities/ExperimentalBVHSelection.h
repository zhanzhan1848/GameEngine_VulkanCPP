#pragma once

// Unified selector for the in-memory geometry-BVH experiment family. This does
// not change the engine's default builder or the PBVH cache; it only names
// which candidate builder the experimental scene uses (ExperimentalSAHMesh.h).
//
//   PRIMAL_GEOMETRY_BVH_IMPL = median | sah12 | lbvh | hlbvh
//       explicit "median" forces the experiment off (overrides the legacy knob)
//   PRIMAL_EXPERIMENT_ENV_SAH = 1  (legacy) maps to sah12
//
// When both are set the unified switch wins. The environment is re-read on
// every call: the in-process interleaved harness (ENV_SAH_INTERLEAVED) flips
// PRIMAL_EXPERIMENT_ENV_SAH per block and relies on late re-evaluation.
//
// Compatibility rules elsewhere are unchanged: while any non-median impl is
// selected, the SDF-hybrid / cross-ray-pair / gather geometry experiments keep
// refusing to run (see ExperimentalBVHRequested()).
#include <cstdlib>
#include <cstring>
#include <optional>

namespace primal::graphics::ray_geometry {

enum class BvhImpl { Median, Sah12, Lbvh, Hlbvh };

inline std::optional<BvhImpl> SelectExperimentalBvhImpl() {
    if (const char* unified = std::getenv("PRIMAL_GEOMETRY_BVH_IMPL")) {
        if (std::strcmp(unified, "median") == 0) return BvhImpl::Median;
        if (std::strcmp(unified, "sah12") == 0) return BvhImpl::Sah12;
        if (std::strcmp(unified, "lbvh") == 0) return BvhImpl::Lbvh;
        if (std::strcmp(unified, "hlbvh") == 0) return BvhImpl::Hlbvh;
        // Unknown value: treat as off rather than guessing a builder.
        return BvhImpl::Median;
    }
    const char* legacy = std::getenv("PRIMAL_EXPERIMENT_ENV_SAH");
    if (legacy && std::strcmp(legacy, "1") == 0) return BvhImpl::Sah12;
    return BvhImpl::Median;
}

inline bool ExperimentalBVHRequested() {
    return SelectExperimentalBvhImpl() != BvhImpl::Median;
}

inline const char* BvhImplName(BvhImpl impl) {
    switch (impl) {
        case BvhImpl::Sah12: return "sah12";
        case BvhImpl::Lbvh: return "lbvh";
        case BvhImpl::Hlbvh: return "hlbvh";
        case BvhImpl::Median: break;
    }
    return "median";
}

} // namespace primal::graphics::ray_geometry
