#pragma once

// SDF generation shared by Geometry.cpp's legacy ImportFbx path and the
// Phase 1 AssetPipeline. Extracted from Geometry.cpp:352-436 in M2 to break
// the dependency cycle: SceneBlobWriter (M8) needs SDF without dragging in
// the rest of Geometry.cpp's legacy pipeline internals.
//
// Behavior is identical to the original inline implementation except that
// the grid resolution is now caller-controlled (was fixed at 32). The
// runtime consumer (ContentToEngine.cpp) reads resolution from the blob, so
// non-default values round-trip correctly.

#include "ToolsCommon.h"
#include "Geometry.h"  // mesh::sdf_data + mesh::vertices + mesh::indices

namespace primal::tools {

namespace sdf {

struct Params {
    // Voxel grid resolution per axis (grid is resolution^3). 0 falls back
    // to the default 32; values are clamped to [4, 256]. Memory and cost
    // scale cubically: 32 -> ~400KB, 128 -> ~25MB, 256 -> ~190MB per mesh
    // (data u16 + voxels u8 + vector_field u16x4 per cell).
    u32 resolution{32};
};

}  // namespace sdf

// Generates a resolution^3 grid-based distance field from mesh.vertices +
// mesh.indices. Writes results into mesh.sdf (resolution / bounds_min /
// bounds_max / data / voxels / vector_field). Bounds auto-padded by 20% of
// the longest dimension. No-op safe if m.vertices is empty.
void generate_sdf(mesh& m, u32 resolution = 32);

}  // namespace primal::tools
