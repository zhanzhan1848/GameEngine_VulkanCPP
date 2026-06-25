#pragma once

#include "CommonHeaders.h"
#include "Graphics/PCG/PCGTypes.h"
#include <vector>

namespace primal::graphics::pcg {

// Output buffers from a Marching Cubes / SurfaceNets extraction.
// All buffers are interleaved (xyz for positions/normals, uv for uvs).
// Positions/normals/uvs arrays all have the same length == vertex_count.
// indices.size() == triangle_count * 3.
struct MarchingCubesResult {
    std::vector<f32> positions;   // interleaved x,y,z per vertex
    std::vector<f32> normals;     // interleaved nx,ny,nz per vertex
    std::vector<f32> uvs;         // interleaved u,v per vertex
    std::vector<u32> indices;     // triangle list, CCW winding when viewed from outside
};

// Generate a triangle mesh from a scalar field by sampling it densely on a regular
// grid and running SurfaceNets.
//
// field        — any PCGField providing SampleFloat(world_pos)
// bounds_min   — world-space AABB lower corner (inclusive)
// bounds_max   — world-space AABB upper corner (inclusive)
// resolution   — per-axis voxel count. Total voxels = resolution^3.
//                Caller is responsible for sane bounds (4-128 typical).
// iso_value    — threshold where the surface lives. For SDF, 0.0 is the surface.
//
// Algorithm:
//   1. Sample field at (resolution+1)^3 corner positions
//   2. For each cell (8 corners), compute corner sign mask relative to iso_value
//   3. SurfaceNets: place one dual vertex at the average of edge crossings inside
//      the cell (only when the cell straddles the iso surface)
//   4. Connect adjacent dual vertices with quads (2 triangles) per shared face
//   5. Normals: central-difference gradient of field, step = voxel_size
//   6. UV: Y-planar projection (u on X axis, v on Z axis, mapped to [0,1] over bounds)
//
// Returns empty result on degenerate input (zero/very small resolution, etc.).
MarchingCubesResult GenerateSurfaceNetsCPU(
    const PCGField& field,
    const math::v3& bounds_min,
    const math::v3& bounds_max,
    u32 resolution,
    f32 iso_value = 0.0f);

} // namespace primal::graphics::pcg
