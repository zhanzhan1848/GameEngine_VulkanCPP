// ============================================================================
// 门禁自测专用迷你契约头（独立于 Engine 源，模拟 P1 核心层契约的最小面）。
// 只在 scripts/dxmath_shape_gate/selftest/ 内使用，不属于引擎。
// 契约形状对齐 P1 的 MathTypes.h/MathAccess.h/MathDirectXOps.h：
//   v2=XMFLOAT2, v2a=XMFLOAT2A, v3=v3a=XMFLOAT3A, v4=v4a=XMFLOAT4A,
//   m4x4=m4x4a=XMFLOAT4X4A, m3x3=XMFLOAT3X3, u32v*=XMUINT*, s32v*=XMINT*
//   + MathAccess（column/elem/set_elem/component/…）+ namespace DirectX 运算符
// ============================================================================
#pragma once

#include <DirectXMath.h>
#include <cstdint>

#define PRIMAL_MATH_BACKEND_DIRECTXMATH 1

namespace primal::math {

using f32 = float;
using u32 = unsigned int;

using v2   = DirectX::XMFLOAT2;
using v2a  = DirectX::XMFLOAT2A;
using v3   = DirectX::XMFLOAT3A;
using v3a  = DirectX::XMFLOAT3A;
using v4   = DirectX::XMFLOAT4A;
using v4a  = DirectX::XMFLOAT4A;
using m3x3 = DirectX::XMFLOAT3X3;
using m4x4 = DirectX::XMFLOAT4X4A;
using m4x4a = DirectX::XMFLOAT4X4A;
using u32v2 = DirectX::XMUINT2;
using u32v3 = DirectX::XMUINT3;
using u32v4 = DirectX::XMUINT4;
using s32v2 = DirectX::XMINT2;
using s32v3 = DirectX::XMINT3;
using s32v4 = DirectX::XMINT4;

// —— MathAccess 最小面（对齐 P1 契约的名字与语义） ——
[[nodiscard]] inline v4 column(const m4x4& m, u32 c) {
    return v4{ m.m[c][0], m.m[c][1], m.m[c][2], m.m[c][3] };
}
inline void set_column(m4x4& m, u32 c, const v4& v) {
    m.m[c][0] = v.x; m.m[c][1] = v.y; m.m[c][2] = v.z; m.m[c][3] = v.w;
}
[[nodiscard]] inline f32 elem(const m4x4& m, u32 c, u32 r) { return m.m[c][r]; }
inline void set_elem(m4x4& m, u32 c, u32 r, f32 value) { m.m[c][r] = value; }
[[nodiscard]] inline m4x4 make_m4x4(const v4& c0, const v4& c1, const v4& c2, const v4& c3) {
    return m4x4{ c0.x, c0.y, c0.z, c0.w,
                 c1.x, c1.y, c1.z, c1.w,
                 c2.x, c2.y, c2.z, c2.w,
                 c3.x, c3.y, c3.z, c3.w };
}
[[nodiscard]] inline f32 component(const v3& v, u32 i) {
    return i == 0 ? v.x : (i == 1 ? v.y : v.z);
}
[[nodiscard]] inline v4 make_v4(const v3& xyz, f32 w) {
    return v4{ xyz.x, xyz.y, xyz.z, w };
}
[[nodiscard]] inline v3 splat3(f32 s) { return v3{ s, s, s }; }

} // namespace primal::math

// —— 运算符（模拟 P1 的 MathDirectXOps.h：写在 namespace DirectX，仅声明）——
namespace DirectX {
XMFLOAT2 operator+(const XMFLOAT2&, const XMFLOAT2&);
XMFLOAT3A operator+(const XMFLOAT3A&, const XMFLOAT3A&);
XMFLOAT3A operator-(const XMFLOAT3A&, const XMFLOAT3A&);
XMFLOAT3A operator*(const XMFLOAT3A&, float);
XMFLOAT3A operator*(float, const XMFLOAT3A&);
XMFLOAT4A operator+(const XMFLOAT4A&, const XMFLOAT4A&);
XMFLOAT4A operator*(const XMFLOAT4A&, float);
} // namespace DirectX
