#pragma once

// SceneBlobWriter: serializes ProcessableScene + derived data into the
// scene_data.buffer binary layout that 4 downstream consumers depend on
// (ContentToEngine.cpp / pack_geometry.py / MeshletValidator.py / C# PrimalEditor).
//
// M2/M8 implements; contract source is Geometry.cpp pack_mesh_data :789-873.
// Magic: MSHL=0x4C48534D, SDF=0x20464453 (ContentToEngine.cpp:185/234).

namespace primal::tools::pipeline {

// TODO(M2/M8): void WriteSceneBlob(const ProcessableScene& scene,
//           const utl::vector<PackedMesh>& packed, scene_data* out);

}  // namespace primal::tools::pipeline
