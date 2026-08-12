// Engine/Graphics/WFC/AutoSocketClassifier.cpp
#include "AutoSocketClassifier.h"

#include <cassert>

namespace primal::graphics::wfc {

namespace {

// f32x3 packed positions in RHIMeshAsset (3 floats, tightly packed, no pad).
constexpr u32 kPositionStride = 12;

// Local math helpers — keeps this .cpp self-contained.
// (Engine has no global cross/dot for math::v3; component-wise inline
// matches the pattern in PCG/Nodes/MeshSurfaceSampler.h.)
inline f32 dot(const math::v3& a, const math::v3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline math::v3 cross(const math::v3& a, const math::v3& b) {
    return math::v3{
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x};
}

// Transform a point (w=1) by a 4x4 matrix. Uses simd::float4x4 multiplication
// on Apple (math::m4x4 aliases to simd::float4x4). Falls back to component-wise
// math on other platforms.
inline math::v3 TransformPoint(const math::m4x4& m, const math::v3& p) {
#if defined(__APPLE__)
    const simd::float4 homo = matrix_multiply(
        m, simd::float4{p.x, p.y, p.z, 1.0f});
    return math::v3{homo.x, homo.y, homo.z};
#else
    return math::v3{
        m.columns[0][0] * p.x + m.columns[1][0] * p.y + m.columns[2][0] * p.z + m.columns[3][0],
        m.columns[0][1] * p.x + m.columns[1][1] * p.y + m.columns[2][1] * p.z + m.columns[3][1],
        m.columns[0][2] * p.x + m.columns[1][2] * p.y + m.columns[2][2] * p.z + m.columns[3][2]};
#endif
}

// Transform a direction (w=0) by a 4x4 matrix — translation column ignored.
inline math::v3 TransformDirection(const math::m4x4& m, const math::v3& d) {
#if defined(__APPLE__)
    const simd::float4 homo = matrix_multiply(
        m, simd::float4{d.x, d.y, d.z, 0.0f});
    return math::v3{homo.x, homo.y, homo.z};
#else
    return math::v3{
        m.columns[0][0] * d.x + m.columns[1][0] * d.y + m.columns[2][0] * d.z,
        m.columns[0][1] * d.x + m.columns[1][1] * d.y + m.columns[2][1] * d.z,
        m.columns[0][2] * d.x + m.columns[1][2] * d.y + m.columns[2][2] * d.z};
#endif
}

// Face basis: sample frame for one of the 6 cube faces on a unit cube
// (extent 0.5, centered at origin). Convention matches WFCFaceCorners.h:
// corners are CCW from outside, with U/V spanning the face plane.
struct FaceBasis {
    math::v3 origin;   // face center on a unit cube (extent 0.5)
    math::v3 normal;   // outward normal
    math::v3 u_axis;   // local U axis on face plane
    math::v3 v_axis;   // local V axis on face plane
};

inline FaceBasis GetFaceBasis(WFCFace face) {
    switch (face) {
        case WFCFace::PosX: return { math::v3{+0.5f, 0, 0}, math::v3{+1, 0, 0}, math::v3{0, 0, -1}, math::v3{0, +1, 0} };
        case WFCFace::NegX: return { math::v3{-0.5f, 0, 0}, math::v3{-1, 0, 0}, math::v3{0, 0, +1}, math::v3{0, +1, 0} };
        case WFCFace::PosY: return { math::v3{0, +0.5f, 0}, math::v3{0, +1, 0}, math::v3{+1, 0, 0}, math::v3{0, 0, +1} };
        case WFCFace::NegY: return { math::v3{0, -0.5f, 0}, math::v3{0, -1, 0}, math::v3{+1, 0, 0}, math::v3{0, 0, -1} };
        case WFCFace::PosZ: return { math::v3{0, 0, +0.5f}, math::v3{0, 0, +1}, math::v3{-1, 0, 0}, math::v3{0, +1, 0} };
        case WFCFace::NegZ: return { math::v3{0, 0, -0.5f}, math::v3{0, 0, -1}, math::v3{+1, 0, 0}, math::v3{0, +1, 0} };
    }
    return {};
}

inline u8 ReverseBits8(u8 b) {
    u8 r = 0;
    for (u32 i = 0; i < 8; ++i) {
        r = static_cast<u8>((r << 1) | (b & 1));
        b = static_cast<u8>(b >> 1);
    }
    return r;
}

} // anonymous namespace

bool AutoSocketClassifier::RayTriangle(
    const math::v3& origin, const math::v3& dir,
    const math::v3& v0, const math::v3& v1, const math::v3& v2,
    f32 max_t, f32* t) {
    constexpr f32 kEpsilon = 1e-6f;

    math::v3 edge1 = v1 - v0;
    math::v3 edge2 = v2 - v0;
    math::v3 h = cross(dir, edge2);
    f32 a = dot(edge1, h);

    // Backface cull: only positive a (front-facing triangles).
    if (a < kEpsilon) return false;

    math::v3 s = origin - v0;
    f32 u = dot(s, h);
    if (u < 0.0f || u > a) return false;

    math::v3 q = cross(s, edge1);
    f32 v = dot(dir, q);
    if (v < 0.0f || (u + v) > a) return false;

    f32 inv_a = 1.0f / a;
    f32 tt = dot(edge2, q) * inv_a;
    if (tt < kEpsilon || tt > max_t) return false;

    if (t) *t = tt;
    return true;
}

SocketEncoding AutoSocketClassifier::ClassifyFace(
    const graphics::rhi::RHIMeshAsset& mesh,
    WFCFace face,
    const math::m4x4& variant_transform) {

    // Read mesh data from RHIMeshAsset buffers directly.
    // Positions: 12-byte stride (f32 x 3, tightly packed).
    // Indices:   u32 (mesh.index_size == 4 for procedural assets).
    if (mesh.position_buffer.empty() || mesh.index_buffer.empty()) return 0;
    if (mesh.num_indices == 0 || mesh.num_vertices == 0) return 0;

    // Only u32-indexed meshes are supported. u16 indices would silently
    // read 2× the buffer as garbage u32s.
    if (mesh.index_size != 4) return 0;

    const u32 tri_count = mesh.num_indices / 3;
    if (tri_count == 0) return 0;
    const u32 num_verts = mesh.num_vertices;

    const u8* pos_bytes = mesh.position_buffer.data();
    const u32* indices  = reinterpret_cast<const u32*>(mesh.index_buffer.data());

    const FaceBasis fb = GetFaceBasis(face);
    // Sample offset: how far outside the face the ray starts. Small enough that
    // the ray clearly clears the face plane even after variant rotation rounding.
    // Ray length: how far the ray reaches into the cube. 5cm is enough to hit
    // any face of a unit (1m) cube with margin for variant-rotated geometry.
    constexpr f32 kSampleOffset = 0.005f;
    constexpr f32 kRayLength    = 0.05f;
    constexpr u32 kGridN        = 8;

    SocketEncoding sig = 0;
    for (u32 j = 0; j < kGridN; ++j) {
        for (u32 i = 0; i < kGridN; ++i) {
            // Sample point at center of cell (i, j) in 8×8 grid covering
            // [-0.5, +0.5]² on the face plane.
            const f32 u = -0.5f + (i + 0.5f) / kGridN;
            const f32 v = -0.5f + (j + 0.5f) / kGridN;

            // Apply variant_transform to face basis (rotates the sample frame).
            const math::v3 local_pos = fb.origin + fb.u_axis * u + fb.v_axis * v;
            const math::v3 world_pos = TransformPoint(variant_transform, local_pos);
            const math::v3 world_nrm = TransformDirection(variant_transform, fb.normal);

            const math::v3 ray_origin = world_pos + world_nrm * kSampleOffset;
            const math::v3 ray_dir    = -world_nrm;

            bool solid = false;
            f32 nearest_t = kRayLength;
            for (u32 t = 0; t < tri_count; ++t) {
                const u32 i0 = indices[t * 3 + 0];
                const u32 i1 = indices[t * 3 + 1];
                const u32 i2 = indices[t * 3 + 2];
                assert(i0 < num_verts && i1 < num_verts && i2 < num_verts);

                const f32* p0 = reinterpret_cast<const f32*>(pos_bytes + i0 * kPositionStride);
                const f32* p1 = reinterpret_cast<const f32*>(pos_bytes + i1 * kPositionStride);
                const f32* p2 = reinterpret_cast<const f32*>(pos_bytes + i2 * kPositionStride);
                const math::v3 v0 = TransformPoint(variant_transform, math::v3{p0[0], p0[1], p0[2]});
                const math::v3 v1 = TransformPoint(variant_transform, math::v3{p1[0], p1[1], p1[2]});
                const math::v3 v2 = TransformPoint(variant_transform, math::v3{p2[0], p2[1], p2[2]});

                f32 hit_t;
                if (RayTriangle(ray_origin, ray_dir, v0, v1, v2, kRayLength, &hit_t)) {
                    if (hit_t < nearest_t) {
                        nearest_t = hit_t;
                        solid = true;
                    }
                }
            }

            const u32 bit_index = i + j * kGridN;
            if (solid) sig |= (SocketEncoding{1} << bit_index);
        }
    }
    return sig;
}

SocketEncoding AutoSocketClassifier::MirrorFlipU(SocketEncoding sig) {
    SocketEncoding out = 0;
    for (u32 row = 0; row < 8; ++row) {
        const u8 bits = static_cast<u8>((sig >> (row * 8)) & 0xFF);
        const u8 reversed = ReverseBits8(bits);
        out |= static_cast<u64>(reversed) << (row * 8);
    }
    return out;
}

AutoSocketClassifier::FaceSignatures
AutoSocketClassifier::ClassifyTile(
    const graphics::rhi::RHIMeshAsset& mesh,
    const math::m4x4& variant_transform) {
    FaceSignatures out;
    for (u32 f = 0; f < WFC_FACE_COUNT_3D; ++f) {
        out.face[f] = ClassifyFace(mesh, static_cast<WFCFace>(f), variant_transform);
    }
    return out;
}

} // namespace primal::graphics::wfc
