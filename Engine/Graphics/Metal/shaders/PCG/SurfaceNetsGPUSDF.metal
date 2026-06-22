// SurfaceNets GPU — SDF variant (Phase 9.3b, Task 3).
//
// Consumes GlobalSDF cascade textures directly, eliminating the CPU sample loop
// (Pass 0 of 9.3a). Pass 1 (classify_cells_sdf) writes the scalar volume buffer
// so passes 2-4 run byte-identical to 9.3a (they load the original
// SurfaceNetsGPU.metal kernels and read from that buffer).
//
// Self-contained — does NOT #include SurfaceNetsGPU.metal. MTLCompileOptions in
// MetalShader.cpp:61 does not set includeSearchPaths, so runtime source
// compilation cannot resolve includes. Shared constants (SN_INVALID_ID,
// kCornerOffset) are redefined here and MUST match 9.3a exactly.

#include <metal_stdlib>

using namespace metal;

// Shared constants — must match SurfaceNetsGPU.metal:8 and :13 verbatim.
constant constexpr uint SN_INVALID_ID = 0xffffffffu;

constant constexpr int3 kCornerOffset[8] = {
    int3(0,0,0), int3(1,0,0), int3(1,0,1), int3(0,0,1),
    int3(0,1,0), int3(1,1,0), int3(1,1,1), int3(0,1,1),
};

// Extended uniforms (mirror SurfaceNetsUniforms + cascade data, see spec §6.2).
// IMPORTANT: the 9.3a SurfaceNetsUniforms fields must come first in the same
// order, so emit_vertices/emit_faces/write_indirect (which take the 9.3a struct)
// read correctly when given the same constant buffer.
struct SurfaceNetsSDFUniforms {
    // --- 9.3a base (offsets 0..60) ---
    uint  resolution;       // cells per axis
    uint  n;                // grid vertices per axis = resolution + 1
    uint  n2;               // n * n
    float voxel_x, voxel_y, voxel_z;
    float origin_x, origin_y, origin_z;
    float extent_x, extent_y, extent_z;
    float iso_value;
    uint  pad0, pad1, pad2;  // align to 16 bytes (matches 9.3a SurfaceNetsUniforms)
    // --- GlobalSDF cascades (offset 64+) ---
    float3 SdfOrigins[3];
    float  SdfVoxelSizes[3];
    float  SdfExtents[3];
    uint   SdfResolutions[3];
};

// Returns true if `p` is inside the cascade AABB [origin, origin + extent].
// Exclusive upper bound prevents double-sampling at cascade boundaries.
inline bool in_cascade(float3 p, float3 origin, float extent) {
    return all(p >= origin) && all(p < origin + float3(extent));
}

// Samples SDF at world_pos using the finest available cascade. Returns a large
// positive value (treated as solid) if outside all cascades. Mirrors the
// sampleBestSDF_elseIf pattern in Lumen/SDFTraceCommon.metal:60-78.
inline float sample_global_sdf(
    constant SurfaceNetsSDFUniforms& u,
    texture3d<float, access::sample> t0,
    texture3d<float, access::sample> t1,
    texture3d<float, access::sample> t2,
    float3 world_pos)
{
    constexpr sampler s(filter::linear, address::clamp_to_edge, coord::normalized);

    if (in_cascade(world_pos, u.SdfOrigins[0], u.SdfExtents[0])) {
        float3 uvw = (world_pos - u.SdfOrigins[0]) / u.SdfExtents[0];
        return t0.sample(s, uvw).r;
    }
    if (in_cascade(world_pos, u.SdfOrigins[1], u.SdfExtents[1])) {
        float3 uvw = (world_pos - u.SdfOrigins[1]) / u.SdfExtents[1];
        return t1.sample(s, uvw).r;
    }
    if (in_cascade(world_pos, u.SdfOrigins[2], u.SdfExtents[2])) {
        float3 uvw = (world_pos - u.SdfOrigins[2]) / u.SdfExtents[2];
        return t2.sample(s, uvw).r;
    }
    return 1e6f;  // outside all cascades → solid
}

// Pass 1 (SDF variant): classify_cells_sdf.
// Same I/O contract as 9.3a classify_cells, plus:
//   - samples 3 cascade textures instead of reading a CPU-uploaded scalar buf
//   - writes the scalar buffer so passes 2-4 can run byte-identical to 9.3a
//
// Bindings:
//   buffer(0):  SurfaceNetsSDFUniforms uniforms
//   texture(0..2): GlobalSDF cascades (finest → coarsest)
//   buffer(1):  scalar_volume (f32[(n)³], write)
//   buffer(2):  dual_id (u32[res³], write)
//   buffer(3):  counters (atomic_uint, increment counters[0] = vertex_count)
//
// Dispatch: resolution³ threads (threadgroup size 4×4×4 = 64, same as 9.3a).
kernel void classify_cells_sdf(
    constant SurfaceNetsSDFUniforms& u  [[buffer(0)]],
    texture3d<float, access::sample> t0 [[texture(0)]],
    texture3d<float, access::sample> t1 [[texture(1)]],
    texture3d<float, access::sample> t2 [[texture(2)]],
    device float*                   scalar   [[buffer(1)]],
    device uint*                    dual_id  [[buffer(2)]],
    device atomic_uint*             counters [[buffer(3)]],
    uint3                           tid      [[thread_position_in_grid]])
{
    const uint res = u.resolution;
    if (any(tid >= uint3(res))) return;

    // Cell (i,j,k) corner samples at grid vertices (i+c.x, j+c.y, k+c.z).
    float cv[8];
    uint mask = 0;
    for (uint c = 0; c < 8; ++c) {
        uint3 g = tid + uint3(kCornerOffset[c]);
        float3 world_pos = float3(
            u.origin_x + u.voxel_x * float(g.x),
            u.origin_y + u.voxel_y * float(g.y),
            u.origin_z + u.voxel_z * float(g.z));
        cv[c] = sample_global_sdf(u, t0, t1, t2, world_pos);

        // Write scalar volume at grid-vertex index (same layout as 9.3a).
        uint idx = g.x + u.n * g.y + u.n2 * g.z;
        scalar[idx] = cv[c];
        if (cv[c] > u.iso_value) mask |= (1u << c);
    }

    uint cell_idx = tid.x + res * tid.y + res * res * tid.z;
    if (mask == 0u || mask == 0xFFu) {
        dual_id[cell_idx] = SN_INVALID_ID;
        return;
    }

    uint vid = atomic_fetch_add_explicit(counters, 1u, memory_order_relaxed);
    dual_id[cell_idx] = vid;
}
