#pragma once

// Phase 1 collision module — Unity-VHACD convex decomposition.
//
// Decomposes a (possibly concave) ProcessableMesh into a set of convex
// hulls suitable for runtime rigid-body physics. Default-disabled in the
// Phase 1 pipeline (engine has no consumer yet); Phase 2 will wire hulls
// into the runtime collider system.
//
// Hulls are returned as standalone arrays — they do NOT enter
// scene_data.buffer (MSHL/SDF magic stays untouched). Phase 1 also does
// not export a C ABI for this module; callers go through the pipeline
// (M8) or ContentToolsCLI extension.

#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"

namespace primal::tools::collision {

// One convex hull produced by VHACD.
struct Hull {
    utl::vector<math::v3>  vertices;       // world-space hull verts
    utl::vector<u32>       indices;        // triangle list (3 per face)
    f32                    volume{0.f};    // hull volume (VHACD-computed)
    math::v3               center{0.f, 0.f, 0.f};   // hull centroid
};

struct Params {
    // Maximum number of convex hulls VHACD may produce. Stop early when
    // reached.
    u32     max_hulls{8};

    // Voxel grid resolution used internally. Higher = better quality but
    // slower. 100k is VHACD's default; 10k is fine for runtime colliders.
    // When auto_resolution is true (default), voxel_resolution is treated as
    // an upper bound and scaled down based on triangle count to avoid the
    // VHACD re-voxelization retry storm on large meshes.
    u32     voxel_resolution{100000};
    bool    auto_resolution{true};

    // Maximum concavity allowed per hull (0..1). Lower = more aggressive
    // splitting = more hulls. VHACD default 0.001.
    f32     max_concavity{0.001f};

    // Cap on verts per output hull. 64 is a typical PhysX/Jolt limit.
    u32     max_vertices_per_hull{64};

    // PCA (principal component analysis) preprocessing. Slightly improves
    // quality on certain meshes; usually not worth the cost.
    bool    pca{false};

    // Voxel-space downsampling. Higher = coarser internal voxel grid.
    u32     plane_downsampling{4};
    u32     convexhull_downsampling{4};
};

// Returns 1..max_hulls convex hulls covering `m`. Empty input → empty
// output + warning. VHACD failure → empty output + error.
utl::vector<Hull> Run(const ProcessableMesh& m, const Params& params,
                      utl::vector<ErrorReport>& errors);

}  // namespace primal::tools::collision
