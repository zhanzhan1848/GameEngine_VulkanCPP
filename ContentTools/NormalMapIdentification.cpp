#include "ToolsCommon.h"

#if defined(_MSC_VER)
#include <DirectXTex.h>

using namespace DirectX;
using namespace Microsoft::WRL;
#elif defined(__APPLE__)
#include "NormalMapIdentification.h"
#endif

namespace primal::tools
{

	constexpr f32 inv255{ 1.f / 255.f };
	constexpr f32 min_avg_length_threshold{ 0.7f };
	constexpr f32 max_avg_length_threshold{ 1.1f };
	constexpr f32 min_avg_z_threshold{ 0.8f };
	constexpr f32 vector_length_sq_rejection_threshold{ min_avg_length_threshold  * min_avg_length_threshold };
	constexpr f32 rejection_ratio_threshold{ 0.33f };

#if defined(_MSC_VER)
	typedef enum DXGI_FORMAT {
		DXGI_FORMAT_UNKNOWN = 0,
		DXGI_FORMAT_R32G32B32A32_TYPELESS = 1,
		DXGI_FORMAT_R32G32B32A32_FLOAT = 2,
		DXGI_FORMAT_R32G32B32A32_UINT = 3,
		DXGI_FORMAT_R32G32B32A32_SINT = 4,
		DXGI_FORMAT_R32G32B32_TYPELESS = 5,
		DXGI_FORMAT_R32G32B32_FLOAT = 6,
		DXGI_FORMAT_R32G32B32_UINT = 7,
		DXGI_FORMAT_R32G32B32_SINT = 8,
		DXGI_FORMAT_R16G16B16A16_TYPELESS = 9,
		DXGI_FORMAT_R16G16B16A16_FLOAT = 10,
		DXGI_FORMAT_R16G16B16A16_UNORM = 11,
		DXGI_FORMAT_R16G16B16A16_UINT = 12,
		DXGI_FORMAT_R16G16B16A16_SNORM = 13,
		DXGI_FORMAT_R16G16B16A16_SINT = 14,
		DXGI_FORMAT_R32G32_TYPELESS = 15,
		DXGI_FORMAT_R32G32_FLOAT = 16,
		DXGI_FORMAT_R32G32_UINT = 17,
		DXGI_FORMAT_R32G32_SINT = 18,
		DXGI_FORMAT_R32G8X24_TYPELESS = 19,
		DXGI_FORMAT_D32_FLOAT_S8X24_UINT = 20,
		DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS = 21,
		DXGI_FORMAT_X32_TYPELESS_G8X24_UINT = 22,
		DXGI_FORMAT_R10G10B10A2_TYPELESS = 23,
		DXGI_FORMAT_R10G10B10A2_UNORM = 24,
		DXGI_FORMAT_R10G10B10A2_UINT = 25,
		DXGI_FORMAT_R11G11B10_FLOAT = 26,
		DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
		DXGI_FORMAT_R8G8B8A8_UNORM = 28,
		DXGI_FORMAT_R8G8B8A8_UNORM_SRGB = 29,
		DXGI_FORMAT_R8G8B8A8_UINT = 30,
		DXGI_FORMAT_R8G8B8A8_SNORM = 31,
		DXGI_FORMAT_R8G8B8A8_SINT = 32,
		DXGI_FORMAT_R16G16_TYPELESS = 33,
		DXGI_FORMAT_R16G16_FLOAT = 34,
		DXGI_FORMAT_R16G16_UNORM = 35,
		DXGI_FORMAT_R16G16_UINT = 36,
		DXGI_FORMAT_R16G16_SNORM = 37,
		DXGI_FORMAT_R16G16_SINT = 38,
		DXGI_FORMAT_R32_TYPELESS = 39,
		DXGI_FORMAT_D32_FLOAT = 40,
		DXGI_FORMAT_R32_FLOAT = 41,
		DXGI_FORMAT_R32_UINT = 42,
		DXGI_FORMAT_R32_SINT = 43,
		DXGI_FORMAT_R24G8_TYPELESS = 44,
		DXGI_FORMAT_D24_UNORM_S8_UINT = 45,
		DXGI_FORMAT_R24_UNORM_X8_TYPELESS = 46,
		DXGI_FORMAT_X24_TYPELESS_G8_UINT = 47,
		DXGI_FORMAT_R8G8_TYPELESS = 48,
		DXGI_FORMAT_R8G8_UNORM = 49,
		DXGI_FORMAT_R8G8_UINT = 50,
		DXGI_FORMAT_R8G8_SNORM = 51,
		DXGI_FORMAT_R8G8_SINT = 52,
		DXGI_FORMAT_R16_TYPELESS = 53,
		DXGI_FORMAT_R16_FLOAT = 54,
		DXGI_FORMAT_D16_UNORM = 55,
		DXGI_FORMAT_R16_UNORM = 56,
		DXGI_FORMAT_R16_UINT = 57,
		DXGI_FORMAT_R16_SNORM = 58,
		DXGI_FORMAT_R16_SINT = 59,
		DXGI_FORMAT_R8_TYPELESS = 60,
		DXGI_FORMAT_R8_UNORM = 61,
		DXGI_FORMAT_R8_UINT = 62,
		DXGI_FORMAT_R8_SNORM = 63,
		DXGI_FORMAT_R8_SINT = 64,
		DXGI_FORMAT_A8_UNORM = 65,
		DXGI_FORMAT_R1_UNORM = 66,
		DXGI_FORMAT_R9G9B9E5_SHAREDEXP = 67,
		DXGI_FORMAT_R8G8_B8G8_UNORM = 68,
		DXGI_FORMAT_G8R8_G8B8_UNORM = 69,
		DXGI_FORMAT_BC1_TYPELESS = 70,
		DXGI_FORMAT_BC1_UNORM = 71,
		DXGI_FORMAT_BC1_UNORM_SRGB = 72,
		DXGI_FORMAT_BC2_TYPELESS = 73,
		DXGI_FORMAT_BC2_UNORM = 74,
		DXGI_FORMAT_BC2_UNORM_SRGB = 75,
		DXGI_FORMAT_BC3_TYPELESS = 76,
		DXGI_FORMAT_BC3_UNORM = 77,
		DXGI_FORMAT_BC3_UNORM_SRGB = 78,
		DXGI_FORMAT_BC4_TYPELESS = 79,
		DXGI_FORMAT_BC4_UNORM = 80,
		DXGI_FORMAT_BC4_SNORM = 81,
		DXGI_FORMAT_BC5_TYPELESS = 82,
		DXGI_FORMAT_BC5_UNORM = 83,
		DXGI_FORMAT_BC5_SNORM = 84,
		DXGI_FORMAT_B5G6R5_UNORM = 85,
		DXGI_FORMAT_B5G5R5A1_UNORM = 86,
		DXGI_FORMAT_B8G8R8A8_UNORM = 87,
		DXGI_FORMAT_B8G8R8X8_UNORM = 88,
		DXGI_FORMAT_R10G10B10_XR_BIAS_A2_UNORM = 89,
		DXGI_FORMAT_B8G8R8A8_TYPELESS = 90,
		DXGI_FORMAT_B8G8R8A8_UNORM_SRGB = 91,
		DXGI_FORMAT_B8G8R8X8_TYPELESS = 92,
		DXGI_FORMAT_B8G8R8X8_UNORM_SRGB = 93,
		DXGI_FORMAT_BC6H_TYPELESS = 94,
		DXGI_FORMAT_BC6H_UF16 = 95,
		DXGI_FORMAT_BC6H_SF16 = 96,
		DXGI_FORMAT_BC7_TYPELESS = 97,
		DXGI_FORMAT_BC7_UNORM = 98,
		DXGI_FORMAT_BC7_UNORM_SRGB = 99,
		DXGI_FORMAT_AYUV = 100,
		DXGI_FORMAT_Y410 = 101,
		DXGI_FORMAT_Y416 = 102,
		DXGI_FORMAT_NV12 = 103,
		DXGI_FORMAT_P010 = 104,
		DXGI_FORMAT_P016 = 105,
		DXGI_FORMAT_420_OPAQUE = 106,
		DXGI_FORMAT_YUY2 = 107,
		DXGI_FORMAT_Y210 = 108,
		DXGI_FORMAT_Y216 = 109,
		DXGI_FORMAT_NV11 = 110,
		DXGI_FORMAT_AI44 = 111,
		DXGI_FORMAT_IA44 = 112,
		DXGI_FORMAT_P8 = 113,
		DXGI_FORMAT_A8P8 = 114,
		DXGI_FORMAT_B4G4R4A4_UNORM = 115,
		DXGI_FORMAT_P208 = 130,
		DXGI_FORMAT_V208 = 131,
		DXGI_FORMAT_V408 = 132,
		DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE = 189,
		DXGI_FORMAT_SAMPLER_FEEDBACK_MIP_REGION_USED_OPAQUE = 190,
		DXGI_FORMAT_FORCE_UINT = 0xffffffff
	};

	struct Image
	{
		u64 width, height, rowPitch, slicePitch;
		u8* pixels;
		DXGI_FORMAT format;
	};

	inline u64 BitsPerPixel(DXGI_FORMAT fmt)
	{
		switch (fmt)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
			return 32;
		default:
			return 0;
		}
	}

	inline u64 BitsPerColor(DXGI_FORMAT fmt)
	{
		switch (fmt)
		{
		case DXGI_FORMAT_R8G8B8A8_UNORM:
		case DXGI_FORMAT_B8G8R8A8_UNORM:
			return 8;
		default:
			return 0;
		}
	}

	inline bool IsBGR(DXGI_FORMAT fmt)
	{
		return fmt == DXGI_FORMAT_B8G8R8A8_UNORM;
	}
#endif

	namespace
	{
		struct color
		{
			f32 r, g, b, a;
			bool is_transparent() const { return a < 0.001f; }
			bool is_blask() const { return r < 0.001f && g < 0.001f && b < 0.001f; }

			color operator+(color c)
			{
				r += c.r; g += c.g; b += c.b; a += c.a;
				return *this;
			}

			color operator+=(color c) { return (*this) + c; }

			color operator*(f32 s)
			{
				r += s; g *= s; b *= s; a *= s;
				return *this;
			}

			color operator*=(f32 s) { return (*this) * s; }
			color operator/=(f32 s) { return (*this) * (1.f / s); }
		};

		using sampler = color(*)(const u8 *const);

		color sample_pixel_rgb(const u8 *const pixel)
		{
			color c{ (f32)pixel[0], (f32)pixel[1], (f32)pixel[2], (f32)pixel[3] };
			return c * inv255;
		}

		color sample_pixel_bgr(const u8 *const pixel)
		{
			color c{ (f32)pixel[2], (f32)pixel[1], (f32)pixel[0], (f32)pixel[3] };
			return c * inv255;
		}

		s32 evaluate_color(color c)
		{
			if (c.is_blask() || c.is_transparent()) return 0;

			math::v3 v{ c.r * 2.f - 1.f, c.g * 2.f - 1.f, c.b * 2.f - 1.f, };
#if defined(_MSC_VER)
			const f32 v_length_sq{ v.x * v.x + v.y * v.y + v.z * v.z };
			return (v.z < 0.f || v_length_sq < vector_length_sq_rejection_threshold) ? -1 : 1;
#elif defined(__clang__)
			const f32 v_length_sq{ v.x * v.x + v.y * v.y + v.z * v.z };
			return (v.z < 0.f || v_length_sq < vector_length_sq_rejection_threshold) ? -1 : 1;
#endif
		}

		bool evaluate_image(const tools::Image *const image, sampler sample)
		{
			constexpr u32 sample_count{ 4096 };
			const size_t image_size{ image->slicePitch };
			const size_t sample_interval{ std::max(image_size / sample_count, (size_t)4) };
			const u32 min_sample_count{ std::max((u32)(image_size / sample_interval) >> 2, (u32)1) };
			const u8 *const pixels{ image->pixels };

			u32 accepted_samples{ 0 };
			u32 rejected_samples{ 0 };
			color average_color{};

			size_t offset{ sample_interval };
			while (offset < image_size)
			{
				const color c{ sample(&pixels[offset]) };
				const s32 result{ evaluate_color(c) };
				if (result < 0)
				{
					++rejected_samples;
				}
				else if (result > 0)
				{
					++accepted_samples;
					average_color += c;
				}

				offset += sample_interval;
			}

			if (accepted_samples >= min_sample_count)
			{
				const f32 rejection_ratio{ (f32)rejected_samples / (f32)accepted_samples };
				if (rejection_ratio > rejection_ratio_threshold) return false;

				average_color /= (f32)accepted_samples;
				math::v3 v{ average_color.r * 2.f - 1.f, average_color.g * 2.f - 1.f, average_color.b * 2.f - 1.f };
#if defined(_MSC_VER)
				const f32 avg_length{ sqrt(v.x * v.x + v.y * v.y + v.z * v.z) };
				const f32 avg_normalized_z{ v.z / avg_length };
#elif defined(__clang__)
				const f32 avg_length{ sqrt(v.x * v.x + v.y * v.y + v.z * v.z) };
				const f32 avg_normalized_z{ v.z / avg_length };
#endif

				return 
					avg_length >= min_avg_length_threshold &&
					avg_length <= max_avg_length_threshold &&
					avg_normalized_z >= min_avg_z_threshold;
			}

			return false;
		}
	} // anonymous namespace

	bool is_normal_map(const tools::Image *const image)
	{
#if defined(_MSC_VER)
		const DXGI_FORMAT image_format{ image->format };
#elif defined(__clang__)
		const tools::MTLPixelFormat image_format{ image->format };
#endif
		if (BitsPerPixel(image_format) != 32 || BitsPerColor(image_format) != 8) return false;

		return evaluate_image(image, IsBGR(image_format) ? sample_pixel_bgr : sample_pixel_rgb);
	}
}