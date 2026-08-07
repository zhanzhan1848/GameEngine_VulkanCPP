#pragma once

// SDF generation shared by Geometry.cpp's legacy ImportFbx path and the
// Phase 1 AssetPipeline. Extracted from Geometry.cpp:352-436 in M2 to break
// the dependency cycle: SceneBlobWriter (M8) needs SDF without dragging in
// the rest of Geometry.cpp's legacy pipeline internals.
//
// Behavior is identical to the original inline implementation — purely code
// motion. Resolution fixed at 32 (matches runtime SDF-tracing consumer
// expectation).

#include "ToolsCommon.h"
#include "Geometry.h"  // mesh::sdf_data + mesh::vertices + mesh::indices

namespace primal::tools {

// Generates a 32^3 grid-based signed distance field from mesh.vertices +
// mesh.indices. Writes results into mesh.sdf (resolution / bounds_min /
// bounds_max / data / voxels / vector_field). Bounds auto-padded by 20% of
// the longest dimension. No-op safe if m.vertices is empty.
void generate_sdf(mesh& m);

}  // namespace primal::tools
