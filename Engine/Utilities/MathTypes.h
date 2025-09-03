#pragma once

#if defined(_WIN64)
#include "CommonHeaders.h"
#elif defined(__APPLE__)
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
}
