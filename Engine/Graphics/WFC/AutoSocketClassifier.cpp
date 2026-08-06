// Engine/Graphics/WFC/AutoSocketClassifier.cpp
#include "AutoSocketClassifier.h"

namespace primal::graphics::wfc {

namespace {

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

} // namespace primal::graphics::wfc
