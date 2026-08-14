#pragma once

// PackedMeshDecoder: inverts Geometry.cpp::pack_vertices + pack_mesh_data +
// pack_data, producing ProcessableScene IR from a scene_data.buffer blob.
// Used by ProcessAIAsset (M12) to round-trip ImportFbx output through the
// Phase 1 pipeline::Run before re-serializing.
//
// Contract sources:
//   - Geometry.cpp:310-405 (pack_vertices — element_buffer encoder)
//   - Geometry.cpp:661-745 (pack_mesh_data — single-mesh body encoder)
//   - Geometry.cpp:865-910 (pack_data — full scene wrapper encoder)
//
// Coverage:
//   - All 4 static_* elements_types reachable from ProcessableMesh IR
//     (position_only / static_color / static_normal / static_normal_texture).
//   - Skeletal_* are unreachable from IR; decoder rejects them with an error.
//
// Lossy fields (irreversible — caller must run derive::Run to rebuild):
//   - For static_normal_texture: tangent.w collapses to a single bit
//     (bit 0 of t_sign encodes "(tw > 0) && (tz > 0)"). Decoder sets w=1.f
//     on positive sign, w=-1.f on negative; tz is rebuilt via sqrt.
//   - For static_normal_texture: normal.z bit (bit 1 of t_sign) is NOT
//     written by the encoder due to a latent pack_vertices bug — the
//     static_normal_texture branch overwrites t_sign entirely. Decoder
//     defaults nz to +sqrt(...) in this case (matches the static_normal
//     branch's encoding intent). Round-tripping then re-deriving is the
//     only correct path.
//
// Index widening: u16 indices are widened to u32 on decode (ProcessableMesh
// uses utl::vector<u32> indices uniformly).

#include "ToolsCommon.h"
#include "Geometry.h"
#include "common/ProcessableMesh.h"
#include "common/PipelineTypes.h"
#include "common/ErrorReport.h"
#include "SceneBlobReader.h"

namespace primal::tools::pipeline {

// Decode a single PackedMeshView into a ProcessableMesh. Caller first calls
// ReadNextPackedMesh to obtain the view (zero-copy slice into the blob),
// then this function to materialize the IR.
//
// On success, dst.positions/normals/tangents/uvs/colors/indices are filled
// per the elements_type contract. On failure, returns false + writes errors;
// dst is left in a partially-filled state (caller should discard).
bool DecodePackedMesh(const PackedMeshView& src, ProcessableMesh& dst,
                      utl::vector<ErrorReport>& errors);

// Decode an entire scene_data.buffer into a ProcessableScene.
//
// Walks scene_name + materials via ReadSceneHeader, then iterates every
// (lod_group, mesh) pair, flattening all meshes into dst.lods[0].meshes.
// Phase 2 ProcessableScene treats all decoded meshes as LOD 0 source
// material — the pipeline generates additional LODs from lods[0].meshes[0]
// when enable_lod is on (existing AssetPipeline contract).
//
// Returns false on any parse error; dst is left partially-filled.
bool DecodeScene(const scene_data& src, ProcessableScene& dst,
                 utl::vector<ErrorReport>& errors);

}  // namespace primal::tools::pipeline
