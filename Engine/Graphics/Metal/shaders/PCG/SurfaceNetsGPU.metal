#include <metal_stdlib>

using namespace metal;

// SurfaceNets GPU kernel constants — must match host-side constants in GPUMesher.cpp.
// Named SN_INVALID_ID (not UINT_MAX) to avoid colliding with the macro of the same
// name defined by metal_types when metal_stdlib is included.
constant constexpr uint SN_INVALID_ID = 0xffffffffu;
constant constexpr float INV_INTERVALS = 2.0f / 65535.0f;  // matches content::PackSignedNormalComponent decode

// Corner offsets — must match CPU kCornerOffset in MarchingCubes.cpp verbatim.
// Bit position in mask = corner index.
constant constexpr int3 kCornerOffset[8] = {
    int3(0,0,0), int3(1,0,0), int3(1,0,1), int3(0,0,1),
    int3(0,1,0), int3(1,1,0), int3(1,1,1), int3(0,1,1),
};

// Uniforms passed via a constant buffer at bind 0.
struct SurfaceNetsUniforms {
    uint  resolution;       // cells per axis
    uint  n;                // grid vertices per axis = resolution + 1
    uint  n2;               // n * n
    float voxel_x, voxel_y, voxel_z;
    float origin_x, origin_y, origin_z;
    float extent_x, extent_y, extent_z;
    float iso_value;
    uint  pad0, pad1, pad2;  // align to 16 bytes
};

// Pass 1: classify_cells
// One thread per cell. Reads 8 corner samples, computes sign mask, emits a dual
// vertex ID via atomic counter if the cell straddles the iso surface.
//
// Bindings:
//   0: uniforms (uniform)
//   1: scalar_volume (device buffer, f32[(n)³])
//   2: dual_id_out  (device buffer, u32[resolution³]; SN_INVALID_ID if not straddling)
//   3: vertex_counter (device buffer, u32[1]; atomic)
//
// Dispatch: resolution³ threads (threadgroup size 4×4×4 = 64).
kernel void classify_cells(
    constant SurfaceNetsUniforms& u        [[buffer(0)]],
    device const float*           scalar   [[buffer(1)]],
    device uint*                  dual_id  [[buffer(2)]],
    device atomic_uint*           vcounter [[buffer(3)]],
    uint3                         tid      [[thread_position_in_grid]])
{
    const uint res = u.resolution;
    if (any(tid >= uint3(res))) return;

    // Cell (i,j,k) corner samples at grid vertices (i+c.x, j+c.y, k+c.z).
    float cv[8];
    uint mask = 0;
    for (uint c = 0; c < 8; ++c) {
        uint3 g = tid + uint3(kCornerOffset[c]);
        uint idx = g.x + u.n * g.y + u.n2 * g.z;
        cv[c] = scalar[idx];
        if (cv[c] > u.iso_value) mask |= (1u << c);
    }

    uint cell_idx = tid.x + res * tid.y + res * res * tid.z;
    if (mask == 0u || mask == 0xFFu) {
        dual_id[cell_idx] = SN_INVALID_ID;
        return;
    }

    uint vid = atomic_fetch_add_explicit(vcounter, 1u, memory_order_relaxed);
    dual_id[cell_idx] = vid;
}
