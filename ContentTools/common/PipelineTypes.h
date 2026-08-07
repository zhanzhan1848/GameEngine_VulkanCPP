#pragma once

// PipelineTypes: derived data types produced by Phase 1 modules and consumed
// by SceneBlobWriter. M2 stub.
//
// MeshletData — output of meshlet module (meshopt_Meshlet array + vertex/triangle arrays).
// PackedMesh  — one submesh worth of blob-ready data (matches Geometry.cpp pack_data layout).

namespace primal::tools {

// TODO(M2): struct MeshletData { utl::vector<meshopt_Meshlet> meshlets;
//           utl::vector<u32> vertices; utl::vector<u8> triangles; };
// TODO(M2): struct PackedMesh { /* mirrors Geometry.cpp:993 pack_data inputs */ };

}  // namespace primal::tools
