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

// 12 cell edges, each as (corner_a, corner_b). Must match CPU kCellEdges.
constant constexpr uint2 kCellEdges[12] = {
    uint2(0,1), uint2(1,2), uint2(2,3), uint2(3,0),
    uint2(4,5), uint2(5,6), uint2(6,7), uint2(7,4),
    uint2(0,4), uint2(1,5), uint2(2,6), uint2(3,7),
};

// Pack a normal/uv into the 20-byte element body. Identical bit layout to
// content::PackVertexElement in ProceduralMesh.h. Caller normalizes (nx,ny,nz).
void pack_vertex_element(
    device uint8_t* elem,
    float nx, float ny, float nz,
    float u, float v)
{
    auto quant = [](float c) -> uint16_t {
        float q = (c + 1.0f) * 32767.5f;
        q = clamp(q, 0.0f, 65535.0f);
        return static_cast<uint16_t>(q);
    };
    uint16_t n0 = quant(nx);
    uint16_t n1 = quant(ny);
    uint16_t t0 = quant(1.0f);
    uint16_t t1 = quant(0.0f);

    uint8_t sign_byte = 0;
    if (nz >= 0.0f) sign_byte |= 0x02;
    uint32_t color_tsign = 0x00FFFFFFu | (uint32_t(sign_byte) << 24);

    // Unaligned stores — use reinterpret_cast + manual copy via uint8_t*.
    device uint8_t* p = elem;
    *reinterpret_cast<device uint32_t*>(p + 0)  = color_tsign;
    *reinterpret_cast<device uint16_t*>(p + 4)  = n0;
    *reinterpret_cast<device uint16_t*>(p + 6)  = n1;
    *reinterpret_cast<device uint16_t*>(p + 8)  = t0;
    *reinterpret_cast<device uint16_t*>(p + 10) = t1;
    *reinterpret_cast<device float*>(p + 12)    = u;
    *reinterpret_cast<device float*>(p + 16)    = v;
}

// Pass 2: emit_vertices
// One thread per cell. Skips cells with dual_id == SN_INVALID_ID. Otherwise computes
// dual vertex position (average of edge crossings), cell-center finite-difference
// normal (using the 8 corner samples), Y-planar UV, and writes to position/element
// buffers at offset = dual_id.
//
// Bindings:
//   0: uniforms
//   1: scalar_volume (read)
//   2: dual_id (read — Pass 1 wrote this)
//   3: position_buffer (write, f32×3 per vertex, indexed by dual_id)
//   4: element_buffer (write, 20B per vertex)
//
// Dispatch: resolution³ threads.
kernel void emit_vertices(
    constant SurfaceNetsUniforms& u        [[buffer(0)]],
    device const float*           scalar   [[buffer(1)]],
    device const uint*            dual_id  [[buffer(2)]],
    device float*                 positions[[buffer(3)]],
    device uint8_t*               elements [[buffer(4)]],
    uint3                         tid      [[thread_position_in_grid]])
{
    const uint res = u.resolution;
    if (any(tid >= uint3(res))) return;

    uint cell_idx = tid.x + res * tid.y + res * res * tid.z;
    uint vid = dual_id[cell_idx];
    if (vid == SN_INVALID_ID) return;

    // Load 8 corner samples + positions.
    float cv[8];
    float3 cp[8];
    uint mask = 0;
    for (uint c = 0; c < 8; ++c) {
        uint3 g = tid + uint3(kCornerOffset[c]);
        uint idx = g.x + u.n * g.y + u.n2 * g.z;
        cv[c] = scalar[idx];
        cp[c] = float3(u.origin_x + u.voxel_x * float(g.x),
                       u.origin_y + u.voxel_y * float(g.y),
                       u.origin_z + u.voxel_z * float(g.z));
        if (cv[c] > u.iso_value) mask |= (1u << c);
    }

    // Average edge crossings → dual position.
    float3 sum = 0.0f;
    uint count = 0u;
    for (uint e = 0; e < 12u; ++e) {
        uint a = kCellEdges[e].x;
        uint b = kCellEdges[e].y;
        bool a_in = (mask >> a) & 1u;
        bool b_in = (mask >> b) & 1u;
        if (a_in == b_in) continue;
        float denom = cv[b] - cv[a];
        float t = (denom != 0.0f) ? (u.iso_value - cv[a]) / denom : 0.5f;
        t = clamp(t, 0.0f, 1.0f);
        sum += cp[a] + t * (cp[b] - cp[a]);
        ++count;
    }
    if (count == 0u) return;
    float3 dual_pos = sum / float(count);

    // Cell-center finite-difference normal: difference of average face-corner
    // values per axis. Cheaper than CPU's dual-position gradient (no extra
    // field samples) and visually smoother.
    float gx = 0.25f * (cv[1] + cv[2] + cv[5] + cv[6])  // +X face
             - 0.25f * (cv[0] + cv[3] + cv[4] + cv[7]); // -X face
    float gy = 0.25f * (cv[4] + cv[5] + cv[6] + cv[7])  // +Y face
             - 0.25f * (cv[0] + cv[1] + cv[2] + cv[3]); // -Y face
    float gz = 0.25f * (cv[2] + cv[3] + cv[6] + cv[7])  // +Z face
             - 0.25f * (cv[0] + cv[1] + cv[4] + cv[5]); // -Z face
    float gl = sqrt(gx*gx + gy*gy + gz*gz) + 1e-12f;
    float3 normal = float3(gx, gy, gz) / gl;

    float u_uv = (dual_pos.x - u.origin_x) / u.extent_x;
    float v_uv = (dual_pos.z - u.origin_z) / u.extent_z;

    // Write outputs.
    positions[vid * 3 + 0] = dual_pos.x;
    positions[vid * 3 + 1] = dual_pos.y;
    positions[vid * 3 + 2] = dual_pos.z;

    device uint8_t* elem = elements + vid * 20;
    pack_vertex_element(elem, normal.x, normal.y, normal.z, u_uv, v_uv);
}

// Pass 3: emit_faces
// ----------------------------------------------------------------------------
// For each grid edge axis a, the two perpendicular axes. Matches CPU kPerpAxes.
constant constexpr uint kPerpAxes[3][2] = {
    {1, 2},  // axis X → perp Y, Z
    {2, 0},  // axis Y → perp Z, X
    {0, 1},  // axis Z → perp X, Y
};

// Helper: load dual_id for cell (i,j,k). Returns SN_INVALID_ID if OOB.
// `dp1` and `dp2` are offsets along the two perp axes (edge axis stays at gp[a]).
template<uint Axis>
inline uint cell_dual_at(device const uint* dual_id, uint res, uint3 gp, int dp1, int dp2) {
    constexpr uint P1 = kPerpAxes[Axis][0];
    constexpr uint P2 = kPerpAxes[Axis][1];
    int ci[3] = { int(gp.x), int(gp.y), int(gp.z) };
    ci[P1] += dp1;
    ci[P2] += dp2;
    if (ci[0] < 0 || ci[1] < 0 || ci[2] < 0) return SN_INVALID_ID;
    if (ci[0] >= int(res) || ci[1] >= int(res) || ci[2] >= int(res)) return SN_INVALID_ID;
    uint idx = uint(ci[0]) + res * uint(ci[1]) + res * res * uint(ci[2]);
    return dual_id[idx];
}

// Emit fan-triangulated polygon. `ids` has up to 4 entries; invalid ones are SN_INVALID_ID.
inline void emit_face_polygon(
    device uint*                index_buffer,
    device atomic_uint*         index_counter,
    uint32_t                    ids[4])
{
    // Compact valid ids preserving CCW order.
    uint v[4];
    uint n_valid = 0u;
    for (uint i = 0; i < 4u; ++i) {
        if (ids[i] != SN_INVALID_ID) v[n_valid++] = ids[i];
    }
    if (n_valid < 3u) return;

    // Fan triangulation. Matches CPU emit_face.
    for (uint t = 1u; t < n_valid - 1u; ++t) {
        uint base = atomic_fetch_add_explicit(index_counter, 3u, memory_order_relaxed);
        index_buffer[base + 0] = v[0];
        index_buffer[base + 1] = v[t];
        index_buffer[base + 2] = v[t + 1];
    }
}

// Pass 3: emit_faces (3 axis-specific kernels).
// One thread per grid vertex per axis. Reads 2 scalar endpoints + 4 dual_ids.
// Emits 1-2 triangles via atomic_append on index_counter.
//
// Bindings:
//   0: uniforms
//   1: scalar_volume (read)
//   2: dual_id (read)
//   3: index_buffer (write)
//   4: counters (atomic, u32[2]; we increment counters[1] = index_count)
//
// Dispatch: n³ threads (one per grid vertex); early-out if edge exits grid.
template<uint Axis>
inline void emit_faces_impl(
    constant SurfaceNetsUniforms& u        [[buffer(0)]],
    device const float*           scalar   [[buffer(1)]],
    device const uint*            dual_id  [[buffer(2)]],
    device uint*                  indices  [[buffer(3)]],
    device atomic_uint*           counters [[buffer(4)]],
    uint3                         tid      [[thread_position_in_grid]])
{
    const uint n = u.n;
    const uint res = u.resolution;
    if (any(tid >= uint3(n))) return;
    if (tid[Axis] + 1u >= n) return;  // edge would exit grid

    // Endpoints along axis.
    uint3 gp1 = tid;
    uint3 gp2 = tid;
    ++gp2[Axis];
    float s1 = scalar[gp1.x + n * gp1.y + n * n * gp1.z];
    float s2 = scalar[gp2.x + n * gp2.y + n * n * gp2.z];
    bool sign_diff = (s1 > u.iso_value) != (s2 > u.iso_value);
    if (!sign_diff) return;

    // 4 surrounding cells, CCW around the edge. Matches CPU make_cell sequence:
    //   (0,0), (-1,0), (-1,-1), (0,-1)
    uint32_t ids[4] = {
        cell_dual_at<Axis>(dual_id, res, tid,  0,  0),
        cell_dual_at<Axis>(dual_id, res, tid, -1,  0),
        cell_dual_at<Axis>(dual_id, res, tid, -1, -1),
        cell_dual_at<Axis>(dual_id, res, tid,  0, -1),
    };
    // counters[1] = index_count (counters[0] is vertex_count, owned by classify_cells).
    emit_face_polygon(indices, counters + 1u, ids);
}

kernel void emit_faces_x(
    constant SurfaceNetsUniforms& u        [[buffer(0)]],
    device const float*           scalar   [[buffer(1)]],
    device const uint*            dual_id  [[buffer(2)]],
    device uint*                  indices  [[buffer(3)]],
    device atomic_uint*           counters [[buffer(4)]],
    uint3                         tid      [[thread_position_in_grid]])
{
    emit_faces_impl<0>(u, scalar, dual_id, indices, counters, tid);
}

kernel void emit_faces_y(
    constant SurfaceNetsUniforms& u        [[buffer(0)]],
    device const float*           scalar   [[buffer(1)]],
    device const uint*            dual_id  [[buffer(2)]],
    device uint*                  indices  [[buffer(3)]],
    device atomic_uint*           counters [[buffer(4)]],
    uint3                         tid      [[thread_position_in_grid]])
{
    emit_faces_impl<1>(u, scalar, dual_id, indices, counters, tid);
}

kernel void emit_faces_z(
    constant SurfaceNetsUniforms& u        [[buffer(0)]],
    device const float*           scalar   [[buffer(1)]],
    device const uint*            dual_id  [[buffer(2)]],
    device uint*                  indices  [[buffer(3)]],
    device atomic_uint*           counters [[buffer(4)]],
    uint3                         tid      [[thread_position_in_grid]])
{
    emit_faces_impl<2>(u, scalar, dual_id, indices, counters, tid);
}

// Pass 4: write_indirect_args
// Single-thread dispatch. Reads vertex_counter and index_counter, writes a
// MTLDrawPrimitivesIndirectCommand to indirect_args buffer.
//
// Bindings:
//   0: uniforms
//   1: counters (device buffer, u32[2]; [0]=vertex_counter, [1]=index_counter)
//   2: indirect_args (device buffer, MTLDrawPrimitivesIndirectCommand)
//
// Phase 9.3a does NOT consume indirect_args (we read back and use
// register_mesh_asset), but writing the data structure here costs nothing and
// Phase 9.3b will need it for GPU-resident mesh rendering.

struct MTLDrawPrimitivesIndirectCommand {
    uint32_t vertexCount;
    uint32_t instanceCount;
    uint32_t vertexStart;
    uint32_t baseInstance;
};

kernel void write_indirect_args(
    constant SurfaceNetsUniforms& u           [[buffer(0)]],
    device const uint*           counters    [[buffer(1)]],
    device MTLDrawPrimitivesIndirectCommand* out [[buffer(2)]],
    uint3                         tid        [[thread_position_in_grid]])
{
    if (tid.x != 0u || tid.y != 0u || tid.z != 0u) return;
    out->vertexCount   = counters[1];  // index count
    out->instanceCount = 1u;
    out->vertexStart   = 0u;
    out->baseInstance  = 0u;
}
