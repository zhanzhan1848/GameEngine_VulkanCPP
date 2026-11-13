#pragma once

// 引擎数学类型的元素访问统一入口。由 MathTypes.h 末尾 include，
// 依赖其中定义的类型别名与 PRIMAL_MATH_BACKEND_* 后端宏。
//
// 动机：DirectXMath 的 XMFLOAT* 没有 .columns 成员、矩阵元素按
// DXM.m[c][r]（c=行）排布；simd/可移植分支按 columns[c][r]（c=列）排布。
// 调用点统一走本头，跨后端不改写：
//   elem(m, c, r) == 引擎语义的 m.columns[c][r]（c=列，r=行）
//   DirectXMath 下 DXM 矩阵第 i 行 == 引擎矩阵第 i 列（RH 行向量约定），
//   故 elem 即 m.m[c][r]，字节序与引擎列主序矩阵完全一致。

#include <cassert>
#include <type_traits>
#include <utility>

namespace primal::math {

// === 矩阵访问：DirectXMath 后端 ===
#if defined(PRIMAL_MATH_BACKEND_DIRECTXMATH)

// 引擎列 c == DXM 行 c，故 column(m, c) 取 DXM 第 c 行的 4 个分量。
[[nodiscard]] inline v4 column(const m4x4& m, u32 c) {
	assert(c < 4);
	return v4{ m.m[c][0], m.m[c][1], m.m[c][2], m.m[c][3] };
}

inline void set_column(m4x4& m, u32 c, const v4& v) {
	assert(c < 4);
	m.m[c][0] = v.x;
	m.m[c][1] = v.y;
	m.m[c][2] = v.z;
	m.m[c][3] = v.w;
}

[[nodiscard]] inline f32 elem(const m4x4& m, u32 c, u32 r) {
	assert(c < 4 && r < 4);
	return m.m[c][r];
}

inline void set_elem(m4x4& m, u32 c, u32 r, f32 value) {
	assert(c < 4 && r < 4);
	m.m[c][r] = value;
}

// XMFLOAT4X4A 的 16 参构造按行主序接收（_11.._14 是第 0 行）；
// DXM 第 0 行 == 引擎第 0 列，故第 0 行传 c0。
[[nodiscard]] inline m4x4 make_m4x4(const v4& c0, const v4& c1, const v4& c2, const v4& c3) {
	return m4x4{
		c0.x, c0.y, c0.z, c0.w,
		c1.x, c1.y, c1.z, c1.w,
		c2.x, c2.y, c2.z, c2.w,
		c3.x, c3.y, c3.z, c3.w
	};
}

// === 矩阵访问：simd / 可移植后端（columns[c] 是第 c 列） ===
#else

[[nodiscard]] inline v4 column(const m4x4& m, u32 c) {
	assert(c < 4);
	return m.columns[c];
}

inline void set_column(m4x4& m, u32 c, const v4& v) {
	assert(c < 4);
	m.columns[c] = v;
}

[[nodiscard]] inline f32 elem(const m4x4& m, u32 c, u32 r) {
	assert(c < 4 && r < 4);
	return m.columns[static_cast<int>(c)][static_cast<int>(r)];
}

inline void set_elem(m4x4& m, u32 c, u32 r, f32 value) {
	assert(c < 4 && r < 4);
	m.columns[static_cast<int>(c)][static_cast<int>(r)] = value;
}

[[nodiscard]] inline m4x4 make_m4x4(const v4& c0, const v4& c1, const v4& c2, const v4& c3) {
	m4x4 m;
	m.columns[0] = c0;
	m.columns[1] = c1;
	m.columns[2] = c2;
	m.columns[3] = c3;
	return m;
}

#endif

// === 向量访问：三后端共用 ===
// v2/v3/v4 在三个后端都有 .x/.y/.z/.w 成员与对应分量构造器，
// 共用一套实现可消除分支间漂移。

[[nodiscard]] inline f32 component(const v2& v, u32 i) {
	assert(i < 2);
	switch (i) {
	case 0: return v.x;
	default: return v.y;
	}
}

[[nodiscard]] inline f32 component(const v3& v, u32 i) {
	assert(i < 3);
	switch (i) {
	case 0: return v.x;
	case 1: return v.y;
	default: return v.z;
	}
}

[[nodiscard]] inline f32 component(const v4& v, u32 i) {
	assert(i < 4);
	switch (i) {
	case 0: return v.x;
	case 1: return v.y;
	case 2: return v.z;
	default: return v.w;
	}
}

#if defined(PRIMAL_MATH_BACKEND_DIRECTXMATH)
// v2a（XMFLOAT2A）与 v2（XMFLOAT2）在 DirectXMath 下是不同类型，补齐对称重载。
[[nodiscard]] inline f32 component(const v2a& v, u32 i) {
	assert(i < 2);
	switch (i) {
	case 0: return v.x;
	default: return v.y;
	}
}
#endif

inline void set_component(v2& v, u32 i, f32 x) {
	assert(i < 2);
	switch (i) {
	case 0: v.x = x; break;
	default: v.y = x; break;
	}
}

inline void set_component(v3& v, u32 i, f32 x) {
	assert(i < 3);
	switch (i) {
	case 0: v.x = x; break;
	case 1: v.y = x; break;
	default: v.z = x; break;
	}
}

inline void set_component(v4& v, u32 i, f32 x) {
	assert(i < 4);
	switch (i) {
	case 0: v.x = x; break;
	case 1: v.y = x; break;
	case 2: v.z = x; break;
	default: v.w = x; break;
	}
}

#if defined(PRIMAL_MATH_BACKEND_DIRECTXMATH)
inline void set_component(v2a& v, u32 i, f32 x) {
	assert(i < 2);
	switch (i) {
	case 0: v.x = x; break;
	default: v.y = x; break;
	}
}
#endif

[[nodiscard]] inline v4 make_v4(const v3& xyz, f32 w) {
	return v4{ xyz.x, xyz.y, xyz.z, w };
}

[[nodiscard]] inline v3 xyz(const v4& v) {
	return v3{ v.x, v.y, v.z };
}

[[nodiscard]] inline v2 splat2(f32 s) {
	return v2{ s, s };
}

[[nodiscard]] inline v3 splat3(f32 s) {
	return v3{ s, s, s };
}

[[nodiscard]] inline v4 splat4(f32 s) {
	return v4{ s, s, s, s };
}

// 模板代码的列访问：矩阵类型由模板参数决定、可能不是 m4x4 时使用（例如
// 单元测试以 `struct Matrix { V columns[4]; }` 实例化 Lumen 的边界点打包
// 模板）。有 .columns 成员的类型（simd/可移植 m4x4、自定义矩阵）直接取列，
// 行为与原写法逐字节相同；没有的（DirectXMath 的 XMFLOAT4X4A）走 column()。
// 用 C++17 写法检测成员：ContentTools 以 C++17 编译，也会包含本头。
namespace detail {
template<class M, class = void>
struct has_columns_member : std::false_type {};
template<class M>
struct has_columns_member<M, std::void_t<decltype(std::declval<const M&>().columns[0])>>
	: std::true_type {};
} // namespace detail

template<class M>
[[nodiscard]] inline auto column_of(const M& m, u32 c) {
	if constexpr (detail::has_columns_member<M>::value) {
		return m.columns[c];
	} else {
		return column(m, c);
	}
}

} // namespace primal::math
