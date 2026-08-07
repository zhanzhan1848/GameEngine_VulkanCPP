#pragma once

// SDF generation shared header. Extracted from Geometry.cpp:352-436 in M2 to
// avoid duplicate SDF logic between legacy ImportFbx path and new pipeline.
//
// Phase 1 does NOT re-implement SDF; this header exposes the existing
// generate_sdf() so AssetPipeline can call it without depending on Geometry.cpp
// internals directly.

namespace primal::tools {

// TODO(M2): declare generate_sdf() signature here;
//           keep implementation in SDF.cpp with body lifted from Geometry.cpp:352-436.

}  // namespace primal::tools
