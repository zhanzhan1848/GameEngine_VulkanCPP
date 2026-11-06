#pragma once

#include "../Common/PrimitiveTypes.h"

// === 数学后端选择（三个分支互斥） ===
//   PRIMAL_MATH_BACKEND_DIRECTXMATH : Windows x64 → DirectXMath（XMFLOAT* POD 类型）
//   PRIMAL_MATH_BACKEND_SIMD        : Apple    → <simd/simd.h>
//   PRIMAL_MATH_BACKEND_PORTABLE    : 其余（Linux / Emscripten）→ 可移植结构体
// 数学相关代码的条件编译一律看这三个后端宏，不看平台宏。
#if defined(_WIN64)
	#define PRIMAL_MATH_BACKEND_DIRECTXMATH 1
#elif defined(__APPLE__)
	#define PRIMAL_MATH_BACKEND_SIMD 1
#else
	#define PRIMAL_MATH_BACKEND_PORTABLE 1
#endif

#if defined(PRIMAL_MATH_BACKEND_SIMD)
#include <simd/simd.h>
#endif

#if defined(PRIMAL_MATH_BACKEND_DIRECTXMATH)
#include <DirectXMath.h>
// XMFLOAT* 类型没有运算符：MathDirectXOps.h 在 namespace DirectX 中补齐与
// 可移植结构体一一对应的运算符（靠参数相关查找 ADL 生效）。必须在全局作用域
// include——若放进 namespace primal::math 内，会错误地创建嵌套命名空间
// primal::math::DirectX，ADL 将找不到这些运算符。
#include "MathDirectXOps.h"
#endif

namespace primal::math {
constexpr f32 pi{ 3.1415926535897932384626433832795f };
constexpr f32 half_pi{ pi * 0.5f };
constexpr f32 two_pi{ 2.f * pi };
constexpr f32 epsilon{ 1e-5f };

#if defined(PRIMAL_MATH_BACKEND_SIMD)
	using v2 = simd::float2;
	using v2a = simd::float2;
	using v3 = simd::float3;
	using v3a = simd::float3;
	using v4 = simd::float4;
	using v4a = simd::float4;
	using u32v2 = simd::uint2;
	using u32v3 = simd::uint3;
	using u32v4 = simd::uint4;
	using s32v2 = simd::int2;
	using s32v3 = simd::int3;
	using s32v4 = simd::int4;
	using m3x3 = simd::float3x3;
	using m4x4 = simd::float4x4;
	using m4x4a = simd::float4x4;

	// 本机 clang 实测（arm64, macOS SDK simd/simd.h）。
	static_assert(sizeof(v2) == 8, "simd::float2 must stay 8 bytes");
	static_assert(sizeof(v3) == 16 && alignof(v3) == 16, "simd::float3 must stay 16/16");
	static_assert(sizeof(v4) == 16 && alignof(v4) == 16, "simd::float4 must stay 16/16");
	static_assert(sizeof(m4x4) == 64 && alignof(m4x4) == 16, "simd::float4x4 must stay 64/16");
	static_assert(sizeof(u32v4) == 16 && alignof(u32v4) == 16, "simd::uint4 must stay 16/16");
	static_assert(sizeof(s32v3) == 16 && alignof(s32v3) == 16, "simd::int3 must stay 16/16");
#elif defined(PRIMAL_MATH_BACKEND_DIRECTXMATH)
	// DirectXMath（RH、行向量、行主序）：DXM 矩阵的第 i 行 == 引擎矩阵的
	// 第 i 列，两者内存字节完全一致，GPU 上传无需转置。
	// 限制与对策：
	//   - XMFLOAT* 没有运算符 → MathDirectXOps.h 在 namespace DirectX 中
	//     按可移植结构体一一对应的集合补齐运算符（ADL 生效）；
	//   - 没有 .columns 成员 → MathAccess.h 提供 column/elem/make_m4x4；
	//   - XMFLOAT3 只有 12 字节，与 simd::float3（16 字节）布局不一致，
	//     因此 v3/v3a 统一用 XMFLOAT3A（16 字节、16 字节对齐）。
	using v2 = DirectX::XMFLOAT2;
	using v2a = DirectX::XMFLOAT2A;
	using v3 = DirectX::XMFLOAT3A;
	using v3a = DirectX::XMFLOAT3A;
	using v4 = DirectX::XMFLOAT4A;
	using v4a = DirectX::XMFLOAT4A;
	using u32v2 = DirectX::XMUINT2;
	using u32v3 = DirectX::XMUINT3;
	using u32v4 = DirectX::XMUINT4;
	using s32v2 = DirectX::XMINT2;
	using s32v3 = DirectX::XMINT3;
	using s32v4 = DirectX::XMINT4;
	using m3x3 = DirectX::XMFLOAT3X3;
	using m4x4 = DirectX::XMFLOAT4X4A;
	using m4x4a = DirectX::XMFLOAT4X4A;

	// Microsoft Learn 文档布局（DirectXMath XMFLOAT* 各页）。
	static_assert(sizeof(v2) == 8, "XMFLOAT2 must stay 8 bytes");
	// 注意：16 字节对齐会把 sizeof 抬到 16（对齐填充）。simd/可移植分支的
	// v2a 是 8 字节——v2a 不得跨后端按字节复制（v2 本身跨后端一致，均为 8）。
	static_assert(sizeof(v2a) == 16 && alignof(v2a) == 16, "XMFLOAT2A must stay 16/16");
	static_assert(sizeof(v3) == 16 && alignof(v3) == 16, "XMFLOAT3A must stay 16/16");
	static_assert(sizeof(v4) == 16 && alignof(v4) == 16, "XMFLOAT4A must stay 16/16");
	static_assert(sizeof(m4x4) == 64 && alignof(m4x4) == 16, "XMFLOAT4X4A must stay 64/16");
	// 已知跨后端布局差异：m3x3 在 simd/可移植分支是 48 字节（v3 列含填充），
	// XMFLOAT3X3 是 36 字节（3x3 float 紧排）。m3x4/m3x3 数据不得跨后端按字节复制。
	static_assert(sizeof(m3x3) == 36, "XMFLOAT3X3 must stay 36 bytes");
#endif

// Linux / Emscripten（及其他未特化平台）：可移植结构体实现。
// 本实现的内存布局与 Apple simd 对齐：v3 为 16 字节（第 4 分量作填充）、
// m4x4 列主序（columns[i] 是第 i 列）。
#if defined(PRIMAL_MATH_BACKEND_PORTABLE)
	struct v2 {
		float x, y;
		constexpr v2() : x(0), y(0) {}
		constexpr v2(float x, float y) : x(x), y(y) {}
		constexpr explicit v2(float s) : x(s), y(s) {}
		float& operator[](int i) { return i == 0 ? x : y; }
		constexpr float operator[](int i) const { return i == 0 ? x : y; }
		v2 operator+(const v2& o) const { return {x + o.x, y + o.y}; }
		v2 operator-(const v2& o) const { return {x - o.x, y - o.y}; }
		// Scalar add/sub broadcast — Apple simd::floatN provides these as
		// built-in vector/scalar operators; portable branch mirrors that set
		// (ContentTools SDF/CardBake rely on v3 ± f32, MSVC C2678/C2679 without).
		v2 operator+(float s) const { return {x + s, y + s}; }
		v2 operator-(float s) const { return {x - s, y - s}; }
		v2 operator*(float s) const { return {x * s, y * s}; }
		v2 operator/(float s) const { return {x / s, y / s}; }
		v2& operator+=(const v2& o) { x += o.x; y += o.y; return *this; }
		v2& operator-=(const v2& o) { x -= o.x; y -= o.y; return *this; }
		v2& operator+=(float s) { x += s; y += s; return *this; }
		v2& operator-=(float s) { x -= s; y -= s; return *this; }
	};
	using v2a = v2;

	// 16-byte aligned to match Apple simd::float3 layout (4 floats, w unused)
	struct alignas(16) v3 {
		float x, y, z;
		float _pad;
		constexpr v3() : x(0), y(0), z(0), _pad(0) {}
		constexpr v3(float x, float y, float z) : x(x), y(y), z(z), _pad(0) {}
		constexpr explicit v3(float s) : x(s), y(s), z(s), _pad(0) {}
		float& operator[](int i) { switch(i) { case 0: return x; case 1: return y; case 2: return z; default: return _pad; } }
		constexpr float operator[](int i) const { switch(i) { case 0: return x; case 1: return y; case 2: return z; default: return _pad; } }
		v3 operator+(const v3& o) const { return {x + o.x, y + o.y, z + o.z}; }
		v3 operator-(const v3& o) const { return {x - o.x, y - o.y, z - o.z}; }
		v3 operator+(float s) const { return {x + s, y + s, z + s}; }
		v3 operator-(float s) const { return {x - s, y - s, z - s}; }
		v3 operator*(float s) const { return {x * s, y * s, z * s}; }
		v3 operator/(float s) const { return {x / s, y / s, z / s}; }
		v3 operator*(const v3& o) const { return {x * o.x, y * o.y, z * o.z}; }
		v3& operator+=(const v3& o) { x += o.x; y += o.y; z += o.z; return *this; }
		v3& operator-=(const v3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
		v3& operator+=(float s) { x += s; y += s; z += s; return *this; }
		v3& operator-=(float s) { x -= s; y -= s; z -= s; return *this; }
		v3& operator/=(float s) { x /= s; y /= s; z /= s; return *this; }
		bool operator==(const v3& o) const { return x == o.x && y == o.y && z == o.z; }
	};
	using v3a = v3;

	inline v3 operator-(const v3& v) { return {-v.x, -v.y, -v.z}; }
	inline v3 operator*(float s, const v3& v) { return {v.x * s, v.y * s, v.z * s}; }
	inline v3 operator+(float s, const v3& v) { return {v.x + s, v.y + s, v.z + s}; }

	struct alignas(16) v4 {
		float x, y, z, w;
		constexpr v4() : x(0), y(0), z(0), w(0) {}
		constexpr v4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
		constexpr explicit v4(float s) : x(s), y(s), z(s), w(s) {}
		constexpr v4(const v3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
		float& operator[](int i) { switch(i) { case 0: return x; case 1: return y; case 2: return z; default: return w; } }
		constexpr float operator[](int i) const { switch(i) { case 0: return x; case 1: return y; case 2: return z; default: return w; } }
		v4 operator+(const v4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
		v4 operator-(const v4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
		v4 operator+(float s) const { return {x + s, y + s, z + s, w + s}; }
		v4 operator-(float s) const { return {x - s, y - s, z - s, w - s}; }
		v4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
		v4 operator/(float s) const { return {x / s, y / s, z / s, w / s}; }
		v4& operator+=(const v4& o) { x += o.x; y += o.y; z += o.z; w += o.w; return *this; }
		v4& operator+=(float s) { x += s; y += s; z += s; w += s; return *this; }
		v4& operator-=(float s) { x -= s; y -= s; z -= s; w -= s; return *this; }
		v4& operator/=(float s) { x /= s; y /= s; z /= s; w /= s; return *this; }
		bool operator==(const v4& o) const { return x == o.x && y == o.y && z == o.z && w == o.w; }
	};
	using v4a = v4;

	struct u32v2 { u32 x, y; };
	struct u32v3 { u32 x, y, z; };
	struct u32v4 { u32 x, y, z, w; };
	struct s32v2 { s32 x, y; };
	struct s32v3 { s32 x, y, z; };
	struct s32v4 { s32 x, y, z, w; };

	struct m3x3 {
		v3 columns[3];
		v3& operator[](int i) { return columns[i]; }
		const v3& operator[](int i) const { return columns[i]; }
	};

	struct m4x4 {
		v4 columns[4];
		v4& operator[](int i) { return columns[i]; }
		const v4& operator[](int i) const { return columns[i]; }
		m4x4 operator*(const m4x4& b) const {
			const m4x4& a = *this;
			m4x4 r;
			for (int col = 0; col < 4; ++col) {
				for (int row = 0; row < 4; ++row) {
					r.columns[col][row] =
						a.columns[0][row] * b.columns[col][0] +
						a.columns[1][row] * b.columns[col][1] +
						a.columns[2][row] * b.columns[col][2] +
						a.columns[3][row] * b.columns[col][3];
				}
			}
			return r;
		}
		v4 operator*(const v4& v) const {
			const m4x4& m = *this;
			return {
				m.columns[0][0]*v[0] + m.columns[1][0]*v[1] + m.columns[2][0]*v[2] + m.columns[3][0]*v[3],
				m.columns[0][1]*v[0] + m.columns[1][1]*v[1] + m.columns[2][1]*v[2] + m.columns[3][1]*v[3],
				m.columns[0][2]*v[0] + m.columns[1][2]*v[1] + m.columns[2][2]*v[2] + m.columns[3][2]*v[3],
				m.columns[0][3]*v[0] + m.columns[1][3]*v[1] + m.columns[2][3]*v[2] + m.columns[3][3]*v[3]
			};
		}
	};
	using m4x4a = m4x4;

	// 非后端机器上经 probe 编译单元（系统头 Apple 配置解析后 #undef __APPLE__）
	// 实测：v2 8/4、v3 16/16、v4 16/16、m4x4 64/16、m3x3 48/16。
	static_assert(sizeof(v2) == 8, "portable v2 must stay 8 bytes");
	static_assert(sizeof(v3) == 16 && alignof(v3) == 16, "portable v3 must stay 16/16");
	static_assert(sizeof(v4) == 16 && alignof(v4) == 16, "portable v4 must stay 16/16");
	static_assert(sizeof(m4x4) == 64 && alignof(m4x4) == 16, "portable m4x4 must stay 64/16");
	static_assert(sizeof(u32v4) == 16, "portable u32v4 must stay 16 bytes");
#endif
}

// 矩阵/向量元素访问的统一入口（column/elem/component/make_* 等），
// DirectXMath、simd、可移植三后端各一套实现，调用点跨后端不改写。
#include "MathAccess.h"
