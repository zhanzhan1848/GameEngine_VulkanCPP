#pragma once

// Intermediate representation (IR) for the Phase 1 AI asset pipeline.
//
// ProcessableMesh replaces the legacy `mesh` struct (Geometry.h:118) as the
// pipeline-facing data carrier. The legacy struct has a dual role — it's both
// the importer-filled IR AND the packed-output container — which leads to
// drift between SoA buffers (positions/normals/uv_sets) and the AoS
// `vertices` array used during packing. Phase 1 splits these concerns:
//   - ProcessableMesh holds only SoA IR data.
//   - SceneBlobWriter (M8) is the sole consumer that produces the binary
//     `scene_data.buffer` layout consumed by 4 downstream readers.
//
// Multi-UV note: scene_data.buffer's elements::* structs each carry at most
// ONE uv channel (see Docs/2026-08-07-contenttools-elements-uv-audit.md).
// ProcessableMesh.uv_sets can carry arbitrarily many UVSets; the
// UVSetPurpose tag tells SceneBlobWriter which set to serialize (Texture by
// default). Lightmap / Detail UVs stay in IR for runtime-side consumption.

#include "ToolsCommon.h"
#include "Geometry.h"  // reuses primal::tools::material

namespace primal::tools {

// Tag identifying a UV set's intended use. Drives uvatlas generation strategy
// (M6) and SceneBlobWriter output slot selection (M8).
enum class UVSetPurpose : u8 {
    Texture    = 0,    // albedo / normal / ORM shared UV — written to scene_data.buffer.
    Lightmap   = 1,    // strict unique-pack UV; stays in IR (Phase 2 sidecar target).
    Detail     = 2,    // usually Texture × tile_count scale; runtime material can also fold this.
    Custom     = 255,
};

// One UV channel. Invariant: coords.size() == 0 || coords.size() == positions.size().
// uv_sets may contain multiple entries with the same purpose; SceneBlobWriter
// uses the first Texture-purpose set as the blob source.
struct UVSet {
    UVSetPurpose                purpose{UVSetPurpose::Texture};
    utl::vector<math::v2>       coords;
};

// Per-mesh pipeline IR. Phase 1 modules read/modify this in-place.
//
// Layout invariants (enforced by MeshConverters at IR boundaries):
//   - positions.size()  > 0
//   - indices.size()    % 3 == 0   (triangle list)
//   - normals.size()    == 0 || normals.size()  == positions.size()
//   - tangents.size()   == 0 || tangents.size() == positions.size()
//   - colors.size()     == 0 || colors.size()   == positions.size()
//   - for each UVSet s: s.coords.size() == 0 || s.coords.size() == positions.size()
struct ProcessableMesh {
    std::string                 name;
    utl::vector<math::v3>       positions;
    utl::vector<u32>            indices;         // triangle list, raw u32 indices
    utl::vector<math::v3>       normals;
    utl::vector<math::v4>       tangents;
    utl::vector<UVSet>          uv_sets;
    utl::vector<math::v3>       colors;
    u32                         material_idx{u32_invalid_id};
    // Dirty flags: set by geometry-mutating modules (repair/remesh/subdivide),
    // cleared by derive. When true, the corresponding attribute channel no
    // longer matches the current positions/indices and must be recomputed.
    bool                        normals_dirty{false};
    bool                        tangents_dirty{false};
};

// One level of detail. `screen_threshold` is the engine's LOD switch trigger
// (mesh switches to the next lower-detail LOD when projected screen size
// drops below this value). `meshes` holds the material-split submesh set at
// this LOD level — Phase 1 does not auto-split by material; the IR simply
// allows the lod module (M7) to preserve an existing split.
struct ProcessableLod {
    f32                             screen_threshold{0.f};
    utl::vector<ProcessableMesh>    meshes;
};

// Top-level pipeline IR. Phase 1 supports one implicit lod_group per scene
// (the `lods` array IS the LOD chain). Multi-lod_group composition is Phase 2.
// `materials` reuses the legacy Geometry.h::material type so existing
// importer output can flow through without conversion.
struct ProcessableScene {
    std::string                 name;
    utl::vector<ProcessableLod> lods;
    utl::vector<material>       materials;
};

// ---- Helpers -------------------------------------------------------------

// Returns the first UVSet matching `purpose`, or nullptr if none.
// SceneBlobWriter uses this to pick the Texture-purpose UV for blob output.
inline const UVSet* find_uv_set(const ProcessableMesh& m, UVSetPurpose purpose) {
    for (const auto& s : m.uv_sets) {
        if (s.purpose == purpose) return &s;
    }
    return nullptr;
}

inline UVSet* find_uv_set(ProcessableMesh& m, UVSetPurpose purpose) {
    for (auto& s : m.uv_sets) {
        if (s.purpose == purpose) return &s;
    }
    return nullptr;
}

// Sanity check: positions/indices present, all auxiliary arrays either empty
// or matching positions.size(). Returns false if any invariant is violated.
// Used by MeshConverters at IR boundaries; not a substitute for module-level
// validation.
bool validate_invariants(const ProcessableMesh& m);

}  // namespace primal::tools
