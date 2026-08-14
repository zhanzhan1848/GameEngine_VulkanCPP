#pragma once

// Phase 1 uvatlas module — wraps xatlas to generate UV channels for
// different runtime purposes (Texture / Lightmap / Detail).
//
// Multiple UV sets can coexist on a single ProcessableMesh — each Run() call
// produces one UV channel tagged with `target_purpose`. Calling Run() once
// per purpose yields a mesh that carries Texture UV (albedo/normal/ORM),
// Lightmap UV (strict unique-pack), and Detail UV (tiling) simultaneously.
//
// xatlas re-indexes vertices (inserting seam verts where charts split). The
// implementation syncs positions, normals, tangents, colors, AND every
// existing uv_sets entry through xatlas's `xref` permutation so prior
// channels stay consistent with the new topology.
//
// Layout contract preserved through Run():
//   - positions.size()  == new vertex count (may be > input)
//   - indices.size()    == xatlas output index count (triangle list)
//   - normals/tangents/colors/uv_sets[*].coords either empty or
//     positions.size() — picked up from input as-is.

#include "common/ProcessableMesh.h"
#include "common/ErrorReport.h"

namespace primal::tools::uvatlas {

// Generator behavior. Defaults are reasonable for the common Texture path;
// Lightmap callers should set strict_unique_pack = true (or use the
// LightmapDefaults helper below).
struct Params {
    // Which UVSetPurpose to tag the generated channel with. Also selects
    // default chart/pack behavior — see Run() for details.
    UVSetPurpose    target_purpose{UVSetPurpose::Texture};

    // Max charts allowed. 0 = unlimited. Lower values force larger charts
    // (longer seams but fewer atlas islands).
    u32             max_chart_count{0};

    // Texel gutter between charts. Larger = less bleeding but more wasted
    // atlas space. Lightmap typically 4; Texture 2 is fine.
    f32             gutter{4.0f};

    // Chart + pack quality knobs. When true, xatlas rotates charts to axis
    // and to each other for tighter packing. Slower but better utilization.
    bool            stretch_optimization{true};

    // When true, uses brute-force packer ( PackOptions.bruteForce ) and
    // consistent winding enforcement. Required for Lightmap UVs where any
    // overlap or winding flip breaks the runtime integrator.
    bool            strict_unique_pack{false};

    // Atlas resolution hint. 0 = let xatlas estimate from texelsPerUnit.
    // If both are 0, xatlas targets ~1024x1024.
    u32             resolution_hint{0};

    // World-space texel density. 0 = estimate from resolution_hint.
    f32             texels_per_unit{0.0f};

    // If true and a Texture-purpose UVSet exists on input, pass it to
    // xatlas as a chart hint ( ChartOptions.useInputMeshUvs ). Artist UVs
    // are preserved as chart boundaries; xatlas still reparameterizes
    // within each chart. Default true for Texture purpose, ignored for
    // Lightmap/Detail.
    bool            use_input_uvs_as_hint{true};
};

// Convenience presets.
inline Params ForTexture() {
    Params p;
    p.target_purpose        = UVSetPurpose::Texture;
    p.gutter                = 2.0f;
    p.stretch_optimization  = true;
    p.strict_unique_pack    = false;
    p.use_input_uvs_as_hint = true;
    return p;
}
inline Params ForLightmap() {
    Params p;
    p.target_purpose        = UVSetPurpose::Lightmap;
    p.gutter                = 4.0f;
    p.stretch_optimization  = true;
    p.strict_unique_pack    = true;
    p.use_input_uvs_as_hint = false;
    return p;
}
inline Params ForDetail() {
    Params p;
    p.target_purpose        = UVSetPurpose::Detail;
    p.gutter                = 2.0f;
    p.stretch_optimization  = true;
    p.strict_unique_pack    = false;
    p.use_input_uvs_as_hint = true;
    return p;
}

// Run xatlas on `io`. On success:
//   - io.positions/indices/normals/tangents/colors are rewritten to match
//     xatlas's output topology (positions may grow when seam verts are added).
//   - Every existing uv_sets entry has its coords rewritten through xatlas's
//     xref map so they remain consistent with the new vertex ordering.
//   - A new UVSet (or the first matching-purpose one) is populated with the
//     generated UVs, normalized to [0,1] by atlas width/height.
// On failure: io is left unchanged and one or more ErrorReports are pushed.
bool Run(ProcessableMesh& io, const Params& params,
         utl::vector<ErrorReport>& errors);

}  // namespace primal::tools::uvatlas
