#pragma once

#include "CommonHeaders.h"
#include "../RHI/Core/RHITypes.h"
#include "../RHI/Core/RHIMeshAsset.h"
#include "../Utilities/Vector.h"

namespace primal::graphics::nanite {

// Meshlet vertex/triangle limits. The triangle cap MUST match the vertex_count
// written by stage7_build_indirect_commands (384 = 128 * 3). If triangle_count
// exceeds 128, the extra triangles never get vertex shader invocations and are
// silently dropped — producing the "fragmented meshlet" visual where meshlets
// render with holes or missing chunks.
constexpr u32 kMeshletMaxVertices = 256;
constexpr u32 kMeshletMaxTriangles = 128;

struct SynthesizedMeshlets {
    utl::vector<rhi::RHIMeshlet> meshlets;
    utl::vector<u32> meshlet_vertices;
    utl::vector<u8> meshlet_triangles;
};

// Runtime meshlet synthesis: chunks an index buffer into ≤256-vertex meshlets.
// Used for assets (e.g. Sponza.model) that ship without pre-baked MSHL sections.
// The GPU-driven pipeline requires meshlet data to function; this fallback
// generates it on demand from position + index buffers.
void SynthesizeMeshlets(const rhi::RHIMeshAsset& asset, SynthesizedMeshlets& out);

} // namespace primal::graphics::nanite
