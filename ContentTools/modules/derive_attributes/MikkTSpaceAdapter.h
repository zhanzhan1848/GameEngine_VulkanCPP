#pragma once

// MikkTSpace adapter: bridges ProcessableMesh IR <-> SMikkTSpaceContext.
//
// MikkTSpace (mikktspace.c) is callback-based: it asks for per-face-per-vertex
// position/normal/texcoord and writes back per-face-per-vertex tangent. The
// canonical advice is "results are returned unindexed, do NOT reuse an
// existing index list" — but ProcessableMesh is an indexed IR and we
// cannot break that invariant here. Our adapter therefore writes tangent
// per vertex_idx (last-write-wins when multiple face-corners share a vertex).
//
// Trade-off: hard-edge tangent discontinuities may be slightly smoothed by
// the last-write-wins. This is acceptable for the derive module's primary
// use case (rebuild tangent after subdivide on smooth organic meshes).
// For strict per-corner tangent, run derive with TangentMode::AreaWeighted
// or pre-split on hard edges via NormalMode::Faceted.

#include "ToolsCommon.h"
#include "common/ProcessableMesh.h"

// Forward-declare only SMikkTSpaceContext (a real struct tag in mikktspace.h).
// SMikkTSpaceInterface is a typedef of an anonymous struct in mikktspace.h and
// cannot be forward-declared — callers therefore manipulate the context
// through this header's API only.
struct SMikkTSpaceContext;

namespace primal::tools::derive {

// Construct a SMikkTSpaceContext that reads position/normal/texcoord from
// `mesh` and writes tangent (xyz + sign in w) back into `mesh.tangents`.
// Caller owns the returned context and must free it with destroy_context.
// `uv_set_index` selects which UVSet supplies texcoords (must be a
// non-empty UVSet; caller verifies before calling).
SMikkTSpaceContext* create_context(ProcessableMesh& mesh, u32 uv_set_index);

void destroy_context(SMikkTSpaceContext* ctx);

}  // namespace primal::tools::derive
