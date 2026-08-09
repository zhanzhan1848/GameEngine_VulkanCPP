#pragma once

// Phase 1 derive_attributes module.
//
// Recomputes normals and tangents on a ProcessableMesh. Useful when upstream
// modules (subdivide / remesh) leave auxiliary attributes empty or invalid.
//
// NormalMode::Smooth / AreaWeighted / AngleWeighted keep positions.size()
// unchanged (each vertex gets one averaged normal). NormalMode::Faceted uses
// an angle threshold to detect hard edges and duplicates vertices so each
// "smooth group" gets its own vertex slot — this can grow positions.size()
// up to indices.size() in the fully-faceted (180°) case.
//
// TangentMode::MikkTSpace is the de-facto standard (matches XNormal /
// Substance / Unity / UE baked normal maps). It needs at least one UV set
// with Texture purpose; if absent the module degrades to AreaWeighted
// tangent with a warning. TangentMode::None clears io.tangents.
//
// Module composition note: this module does NOT depend on subdivide or
// uvatlas. AssetPipeline::run_in_place_modules injects an automatic
// derive() call after subdivide (when cfg.auto_derive_after_subdivide) —
// that orchestration lives in the pipeline layer, not here.

#include "ToolsCommon.h"
#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"

namespace primal::tools::derive {

enum class NormalMode : u8 {
    Faceted       = 0,   // hard-edge threshold, duplicates vertices
    Smooth        = 1,   // unweighted average of adjacent face normals
    AreaWeighted  = 2,   // face-area-weighted average (default, robust)
    AngleWeighted = 3,   // vertex-angle-weighted average (most accurate)
};

enum class TangentMode : u8 {
    None          = 0,
    AreaWeighted  = 1,   // classic Gram-Schmidt from UV gradients
    MikkTSpace    = 2,   // vendor mikktspace.c (default)
};

struct Params {
    NormalMode  normal_mode            {NormalMode::AreaWeighted};
    TangentMode tangent_mode           {TangentMode::MikkTSpace};
    f32         faceted_angle_degrees  {60.f};  // only used by Faceted mode
};

// Recompute io.normals (unless TangentMode::None alone is requested) and
// io.tangents in-place. Faceted normal mode may grow positions/normals/
// tangents/colors and every uv_sets[].coords by duplicating vertices on
// hard edges. Indices are rewritten to reference the new vertex pool.
//
// Empty mesh → warning, no-op, returns false.
// MikkTSpace without any Texture-purpose UVSet → warning + falls back to
// AreaWeighted tangent for that mesh.
bool Run(ProcessableMesh& io, const Params& params,
         utl::vector<ErrorReport>& errors);

}  // namespace primal::tools::derive
