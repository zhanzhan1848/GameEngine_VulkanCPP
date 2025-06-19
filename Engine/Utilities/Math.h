#pragma once

#include "CommonHeaders.h"
#include "MathTypes.h"

#if defined(_MSC_VER)
    #include <intrin.h>
// #elif !defined(__GNUC__) || !defined(__clang__)
//     #include <x86intrin.h>
#elif defined(__APPLE__)
	#include <arm_neon.h>
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
		// Linux和其他平台使用SSE4.2指令
		while (at < end)
		{
			crc = _mm_crc32_u64(crc, *((const u64*)at));
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
}
