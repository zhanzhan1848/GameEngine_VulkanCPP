#pragma once

#if defined(_WIN64)
#include "CommonHeaders.h"
#elif defined(__APPLE__)
#include "../Common/PrimitiveTypes.h"
#elif defined(__EMSCRIPTEN__)
#include "../Common/PrimitiveTypes.h"
#endif

#if defined(__APPLE__)
#include <simd/simd.h>
#endif

namespace primal::math {
constexpr f32 pi{ 3.1415926535897932384626433832795f };
constexpr f32 half_pi{ pi * 0.5f };
constexpr f32 two_pi{ 2.f * pi };
constexpr f32 epsilon{ 1e-5f };

#if defined(_WIN64)
	using v2 = DirectX::XMFLOAT2;
	using v2a = DirectX::XMFLOAT2A;
	using v3 = DirectX::XMFLOAT3;
	using v3a = DirectX::XMFLOAT3A;
	using v4 = DirectX::XMFLOAT4;
	using v4a = DirectX::XMFLOAT4A;
	using u32v2 = DirectX::XMUINT2;
	using u32v3 = DirectX::XMUINT3;
	using u32v4 = DirectX::XMUINT4;
	using s32v2 = DirectX::XMINT2;
	using s32v3 = DirectX::XMINT3;
	using s32v4 = DirectX::XMINT4;
	using m3x3 = DirectX::XMFLOAT3X3;
	using m4x4 = DirectX::XMFLOAT4X4;
	using m4x4a = DirectX::XMFLOAT4X4A;
#endif

#if defined(__APPLE__)
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
#endif

#if defined(__EMSCRIPTEN__)
	struct v2 {
		float x, y;
		constexpr v2() : x(0), y(0) {}
		constexpr v2(float x, float y) : x(x), y(y) {}
		constexpr explicit v2(float s) : x(s), y(s) {}
		float& operator[](int i) { return i == 0 ? x : y; }
		constexpr float operator[](int i) const { return i == 0 ? x : y; }
		v2 operator+(const v2& o) const { return {x + o.x, y + o.y}; }
		v2 operator-(const v2& o) const { return {x - o.x, y - o.y}; }
		v2 operator*(float s) const { return {x * s, y * s}; }
		v2 operator/(float s) const { return {x / s, y / s}; }
		v2& operator+=(const v2& o) { x += o.x; y += o.y; return *this; }
		v2& operator-=(const v2& o) { x -= o.x; y -= o.y; return *this; }
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
		v3 operator*(float s) const { return {x * s, y * s, z * s}; }
		v3 operator/(float s) const { return {x / s, y / s, z / s}; }
		v3 operator*(const v3& o) const { return {x * o.x, y * o.y, z * o.z}; }
		v3& operator+=(const v3& o) { x += o.x; y += o.y; z += o.z; return *this; }
		v3& operator-=(const v3& o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
		v3& operator/=(float s) { x /= s; y /= s; z /= s; return *this; }
		bool operator==(const v3& o) const { return x == o.x && y == o.y && z == o.z; }
	};
	using v3a = v3;

	struct v4 {
		float x, y, z, w;
		constexpr v4() : x(0), y(0), z(0), w(0) {}
		constexpr v4(float x, float y, float z, float w) : x(x), y(y), z(z), w(w) {}
		constexpr explicit v4(float s) : x(s), y(s), z(s), w(s) {}
		constexpr v4(const v3& v, float w_) : x(v.x), y(v.y), z(v.z), w(w_) {}
		float& operator[](int i) { switch(i) { case 0: return x; case 1: return y; case 2: return z; default: return w; } }
		constexpr float operator[](int i) const { switch(i) { case 0: return x; case 1: return y; case 2: return z; default: return w; } }
		v4 operator+(const v4& o) const { return {x + o.x, y + o.y, z + o.z, w + o.w}; }
		v4 operator-(const v4& o) const { return {x - o.x, y - o.y, z - o.z, w - o.w}; }
		v4 operator*(float s) const { return {x * s, y * s, z * s, w * s}; }
		v4 operator/(float s) const { return {x / s, y / s, z / s, w / s}; }
		v4& operator+=(const v4& o) { x += o.x; y += o.y; z += o.z; w += o.w; return *this; }
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
#endif
}
