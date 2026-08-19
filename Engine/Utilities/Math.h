#pragma once

#include "CommonHeaders.h"
#include "MathTypes.h"
#include <cmath>

#if defined(_MSC_VER)
    #include <intrin.h>
// #elif !defined(__GNUC__) || !defined(__clang__)
//     #include <x86intrin.h>
#elif defined(__APPLE__)
	#include <arm_neon.h>
	#include "CRC64Table.h"
#elif defined(__EMSCRIPTEN__)
	#include "CRC64Table.h"
#endif

namespace primal::math
{
	constexpr bool is_equal(f32 a, f32 b, f32 eps = epsilon)
	{
		f32 diff{ a - b };
		if (diff < 0.f) diff = -diff;
		return diff < eps;
	}

	template<typename T>
	[[nodiscard]] constexpr T clamp(T value, T min, T max)
	{
		return (value < min) ? min : (value > max) ? max : value;
	}

	template<u32 bits>
	[[nodiscard]] constexpr u32 pack_unit_float(f32 f)
	{
		static_assert(bits <= sizeof(u32) * 8);
		assert(f >= 0.f && f <= 1.f);
		constexpr f32 intervals{ (f32)(((u32)1 << bits) - 1) };
		return (u32)(intervals * f + 0.5f);
	}

	template<u32 bits>
	[[nodiscard]] constexpr u32 unpack_to_unit_float(u32 i)
	{
		static_assert(bits <= sizeof(u32) * 8);
		assert(i < ((u32)1 << bits));
		constexpr f32 intervals{ (f32)(((u32)1 << bits) - 1) };
		return (f32)i / intervals;
	}

	template<u32 bits>
	[[nodiscard]] constexpr u32 pack_float(f32 f, f32 min, f32 max)
	{
		assert(min < max);
		assert(f <= max && f >= min);
		const f32 distance{ (f - min) / (max - min) };
		return pack_unit_float<bits>(distance);
	}

	template<u32 bits>
	[[nodiscard]] constexpr f32 unpack_to_float(u32 i, f32 min, f32 max)
	{
		assert(min < max);
		return unpack_to_unit_float<bits>(i) * (max - min) + min;
	}

	// Align by rounding up. Will result in a multiple of 'alignment' that is greater than or equal to 'size'.
	template<u64 alignment>
	[[nodiscard]] constexpr u64 align_size_up(u64 size)
	{
		static_assert(alignment, "Alignment must be non-zero.");
		constexpr u64 mask{ alignment - 1 };
		static_assert(!(alignment & mask), "alignment should be a power of 2.");
		return ((size + mask) & ~mask);
	}

	// Align by rounding down. Will result in a multiple of 'alignment' that is less than or equal to 'size'.
	template<u64 alignment>
	[[nodiscard]] constexpr u64 align_size_down(u64 size)
	{
		static_assert(alignment, "Alignment must be non-zero.");
		constexpr u64 mask{ alignment - 1 };
		static_assert(!(alignment & mask), "alignment should be a power of 2.");
		return (size & ~mask);
	}

	// Align by rounding up. Will result in a multiple of 'alignment' that is greater than or equal to 'size'.
	[[nodiscard]] constexpr u64 align_size_up(u64 size, u64 alignment)
	{
		assert(alignment && "Alignment must be non-zero.");
		const u64 mask{ alignment - 1 };
		assert(!(alignment & mask) && "alignment should be a power of 2.");
		return ((size + mask) & ~mask);
	}

	// Align by rounding down. Will result in a multiple of 'alignment' that is less than or equal to 'size'.
	[[nodiscard]] constexpr u64 align_size_down(u64 size, u64 alignment)
	{
		assert(alignment && "Alignment must be non-zero.");
		const u64 mask{ alignment - 1 };
		assert(!(alignment & mask) && "alignment should be a power of 2.");
		return (size & ~mask);
	}

	[[nodiscard]] constexpr u64 calc_crc32_u64(const u8 *const data, u64 size)
	{
		assert(size >= sizeof(u64));
		u64 crc{ 0 };
		const u8* at{ data };
		const u8 *const end{ data + align_size_down(size, sizeof(u64)) };
		
	#if defined(_MSC_VER)
		while (at < end)
		{
			crc = _mm_crc32_u64(crc, *((const u64*)at));
			at += sizeof(u64);
		}
	#elif defined(__APPLE__)
		while (at < end)
		{
			const u64 val = *((const u64*)at);
			// 使用查找表来计算CRC64
			for(int i = 0; i < 8; ++i)
			{
				const u8 byte = (val >> (i * 8)) & 0xFF;
				crc = (crc >> 8) ^ primal::math::crc64_tab[(crc ^ byte) & 0xFF];
			}
			at += sizeof(u64);
		}
	#else
		// Fallback for Linux, Emscripten, etc.
		while (at < end)
		{
			const u64 val = *((const u64*)at);
			for(int i = 0; i < 8; ++i)
			{
				const u8 byte = (val >> (i * 8)) & 0xFF;
				crc = (crc >> 8) ^ primal::math::crc64_tab[(crc ^ byte) & 0xFF];
			}
			at += sizeof(u64);
		}
	#endif

		return crc;
	}

//	void* alignedAlloc(size_t size, size_t alignment)
//	{
//		void* data = nullptr;
//#if defined(_MSC_VER) || defined(__MINGW32__)
//		data = _aligned_malloc(size, alignment);
//#else
//		int res = posix_memalign(&data, alignment, size);
//		if (res != 0) data = nullptr;
//#endif
//		return data;
//	}
//
//	void alignedFree(void* data)
//	{
//#if defined(_MSC_VER) || defined(__MINGW32__)
//		_aligned_free(data);
//#else
//		free(data);
//#endif
//	}

#if defined(__APPLE__)
	/**
	 * @brief 创建左手坐标系的LookTo视图矩阵
	 * @param eyePosition 摄像机位置
	 * @param eyeDirection 摄像机朝向（已归一化）
	 * @param upDirection 上方向向量
	 * @return 4x4视图矩阵
	 * 
	 * 注意：当eyeDirection和upDirection平行时，会自动选择替代的上方向向量以避免数值不稳定
	 */
	[[nodiscard]] constexpr math::m4x4 createLookToLH(
        const math::v3& eyePosition,
        const math::v3& eyeDirection,
        const math::v3& upDirection)
    {
        using namespace simd;
        
        // 标准化方向向量（Z 轴指向摄像机前方，左手系中Z轴是正向的）
        simd::float3 eye_pos = simd_make_float3(eyePosition.x, eyePosition.y, eyePosition.z);
        simd::float3 eye_dir = simd_make_float3(eyeDirection.x, eyeDirection.y, eyeDirection.z);
        simd::float3 up_dir = simd_make_float3(upDirection.x, upDirection.y, upDirection.z);
        
        simd::float3 zAxis = simd_normalize(eye_dir);
    
        // 检查eyeDirection和upDirection是否平行，避免叉积为零向量
        simd::float3 cross_product = simd_cross(up_dir, zAxis);
        float cross_length_sq = simd_length_squared(cross_product);
        
        // 如果叉积长度的平方小于阈值，说明两向量接近平行
        if (cross_length_sq < 1e-6f) {
            // 选择一个替代的上方向向量
            // 如果zAxis主要沿Y轴，则使用X轴作为替代
            if (abs(zAxis.y) > 0.9f) {
                up_dir = simd_make_float3(1.0f, 0.0f, 0.0f);
            } else {
                up_dir = simd_make_float3(0.0f, 1.0f, 0.0f);
            }
            cross_product = simd_cross(up_dir, zAxis);
        }
        
        // 计算右向量（X 轴）
        simd::float3 xAxis = simd_normalize(cross_product);
        
        // 修正上向量（Y 轴）
        simd::float3 yAxis = simd_cross(zAxis, xAxis);
        
        // 构建旋转矩阵的转置（用于视图矩阵）
        simd::float3x3 rotation_transpose = simd_matrix(
            simd_make_float3(xAxis.x, yAxis.x, zAxis.x),
            simd_make_float3(xAxis.y, yAxis.y, zAxis.y),
            simd_make_float3(xAxis.z, yAxis.z, zAxis.z)
        );
        
        // 计算平移部分
        simd::float3 translation = simd_mul(rotation_transpose, -eye_pos);
        
        // 构建 4x4 视图矩阵
        simd::float4x4 viewMatrix = simd_matrix(
            simd_make_float4(rotation_transpose.columns[0].x, rotation_transpose.columns[0].y, rotation_transpose.columns[0].z, 0.0f),
            simd_make_float4(rotation_transpose.columns[1].x, rotation_transpose.columns[1].y, rotation_transpose.columns[1].z, 0.0f),
            simd_make_float4(rotation_transpose.columns[2].x, rotation_transpose.columns[2].y, rotation_transpose.columns[2].z, 0.0f),
            simd_make_float4(translation.x, translation.y, translation.z, 1.0f)
        );
        
        return viewMatrix;
    }

    [[nodiscard]] constexpr math::m4x4 createPerspectiveFovLH(float fovY, float aspectRatio, float nearZ, float farZ)
    {
        using namespace simd;
        
        float tanHalfFovY = std::tan(fovY * 0.5f);
        float f = 1.0f / tanHalfFovY;  // 焦距缩放因子

        // 构建透视投影矩阵（左手系）
        simd::float4x4 proj = simd_matrix(
            simd_make_float4(f / aspectRatio, 0.0f, 0.0f, 0.0f),  // X 缩放
            simd_make_float4(0.0f, f, 0.0f, 0.0f),                // Y 缩放
            simd_make_float4(0.0f, 0.0f, farZ / (farZ - nearZ), 1.0f),  // Z 缩放和透视分量
            simd_make_float4(0.0f, 0.0f, -(nearZ * farZ) / (farZ - nearZ), 0.0f)  // 平移分量
        );

        // 添加深度范围调整
        // simd::float4x4 depthAdjust = simd_matrix(
        //     simd_make_float4(1.0f, 0.0f, 0.0f, 0.0f),
        //     simd_make_float4(0.0f, 1.0f, 0.0f, 0.0f),
        //     simd_make_float4(0.0f, 0.0f, 0.5f, 0.0f),
        //     simd_make_float4(0.0f, 0.0f, 0.5f, 1.0f)
        // );

        return proj;
    }

    [[nodiscard]] constexpr math::m4x4 createOrthographicLH(float width, float height, float nearZ, float farZ)
    {
        using namespace simd;
        
        // 构建正交投影矩阵（左手系）
        simd::float4x4 proj = simd_matrix(
            simd_make_float4(2.0f / width, 0.0f, 0.0f, 0.0f),              // X 缩放
            simd_make_float4(0.0f, 2.0f / height, 0.0f, 0.0f),             // Y 缩放
            simd_make_float4(0.0f, 0.0f, 1.0f / (farZ - nearZ), 0.0f),     // Z 缩放（左手系）
            simd_make_float4(0.0f, 0.0f, -nearZ / (farZ - nearZ), 1.0f)    // Z 平移
        );

        return proj;
    }
#else
	// 可移植实现（Windows / Emscripten）：与上方 Apple 分支逐列对应（列主序，
	// columns[i] 为第 i 列），语义与数值完全一致。
	[[nodiscard]] inline math::m4x4 createLookToLH(
        const math::v3& eyePosition,
        const math::v3& eyeDirection,
        const math::v3& upDirection)
    {
		auto cross = [](const math::v3& a, const math::v3& b) {
			return math::v3{ a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
		};

		// 标准化方向向量（左手系 Z 轴指向摄像机前方）
		math::v3 zAxis = eyeDirection;
		const f32 zlen = std::sqrt(zAxis.x * zAxis.x + zAxis.y * zAxis.y + zAxis.z * zAxis.z);
		const f32 zinv = 1.0f / zlen;
		zAxis = math::v3{ zAxis.x * zinv, zAxis.y * zinv, zAxis.z * zinv };

		// eyeDirection 与 upDirection 平行时选择替代上方向，避免叉积退化
		math::v3 up = upDirection;
		math::v3 c = cross(up, zAxis);
		if (c.x * c.x + c.y * c.y + c.z * c.z < 1e-6f) {
			up = (std::abs(zAxis.y) > 0.9f) ? math::v3{ 1.0f, 0.0f, 0.0f } : math::v3{ 0.0f, 1.0f, 0.0f };
			c = cross(up, zAxis);
		}
		const f32 cinv = 1.0f / std::sqrt(c.x * c.x + c.y * c.y + c.z * c.z);
		math::v3 xAxis{ c.x * cinv, c.y * cinv, c.z * cinv };
		math::v3 yAxis = cross(zAxis, xAxis);

		// translation = R^T * (-eye)
		const math::v3 translation{
			-(xAxis.x * eyePosition.x + yAxis.x * eyePosition.y + zAxis.x * eyePosition.z),
			-(xAxis.y * eyePosition.x + yAxis.y * eyePosition.y + zAxis.y * eyePosition.z),
			-(xAxis.z * eyePosition.x + yAxis.z * eyePosition.y + zAxis.z * eyePosition.z)
		};

		return math::m4x4{
			math::v4{ xAxis.x, yAxis.x, zAxis.x, 0.0f },
			math::v4{ xAxis.y, yAxis.y, zAxis.y, 0.0f },
			math::v4{ xAxis.z, yAxis.z, zAxis.z, 0.0f },
			math::v4{ translation.x, translation.y, translation.z, 1.0f }
		};
    }

    [[nodiscard]] inline math::m4x4 createPerspectiveFovLH(float fovY, float aspectRatio, float nearZ, float farZ)
    {
        const float tanHalfFovY = std::tan(fovY * 0.5f);
        const float f = 1.0f / tanHalfFovY;

        return math::m4x4{
            math::v4{ f / aspectRatio, 0.0f, 0.0f, 0.0f },
            math::v4{ 0.0f, f, 0.0f, 0.0f },
            math::v4{ 0.0f, 0.0f, farZ / (farZ - nearZ), 1.0f },
            math::v4{ 0.0f, 0.0f, -(nearZ * farZ) / (farZ - nearZ), 0.0f }
        };
    }

    [[nodiscard]] inline math::m4x4 createOrthographicLH(float width, float height, float nearZ, float farZ)
    {
        return math::m4x4{
            math::v4{ 2.0f / width, 0.0f, 0.0f, 0.0f },
            math::v4{ 0.0f, 2.0f / height, 0.0f, 0.0f },
            math::v4{ 0.0f, 0.0f, 1.0f / (farZ - nearZ), 0.0f },
            math::v4{ 0.0f, 0.0f, -nearZ / (farZ - nearZ), 1.0f }
        };
    }
#endif
}
