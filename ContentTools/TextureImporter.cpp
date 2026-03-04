#include "ToolsCommon.h"
#include "Content/ContentToEngine.h"
#include "Utilities/IOStream.h"

#if defined(_MSC_VER)
#include <DirectXTex.h>
#include <dxgi1_6.h>

using namespace DirectX;
using namespace Microsoft::WRL;

namespace primal::tools
{

	bool is_normal_map(const Image *const image);

	namespace 
	{
		struct import_error
		{
			enum error_code : u32
			{
				succeeded = 0,
				unknown,
				compress,
				decompress,
				load,
				mipmap_generation,
				max_size_exceeded,
				size_mismatch,
				format_mismatch,
				file_not_found,
				need_six_images,
			};
		};

		struct texture_dimension
		{
			enum dimension : u32
			{
				texture_1d,
				texture_2d,
				texture_3d,
				texture_cube
			};
		};

		struct texture_import_settings
		{
			char*							sources;			// string of one or more file paths separated by semi-colons ';'
			u32								source_count;		// number of file paths
			u32								dimension;
			u32								mip_levels;
			u32								alpha_threshold;
			u32								prefer_bc7;
			u32								output_format;
			u32								compress;
		};

		struct texture_info
		{
			u32								width;
			u32								height;
			u32								array_size;
			u32								mip_levels;
			u32								format;
			u32								import_error;
			u32								flags;
		};

		struct texture_data
		{
			constexpr static u32			max_mip{ 14 };		// support up to 8k textures
			u8*								subresource_data;
			u32								subresource_size;
			u8*								icon;
			u32								icon_size;
			texture_info					info;
			texture_import_settings			import_settings;
		};

		struct d3d11_device 
		{
			ComPtr<ID3D11Device>			device;
			std::mutex						hw_compression_mutex;
		};
		
		std::mutex							device_creation_mutex;
		utl::vector<d3d11_device>			d3d11_devices;

		HMODULE								dxgi_module{ nullptr };
		HMODULE								d3d11_module{ nullptr };

		utl::vector<ComPtr<IDXGIAdapter>> get_adapters_by_performance()
		{
			if (!dxgi_module)
			{
				dxgi_module = LoadLibrary(L"dxgi.dll");
				if (!dxgi_module) return {};
			}
			using PFN_CreateDXGIFactory1 = HRESULT(WINAPI*)(REFIID, void**);
			const PFN_CreateDXGIFactory1 create_dxgi_factory1{ (PFN_CreateDXGIFactory1)((void*)GetProcAddress(dxgi_module, "CreateDXGIFactory1")) };
			if (!create_dxgi_factory1) return {};

			ComPtr<IDXGIFactory7> factory;
			utl::vector<ComPtr<IDXGIAdapter>> adapters;

			if (SUCCEEDED(create_dxgi_factory1(IID_PPV_ARGS(factory.GetAddressOf()))))
			{
				constexpr u32 warp_id{ 0x1414 };

				ComPtr<IDXGIAdapter> adapter;
				for (u32 i{ 0 }; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(adapter.GetAddressOf())) != DXGI_ERROR_NOT_FOUND; ++i)
				{
					if (!adapter) continue;

					DXGI_ADAPTER_DESC desc;
					adapter->GetDesc(&desc);

					if (desc.VendorId != warp_id) adapters.emplace_back(adapter);

					adapter.Reset();
				}
			}

			return adapters;
		}

		void create_device()
		{
			if (d3d11_devices.size()) return;

			if (!d3d11_module)
			{
				d3d11_module = LoadLibrary(L"d3d11.dll");
				if (!d3d11_module) return;
			}

			const  PFN_D3D11_CREATE_DEVICE d3d11_create_device{ (PFN_D3D11_CREATE_DEVICE)((void*)GetProcAddress(d3d11_module, "D3D11CreateDevice")) };
			if (!d3d11_create_device) return;

			u32 create_device_flags{ 0 };
#ifdef _DEBUG
			create_device_flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

			utl::vector<ComPtr<IDXGIAdapter>> adapters{ get_adapters_by_performance() };
			utl::vector<ComPtr<ID3D11Device>>	devices(adapters.size(), nullptr);
			constexpr D3D_FEATURE_LEVEL feature_levels[]{ D3D_FEATURE_LEVEL_11_0 };

			for (u32 i{ 0 }; i < adapters.size(); ++i)
			{
				ID3D11Device** device{ &devices[i] };
				D3D_FEATURE_LEVEL feature_level;
				[[maybe_unused]]
				HRESULT hr{ d3d11_create_device(adapters[i].Get(), adapters[i] ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
					nullptr, create_device_flags, feature_levels, _countof(feature_levels),
					D3D11_SDK_VERSION, device, &feature_level, nullptr) };
				assert(SUCCEEDED(hr));
			}

			for (u32 i{ 0 }; i < devices.size(); ++i)
			{
				// NOTE: we check for valid devices since device creation can fail for adapters that don't support
				//		 the requested feature level (D3D_FEATURE_LEVEL_11_0).
				if (devices[i])
				{
					d3d11_devices.emplace_back();
					d3d11_devices.back().device = devices[i];
				}
			}
		}

		constexpr void set_or_clear_flag(u32& flags, u32 flag, bool set)
		{
			if (set) flags |= flag; else flags &= ~flag;
		}

		constexpr u32 get_max_mip_count(u32 width, u32 height, u32 depth)
		{
			u32 mip_levels{ 1 };
			while (width > 1 || height > 1 || depth > 1)
			{
				width >>= 1;
				height >>= 1;
				depth >>= 1;

				++mip_levels;
			}
			return mip_levels;
		}

		void texture_info_from_metadata(const TexMetadata& metadata, texture_info& info)
		{
			using namespace primal::content;
			const DXGI_FORMAT format{ metadata.format };
			info.format = format;
			info.width = (u32)metadata.width;
			info.height = (u32)metadata.height;
			info.array_size = metadata.IsVolumemap() ? (u32)metadata.depth : (u32)metadata.arraySize;
			info.mip_levels = (u32)metadata.mipLevels;
			set_or_clear_flag(info.flags, texture_flags::has_alpha, HasAlpha(format));
			set_or_clear_flag(info.flags, texture_flags::is_hdr, format == DXGI_FORMAT_BC6H_UF16 || format == DXGI_FORMAT_BC6H_SF16);
			set_or_clear_flag(info.flags, texture_flags::is_premultiplied_alpha, metadata.IsPMAlpha());
			set_or_clear_flag(info.flags, texture_flags::is_cube_map, metadata.IsCubemap());
			set_or_clear_flag(info.flags, texture_flags::is_volume_map, metadata.IsVolumemap());
			set_or_clear_flag(info.flags, texture_flags::is_srgb, IsSRGB(format));
		}

		void copy_subresources(const ScratchImage& scratch, texture_data *const data)
		{
			const TexMetadata& metadata{ scratch.GetMetadata() };
			const Image *const images{ scratch.GetImages() };
			const u32 image_count{ (u32)scratch.GetImageCount() };
			assert(images && metadata.mipLevels && metadata.mipLevels <= texture_data::max_mip);

			u64 subresource_size{ 0 };

			for (u32 i{ 0 }; i < image_count; ++i)
			{
				// 4 x u32 for width, height, rowPitch and SlicePitch
				subresource_size += sizeof(u32) * 4 + images[i].slicePitch;
			}

			if (subresource_size > ~(u32)0)
			{
				// Support up to 4GB per resource
				data->info.import_error = import_error::max_size_exceeded;
				return;
			}

			data->subresource_size = (u32)subresource_size;
			data->subresource_data = (u8 *const)CoTaskMemRealloc(data->subresource_data, subresource_size);
			assert(data->subresource_data);
			utl::blob_stream_writer blob{ data->subresource_data, data->subresource_size };

			for (u32 i{ 0 }; i < image_count; ++i)
			{
				const Image& image{ images[i] };
				blob.write((u32)image.width);
				blob.write((u32)image.height);
				blob.write((u32)image.rowPitch);
				blob.write((u32)image.slicePitch);
				blob.write(image.pixels, image.slicePitch);
			}
		}

		[[nodiscard]] utl::vector<Image> subsource_data_to_images(texture_data *const data)
		{
			assert(data && data->subresource_data && data->subresource_size);
			assert(data->info.mip_levels && data->info.mip_levels <= texture_data::max_mip);
			assert(data->info.array_size);

			const texture_info& info{ data->info };
			u32 image_count{ info.array_size };

			if (info.flags & content::texture_flags::is_volume_map)
			{
				u32 depth_per_mip_level{ info.array_size };
				for (u32 i{ 0 }; i < info.mip_levels; ++i)
				{
					depth_per_mip_level = std::max(depth_per_mip_level >> 1, (u32)1);
					image_count += depth_per_mip_level;
				}
			}
			else 
			{
				image_count *= info.mip_levels;
			}

			utl::blob_stream_reader blob{ data->subresource_data };
			utl::vector<Image> images(image_count);

			for (u32 i{ 0 }; i < image_count; ++i)
			{
				Image image{};
				image.width = blob.read<u32>();
				image.height = blob.read<u32>();
				image.format = (DXGI_FORMAT)info.format;
				image.rowPitch = blob.read<u32>();
				image.slicePitch = blob.read<u32>();
				image.pixels = (u8*)blob.position();

				blob.skip(image.slicePitch);
				images[i] = image;
			}

			return images;
		}

		void copy_icon(const Image& bc_image, texture_data *const data)
		{
			ScratchImage scratch;
			if (FAILED(Decompress(bc_image, DXGI_FORMAT_UNKNOWN, scratch)))
			{
				return;
			}

			assert(scratch.GetImages());
			const Image& image{ scratch.GetImages()[0]};

			// 4 x u32 for width, height, rowPitch and SlicePitch
			data->icon_size = (u32)(sizeof(u32) * 4 + image.slicePitch);
			data->icon = (u8 *const)CoTaskMemRealloc(data->icon, data->icon_size);
			assert(data->icon);

			utl::blob_stream_writer blob{ data->icon, data->icon_size };
			blob.write((u32)image.width);
			blob.write((u32)image.height);
			blob.write((u32)image.rowPitch);
			blob.write((u32)image.slicePitch);
			blob.write(image.pixels, image.slicePitch);
		}

		[[nodiscard]] ScratchImage load_from_file(texture_data *const data, const char* file_name)
		{
			using namespace primal::content;
			assert(file_exists(file_name));
			if (!file_exists(file_name))
			{
				data->info.import_error = import_error::file_not_found;
				return {};
			}

			data->info.import_error = import_error::load;

			WIC_FLAGS wic_flags{ WIC_FLAGS_NONE };
			TGA_FLAGS tga_flags{ TGA_FLAGS_NONE };

			if (data->import_settings.output_format == DXGI_FORMAT_BC4_UNORM ||
				data->import_settings.output_format == DXGI_FORMAT_BC5_UNORM)
			{
				wic_flags |= WIC_FLAGS_IGNORE_SRGB;
				tga_flags |= TGA_FLAGS_IGNORE_SRGB;
			}

			const std::wstring wfile{ to_wstring(file_name) };
			const wchar_t *const file{ wfile.c_str() };
			ScratchImage scratch;

			// Try one of WIC formats first(e.g. BMP, JPEG, PNG, etc.).
			wic_flags |= WIC_FLAGS_FORCE_RGB;
			HRESULT hr{ LoadFromWICFile(file, wic_flags, nullptr, scratch) };

			// It wasn't a WIC format. Try TGA.
			if (FAILED(hr))
			{
				hr = LoadFromTGAFile(file, tga_flags, nullptr, scratch);
			}

			// It wasn't a TGA either. Try HDR.
			if (FAILED(hr))
			{
				hr = LoadFromHDRFile(file, nullptr, scratch);
				if (SUCCEEDED(hr)) data->info.flags |= texture_flags::is_hdr;
			}

			// It wasn't HDR. Try DDS.
			if (FAILED(hr))
			{
				hr = LoadFromDDSFile(file, DDS_FLAGS_FORCE_RGB, nullptr, scratch);
				if (SUCCEEDED(hr))
				{
					data->info.import_error = import_error::decompress;
					ScratchImage mip_scratch;
					hr = Decompress(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(),
						DXGI_FORMAT_UNKNOWN, mip_scratch);

					if (SUCCEEDED(hr))
					{
						scratch = std::move(mip_scratch);
					}
				}
			}

			if (SUCCEEDED(hr))
			{
				data->info.import_error = import_error::succeeded;
			}

			return scratch;
		}

		[[nodiscard]] ScratchImage initialize_from_images(texture_data *const data, const utl::vector<Image>& images)
		{
			assert(data);
			const texture_import_settings& settings{ data->import_settings };

			ScratchImage scratch;
			HRESULT hr{ S_OK };
			const u32 array_size{ (u32)images.size() };

			// Scope for working scratch
			{
				ScratchImage working_scratch{};

				if (settings.dimension == texture_dimension::texture_1d ||
					settings.dimension == texture_dimension::texture_2d)
				{
					const bool allow_1d{ settings.dimension == texture_dimension::texture_1d };
					assert(array_size >= 1 && images.size() >= 1);
					hr = working_scratch.InitializeArrayFromImages(images.data(), images.size(), allow_1d);
				}
				else if (settings.dimension == texture_dimension::texture_cube)
				{
					if (array_size % 6)
					{
						data->info.import_error = import_error::need_six_images;
						return {};
					}
					hr = working_scratch.InitializeCubeFromImages(images.data(), images.size());
				}
				else
				{
					assert(settings.dimension == texture_dimension::texture_3d);
					hr = working_scratch.Initialize3DFromImages(images.data(), images.size());
				}

				if (FAILED(hr))
				{
					data->info.import_error = import_error::unknown;
					return {};
				}

				scratch = std::move(working_scratch);
			}

			if (settings.mip_levels != 1)
			{
				ScratchImage mip_scratch;
				const TexMetadata& metadata{ scratch.GetMetadata() };
				u32 mip_levels{ math::clamp(settings.mip_levels, (u32)0, get_max_mip_count((u32)metadata.width, (u32)metadata.height, (u32)metadata.depth)) };

				if (settings.dimension != texture_dimension::texture_3d)
				{
					hr = GenerateMipMaps(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(),
						TEX_FILTER_DEFAULT, mip_levels, mip_scratch);
				}
				else
				{
					hr = GenerateMipMaps3D(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(),
						TEX_FILTER_DEFAULT, mip_levels, mip_scratch);
				}

				if (FAILED(hr))
				{
					data->info.import_error = import_error::mipmap_generation;
					return {};
				}

				scratch = std::move(mip_scratch);
			}

			return scratch;
		}

		DXGI_FORMAT determine_output_format(texture_data *const data, ScratchImage& scratch, const Image *const image)
		{
			assert(data && data->import_settings.compress);
			using namespace primal::content;
			const DXGI_FORMAT image_format{ image->format };
			DXGI_FORMAT output_format{ (DXGI_FORMAT)data->import_settings.output_format };

			// Determine the best block compressed format if import settings
			// don't explicitly specify a format.
			if (output_format != DXGI_FORMAT_UNKNOWN)
			{
				goto _done;
			}

			if ((data->info.flags & texture_flags::is_hdr) ||
				image_format == DXGI_FORMAT_BC6H_UF16 || image_format == DXGI_FORMAT_BC6H_SF16)
			{
				output_format = DXGI_FORMAT_BC6H_UF16;
			}
			// If the source image is gray scale or a single channel block compressed format (BC4),
			// then output format will be BC4
			else if (image_format == DXGI_FORMAT_R8_UNORM || image_format == DXGI_FORMAT_BC4_UNORM || image_format == DXGI_FORMAT_BC4_SNORM)
			{
				output_format = DXGI_FORMAT_BC4_UNORM;
			}
			// Test it the source image is a normal map and if so, use BC5 format for the output
			else if (is_normal_map(image) || image_format == DXGI_FORMAT_BC5_UNORM || image_format == DXGI_FORMAT_BC5_SNORM)
			{
				data->info.flags |= texture_flags::is_imported_as_normal_map;
				output_format = DXGI_FORMAT_BC5_UNORM;

				if (IsSRGB(image_format))
				{
					scratch.OverrideFormat(MakeTypelessUNORM(MakeTypeless(image_format)));
				}
			}
			// We exhausted all options. use an RGBA block compressed format.
			else
			{
				output_format = data->import_settings.prefer_bc7 ? DXGI_FORMAT_BC7_UNORM : 
				 	scratch.IsAlphaAllOpaque() ? DXGI_FORMAT_BC1_UNORM :  DXGI_FORMAT_BC3_UNORM;
			}
		_done:
			assert(IsCompressed(output_format));
			if (HasAlpha(output_format)) data->info.flags |= texture_flags::has_alpha;

			return IsSRGB(image_format) ? MakeSRGB(output_format) : output_format;
		}

		bool can_use_gpu(DXGI_FORMAT format)
		{
			switch (format)
			{
			case DXGI_FORMAT_BC6H_TYPELESS:
			case DXGI_FORMAT_BC6H_UF16:
			case DXGI_FORMAT_BC6H_SF16:
			case DXGI_FORMAT_BC7_TYPELESS:
			case DXGI_FORMAT_BC7_UNORM:
			case DXGI_FORMAT_BC7_UNORM_SRGB:
			{
				std::lock_guard lock{ device_creation_mutex };
				static bool try_once = false;
				if (!try_once)
				{
					try_once = true;
					create_device();
				}

				return d3d11_devices.size() > 0;
			}
			}

			return false;
		}

		[[nodiscard]] ScratchImage compress_image(texture_data *const data, ScratchImage& scratch)
		{
			assert(data && data->import_settings.compress && scratch.GetImages());

			const Image *const image{ scratch.GetImage(0, 0, 0) };
			if (!image)
			{
				data->info.import_error = import_error::unknown;
				return {};
			}

			const DXGI_FORMAT output_format{ determine_output_format(data, scratch, image) };
			HRESULT hr{ S_OK };
			ScratchImage bc_scratch;
			if (can_use_gpu(output_format))
			{
				bool wait{ true };
				while (wait)
				{
					for (u32 i{ 0 }; i < d3d11_devices.size(); ++i)
					{
						if (d3d11_devices[i].hw_compression_mutex.try_lock())
						{
							hr = Compress(d3d11_devices[i].device.Get(), scratch.GetImages(), scratch.GetImageCount(),
								scratch.GetMetadata(), output_format, TEX_COMPRESS_DEFAULT, 1.0f, bc_scratch);
							d3d11_devices[i].hw_compression_mutex.unlock();
							wait = false;
							break;
						}
					}
					if (wait) std::this_thread::sleep_for(std::chrono::microseconds(200));
				}
			}
			else
			{
				hr = Compress(scratch.GetImages(), scratch.GetImageCount(), scratch.GetMetadata(),
					output_format, TEX_COMPRESS_PARALLEL, data->import_settings.alpha_threshold, bc_scratch);
			}

			if (FAILED(hr))
			{
				data->info.import_error = import_error::compress;
				return {};
			}

			return bc_scratch;
		}
	}// anonymous namespace

	void ShutDownTextureTools()
	{
		d3d11_devices.clear();

		if (dxgi_module)
		{
			FreeLibrary(dxgi_module);
			dxgi_module = nullptr;
		}

		if (d3d11_module)
		{
			FreeLibrary(d3d11_module);
			d3d11_module = nullptr;
		}
	}

	EDITOR_INTERFACE void Decompress(texture_data *const data)
	{
		using namespace primal::content;
		assert(data->import_settings.compress);
		texture_info& info{ data->info };
		const DXGI_FORMAT format{ (DXGI_FORMAT)info.format };
		assert(IsCompressed(format));
		utl::vector<Image> images = subsource_data_to_images(data);
		const bool is_3d{ (info.flags & texture_flags::is_volume_map) != 0 };

		TexMetadata metadata{};
		metadata.width = info.width;
		metadata.height = info.height;
		metadata.depth = is_3d ? info.array_size : 1;
		metadata.arraySize = is_3d ? 1 : info.array_size;
		metadata.mipLevels = info.mip_levels;
		metadata.miscFlags = info.flags & texture_flags::is_cube_map ? TEX_MISC_TEXTURECUBE : 0;
		metadata.miscFlags2 = info.flags & texture_flags::is_premultiplied_alpha ?
			TEX_ALPHA_MODE_PREMULTIPLIED :
			info.flags & texture_flags::has_alpha ? TEX_ALPHA_MODE_STRAIGHT : TEX_ALPHA_MODE_OPAQUE;
		metadata.format = format;
		// TODO: 1d
		metadata.dimension = is_3d ? TEX_DIMENSION_TEXTURE3D : TEX_DIMENSION_TEXTURE2D;

		ScratchImage scratch;
		HRESULT hr{ Decompress(images.data(), (size_t)images.size(), metadata, DXGI_FORMAT_UNKNOWN, scratch) };
		if (SUCCEEDED(hr))
		{
			copy_subresources(scratch, data);
			texture_info_from_metadata(scratch.GetMetadata(), data->info);
		}
		else
		{
			info.import_error = import_error::decompress;
		}
	}

	EDITOR_INTERFACE void Import(texture_data *const data)
	{
		// return imported texture data that might be compressed using block compression
		if (1)
		{
			const texture_import_settings& settings{ data->import_settings };
			assert(settings.sources && settings.source_count);

			utl::vector<ScratchImage> scratch_images;
			utl::vector<Image> images;

			u32 width{ 0 };
			u32 height{ 0 };
			DXGI_FORMAT format{};
			utl::vector<std::string> files = split(settings.sources, ';');
			assert(files.size() == settings.source_count);

			for (u32 i{ 0 }; i < settings.source_count; ++i)
			{
				scratch_images.emplace_back(load_from_file(data, files[i].c_str()));
				if (data->info.import_error) return;

				const ScratchImage& scratch{ scratch_images.back() };
				const TexMetadata& metadata{ scratch.GetMetadata() };

				if (i == 0)
				{
					width = (u32)metadata.width;
					height = (u32)metadata.height;
					format = metadata.format;
				}

				// All image sources should have the same size.
				if (width != metadata.width || height != metadata.height)
				{
					data->info.import_error = import_error::size_mismatch;
					return;
				}

				// All image sources should have the same format.
				if (format != metadata.format)
				{
					data->info.import_error = import_error::format_mismatch;
					return;
				}

				const u32 array_size{ (u32)metadata.arraySize };
				const u32 depth{ (u32)metadata.depth };

				for(u32 array_index{0}; array_index < array_size; ++array_index)
					for (u32 depth_index{ 0 }; depth_index < depth; ++depth_index)
					{
						const Image* image{ scratch.GetImage(0, array_index, depth_index) };
						assert(image);

						if (!image)
						{
							data->info.import_error = import_error::unknown;
							return;
						}

						if (width != image->width || height != image->height)
						{
							data->info.import_error = import_error::size_mismatch;
							return;
						}

						images.emplace_back(*image);
					}
			}
			ScratchImage scratch{ initialize_from_images(data, images) };
			if (data->info.import_error) return;

			if (settings.compress)
			{
				ScratchImage bc_scratch{ compress_image(data, scratch) };
				
				if (data->info.import_error) return;

				// Desompress the firast image to be used for the icon
				assert(bc_scratch.GetImages());
				copy_icon(bc_scratch.GetImages()[0], data);
				scratch = std::move(bc_scratch);
			}

			copy_subresources(scratch, data);
			// if (data->info.import_error) return;
			texture_info_from_metadata(scratch.GetMetadata(), data->info);
		}
	}
}
#elif defined(__clang__)

#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

#define NS_PRIVATE_IMPLEMENTATION
#define MTL_PRIVATE_IMPLEMENTATION
#define MTK_PRIVATE_IMPLEMENTATION
#include <Metal/Metal.hpp>
#include <MetalKit/MetalKit.hpp>
#include <Foundation/Foundation.hpp>
#include <CoreServices/CoreServices.h>


#include "NormalMapIdentification.h"

namespace primal::tools
{
	namespace
	{
		struct import_error
		{
			enum error_code : u32
			{
				succeeded = 0,
				unknown,
				compress,
				decompress,
				load,
				mipmap_generation,
				max_size_exceeded,
				size_mismatch,
				format_mismatch,
				file_not_found,
				need_six_images,
			};
		};

		struct texture_dimension
		{
			enum dimension : u32
			{
				texture_1d,
				texture_2d,
				texture_3d,
				texture_cube
			};
		};

		struct texture_import_settings
		{
			char*							sources;			// string of one or more file paths separated by semi-colons ';'
			u32								source_count;		// number of file paths
			u32								dimension;
			u32								mip_levels;
			u32								alpha_threshold;
			u32								prefer_bc7;
			u32								output_format;
			u32								compress;
		};

		struct texture_info
		{
			u32								width;
			u32								height;
			u32								array_size;
			u32								mip_levels;
			u32								format;
			u32								import_error;
			u32								flags;
		};

		struct texture_data
		{
			constexpr static u32			max_mip{ 14 };		// support up to 8k textures
			u8*								subresource_data;
			u32								subresource_size;
			u8*								icon;
			u32								icon_size;
			texture_info					info;
			texture_import_settings			import_settings;
		};

		struct TexMetadata {
			u64 width;
			u64 height;
			u64 depth;
			u64 arraySize;
			u64 mipLevels;
			u64 bitsPerPixel;
			u64 bitsPerComponent;
			tools::MTLPixelFormat format;
			MTL::TextureType dimension;
			bool hasAlpha;
			bool isPMAlpha;
			bool isSRGB;
		};
		
		struct ScratchImage {
			TexMetadata metadata;
			utl::vector<u8> pixels;  // RGBA8 格式
			// 所有图像数据（subresources）的结构化视图（imageCount = array × mip × depth）
			utl::vector<tools::Image> m_images;
			// 总 subresource 数量（用于 m_images 的大小）
			u64 m_nimages;
		};

		struct metal_device
		{
			MTL::Device*					device;
			std::mutex						hw_compression_mutex;
		};

		std::mutex 							device_creation_mutex;
		utl::vector<metal_device> 			metal_devices;
		
		void create_device()
		{
			if (metal_devices.size() > 0) return;

			MTL::Device* device = MTL::CreateSystemDefaultDevice();
			
			if(device)
			{
				metal_devices.emplace_back();
				metal_devices.back().device = device;
			}
		}

		constexpr void set_or_clear_flag(u32& flags, u32 flag, bool set)
		{
			if (set) flags |= flag;
			else flags &= ~flag;
		}

		constexpr u32 get_max_mip_count(u32 width, u32 height, u32 depth)
		{
			u32 mip_levels{ 1 };
			while (width > 1 || height > 1 || depth > 1)
			{
				width >>= 1;
				height >>= 1;
				depth >>= 1;
				++mip_levels;
			}
			return mip_levels;
		}

		bool HasAlpha(CGImageRef image) 
		{
			CGImageAlphaInfo alphaInfo = CGImageGetAlphaInfo(image);
		
			switch (alphaInfo) {
				case kCGImageAlphaPremultipliedLast:
				case kCGImageAlphaPremultipliedFirst:
				case kCGImageAlphaLast:
				case kCGImageAlphaFirst:
				case kCGImageAlphaOnly:
					return true;
		
				case kCGImageAlphaNone:
				case kCGImageAlphaNoneSkipLast:
				case kCGImageAlphaNoneSkipFirst:
				default:
					return false;
			}
		}

		bool HasAlphaChannel(tools::MTLPixelFormat format)
		{
			switch (format) {
				case tools::MTLPixelFormatRGBA8Unorm:
				case tools::MTLPixelFormatRGBA8Unorm_sRGB:
				case tools::MTLPixelFormatRGBA32Float:
					return true;
				// 添加其他格式...
				default:
					return false;
			}
		}

		/**
		 * @brief 检查Metal设备是否支持GPU压缩（对应DirectXTex的can_use_gpu）
		 * @param format Metal像素格式
		 * @return 如果支持GPU压缩返回true
		 */
		bool can_use_gpu_metal(tools::MTLPixelFormat format)
		{
			switch (format)
			{
			case tools::MTLPixelFormatASTC_4x4_LDR:
			case tools::MTLPixelFormatASTC_4x4_sRGB:
			case tools::MTLPixelFormatASTC_6x6_LDR:
			case tools::MTLPixelFormatASTC_6x6_sRGB:
			case tools::MTLPixelFormatASTC_8x8_LDR:
			case tools::MTLPixelFormatASTC_8x8_sRGB:
			{
				std::lock_guard lock{ device_creation_mutex };
				static bool try_once = false;
				if (!try_once)
				{
					try_once = true;
					create_device();
				}

				return metal_devices.size() > 0 && metal_devices[0].device;
			}
			}

			return false;
		}

		u32 GetBytesPerPixel(tools::MTLPixelFormat format)
		{
			switch (format) {
				case tools::MTLPixelFormatRGBA8Unorm:
				case tools::MTLPixelFormatRGBA8Unorm_sRGB:
					return 4;
				case tools::MTLPixelFormatRGBA32Float:
					return 16;
				// 添加其他格式的支持...
				default:
					return 4; // 默认值
			}
		}

		bool IsPMAlpha(CGImageRef image) {
			CGImageAlphaInfo alphaInfo = CGImageGetAlphaInfo(image);
			return (alphaInfo == kCGImageAlphaPremultipliedLast ||
					alphaInfo == kCGImageAlphaPremultipliedFirst);
		}

		bool IsRGB(CGImageRef image) {
			CGColorSpaceRef cs = CGImageGetColorSpace(image);
			if (cs) {
				CGColorSpaceModel model = CGColorSpaceGetModel(cs);
				return model == kCGColorSpaceModelRGB;
			}
			return false;
		}

		/**
		 * @brief 检查Metal像素格式是否为压缩格式
		 * @param format Metal像素格式
		 * @return 如果是压缩格式返回true
		 */
		bool IsCompressed(tools::MTLPixelFormat format)
		{
			switch (format)
			{
				// ASTC压缩格式
				case tools::MTLPixelFormatASTC_4x4_LDR:
				case tools::MTLPixelFormatASTC_4x4_sRGB:
				case tools::MTLPixelFormatASTC_6x6_LDR:
				case tools::MTLPixelFormatASTC_6x6_sRGB:
				case tools::MTLPixelFormatASTC_8x8_LDR:
				case tools::MTLPixelFormatASTC_8x8_sRGB:
				case tools::MTLPixelFormatASTC_10x10_LDR:
				case tools::MTLPixelFormatASTC_10x10_sRGB:
				case tools::MTLPixelFormatASTC_12x12_LDR:
				case tools::MTLPixelFormatASTC_12x12_sRGB:
					return true;
				
				// ETC2压缩格式
				case tools::MTLPixelFormatETC2_RGB8:
				case tools::MTLPixelFormatETC2_RGB8_sRGB:
				case tools::MTLPixelFormatETC2_RGB8A1:
				case tools::MTLPixelFormatETC2_RGB8A1_sRGB:
					return true;
				
				// PVRTC压缩格式
				case tools::MTLPixelFormatPVRTC_RGB_2BPP:
				case tools::MTLPixelFormatPVRTC_RGB_2BPP_sRGB:
				case tools::MTLPixelFormatPVRTC_RGB_4BPP:
				case tools::MTLPixelFormatPVRTC_RGB_4BPP_sRGB:
				case tools::MTLPixelFormatPVRTC_RGBA_2BPP:
				case tools::MTLPixelFormatPVRTC_RGBA_2BPP_sRGB:
				case tools::MTLPixelFormatPVRTC_RGBA_4BPP:
				case tools::MTLPixelFormatPVRTC_RGBA_4BPP_sRGB:
					return true;
				
				// 其他压缩格式
				case tools::MTLPixelFormatBC1_RGBA:
				case tools::MTLPixelFormatBC1_RGBA_sRGB:
				case tools::MTLPixelFormatBC2_RGBA:
				case tools::MTLPixelFormatBC2_RGBA_sRGB:
				case tools::MTLPixelFormatBC3_RGBA:
				case tools::MTLPixelFormatBC3_RGBA_sRGB:
				case tools::MTLPixelFormatBC4_RUnorm:
				case tools::MTLPixelFormatBC4_RSnorm:
				case tools::MTLPixelFormatBC5_RGUnorm:
				case tools::MTLPixelFormatBC5_RGSnorm:
				case tools::MTLPixelFormatBC6H_RGBFloat:
				case tools::MTLPixelFormatBC6H_RGBUfloat:
				case tools::MTLPixelFormatBC7_RGBAUnorm:
				case tools::MTLPixelFormatBC7_RGBAUnorm_sRGB:
					return true;
			}
			
			return false;
		}

		void GenerateMipLevel3D(const tools::Image* srcSlices, u32 srcDepth, u32 srcSliceStart, tools::Image& dst);

		/**
		 * @brief 为3D纹理生成mipmap
		 * @param src 源图像
		 * @param dst 目标图像（输出）
		 * @param levels 要生成的mip级别数
		 * @return 成功返回true，失败返回false
		 */
		bool GenerateMipMaps3D(const ScratchImage& src, ScratchImage& dst, u32 levels)
		{
			const TexMetadata& metadata = src.metadata;
			u32 width = metadata.width;
			u32 height = metadata.height;
			u32 depth = metadata.depth;
			
			// 计算实际的mip级别数
			u32 maxPossibleLevels = get_max_mip_count(width, height, depth);
			u32 mipLevels = std::min(levels, maxPossibleLevels);
			if (mipLevels <= 1) {
				// 不需要生成mipmap
				dst = src;
				return true;
			}
			
			// 初始化目标ScratchImage
			dst.metadata = metadata;
			dst.metadata.mipLevels = mipLevels;
			
			// 计算所有mip级别的总图像数
			u32 totalImages = 0;
			u32 w = width;
			u32 h = height;
			u32 d = depth;
			for (u32 level = 0; level < mipLevels; ++level) {
				totalImages += d;
				w = std::max(1u, w / 2);
				h = std::max(1u, h / 2);
				d = std::max(1u, d / 2);
			}
			
			dst.m_nimages = totalImages;
			dst.m_images.resize(totalImages);
			
			// 计算所有mip级别的总大小
			u64 totalSize = 0;
			w = width;
			h = height;
			d = depth;
			for (u32 level = 0; level < mipLevels; ++level) {
				u32 rowPitch = w * 4; // 假设RGBA8格式，每像素4字节
				u32 slicePitch = rowPitch * h;
				totalSize += slicePitch * d;
				w = std::max(1u, w / 2);
				h = std::max(1u, h / 2);
				d = std::max(1u, d / 2);
			}
			
			// 分配像素数据
			dst.pixels.resize(totalSize);
			
			// 复制第一级mip（原始图像）
			u64 offset = 0;
			u32 imageIndex = 0;
			for (u32 slice = 0; slice < depth; ++slice) {
				const tools::Image& srcImage = src.m_images[slice];
				tools::Image& dstImage = dst.m_images[imageIndex++];
				
				dstImage = srcImage;
				dstImage.pixels = dst.pixels.data() + offset;
				
				// 复制像素数据
				std::memcpy(dstImage.pixels, srcImage.pixels, srcImage.slicePitch);
				offset += srcImage.slicePitch;
			}
			
			// 生成剩余的mip级别
			w = width;
			h = height;
			d = depth;
			u32 prevLevelStart = 0;
			u32 currLevelStart = depth;
			
			for (u32 level = 1; level < mipLevels; ++level) {
				u32 mipWidth = std::max(1u, w / 2);
				u32 mipHeight = std::max(1u, h / 2);
				u32 mipDepth = std::max(1u, d / 2);
				u32 mipRowPitch = mipWidth * 4; // 假设RGBA8格式
				u32 mipSlicePitch = mipRowPitch * mipHeight;
				
				for (u32 slice = 0; slice < mipDepth; ++slice) {
					// 设置当前mip级别的图像
					tools::Image& currMip = dst.m_images[currLevelStart + slice];
					currMip.width = mipWidth;
					currMip.height = mipHeight;
					currMip.format = src.m_images[0].format;
					currMip.rowPitch = mipRowPitch;
					currMip.slicePitch = mipSlicePitch;
					currMip.pixels = dst.pixels.data() + offset;
					
					// 使用3D过滤生成mip级别
					GenerateMipLevel3D(dst.m_images.data() + prevLevelStart, d, slice * 2, currMip);
					
					offset += mipSlicePitch;
				}
				
				prevLevelStart = currLevelStart;
				currLevelStart += mipDepth;
				w = mipWidth;
				h = mipHeight;
				d = mipDepth;
			}
			
			return true;
		}
		
		/**
		 * @brief CPU版本的mip级别生成（作为Metal版本的回退）
		 * @param src 源图像数据
		 * @param dst 目标图像数据
		 */
		void GenerateMipLevelCPU(const tools::Image& src, tools::Image& dst)
		{
			// 原有的CPU实现
			u32 srcWidth = src.width;
			u32 srcHeight = src.height;
			u32 dstWidth = dst.width;
			u32 dstHeight = dst.height;
			u32 bytesPerPixel = GetBytesPerPixel(src.format);
			
			for (u32 y = 0; y < dstHeight; ++y) {
				for (u32 x = 0; x < dstWidth; ++x) {
					u32 srcX = x * 2;
					u32 srcY = y * 2;
					
					// 获取源图像中的4个像素
					u8* p1 = src.pixels + (srcY * src.rowPitch) + (srcX * bytesPerPixel);
					u8* p2 = (srcX + 1 < srcWidth) ? p1 + bytesPerPixel : p1;
					u8* p3 = (srcY + 1 < srcHeight) ? p1 + src.rowPitch : p1;
					u8* p4 = (srcX + 1 < srcWidth && srcY + 1 < srcHeight) ? p3 + bytesPerPixel : p3;
					
					// 目标像素位置
					u8* pDst = dst.pixels + (y * dst.rowPitch) + (x * bytesPerPixel);
					
					// 对每个通道进行平均
					for (u32 c = 0; c < bytesPerPixel; ++c) {
						pDst[c] = (p1[c] + p2[c] + p3[c] + p4[c]) / 4;
					}
				}
			}
		}

		/**
		 * @brief 使用Metal GPU加速生成单个mip级别
		 * @param src 源图像数据
		 * @param dst 目标图像数据
		 */
		void GenerateMipLevel(const tools::Image& src, tools::Image& dst)
		{
			// 确保Metal设备已创建
			create_device();
			
			if (metal_devices.empty() || !metal_devices[0].device) {
				// 如果Metal设备不可用，回退到CPU实现
				GenerateMipLevelCPU(src, dst);
				return;
			}
			
			MTL::Device* device = metal_devices[0].device;
			
			// 获取像素格式
			tools::MTLPixelFormat pixelFormat = tools::MTLPixelFormatRGBA8Unorm;
			switch (src.format) {
				case tools::MTLPixelFormatRGBA8Unorm:
				case tools::MTLPixelFormatBGRA8Unorm:
					pixelFormat = (tools::MTLPixelFormat)src.format;
					break;
				default:
					// 对于不支持的格式，使用CPU实现
					GenerateMipLevelCPU(src, dst);
					return;
			}
			
			// 创建源纹理描述符
			auto* srcDesc = MTL::TextureDescriptor::texture2DDescriptor(
				(MTL::PixelFormat)pixelFormat, src.width, src.height, false);
			srcDesc->setUsage(MTL::TextureUsageShaderRead);
			
			// 创建目标纹理描述符（支持mipmap生成）
			auto* dstDesc = MTL::TextureDescriptor::texture2DDescriptor(
				(MTL::PixelFormat)pixelFormat, src.width, src.height, true);
			dstDesc->setUsage(MTL::TextureUsageShaderRead | MTL::TextureUsageRenderTarget);
			dstDesc->setMipmapLevelCount(2); // 源级别 + 目标级别
			
			// 创建纹理
			auto* srcTexture = device->newTexture(srcDesc);
			auto* dstTexture = device->newTexture(dstDesc);
			
			if (!srcTexture || !dstTexture) {
				// 纹理创建失败，回退到CPU实现
				if (srcTexture) srcTexture->release();
				if (dstTexture) dstTexture->release();
				GenerateMipLevelCPU(src, dst);
				return;
			}
			
			// 上传源图像数据到纹理
			MTL::Region srcRegion = MTL::Region::Make2D(0, 0, src.width, src.height);
			srcTexture->replaceRegion(srcRegion, 0, src.pixels, src.rowPitch);
			
			// 将源纹理数据复制到目标纹理的第0级mip
			auto* commandQueue = device->newCommandQueue();
			auto* commandBuffer = commandQueue->commandBuffer();
			auto* blitEncoder = commandBuffer->blitCommandEncoder();
			
			// 复制源纹理到目标纹理的mip级别0
			blitEncoder->copyFromTexture(srcTexture, NS::UInteger(1), NS::UInteger(0), MTL::Origin::Make(NS::UInteger(0), NS::UInteger(0), NS::UInteger(0)), MTL::Size::Make(src.width, src.height, 1), 
										 dstTexture, NS::UInteger(1), NS::UInteger(0), MTL::Origin::Make(NS::UInteger(0), NS::UInteger(0), NS::UInteger(0)));
			
			// 生成mipmap（这会生成所有mip级别）
			blitEncoder->generateMipmaps(dstTexture);
			blitEncoder->endEncoding();
			
			// 提交并等待完成
			commandBuffer->commit();
			commandBuffer->waitUntilCompleted();
			
			// 从目标纹理的mip级别1读取数据到dst
			MTL::Region dstRegion = MTL::Region::Make2D(0, 0, dst.width, dst.height);
			dstTexture->getBytes(dst.pixels, dst.rowPitch, dstRegion, 1); // mip级别1
			
			// 清理资源
			srcTexture->release();
			dstTexture->release();
			commandQueue->release();
			srcDesc->release();
			dstDesc->release();
		}

		/**
		 * @brief 为2D纹理或立方体贴图生成mipmap
		 * @param src 源图像
		 * @param dst 目标图像（输出）
		 * @param levels 要生成的mip级别数
		 * @return 成功返回true，失败返回false
		 */
		bool GenerateMipMaps(const ScratchImage& src, ScratchImage& dst, u32 levels)
		{
			const TexMetadata& metadata = src.metadata;
			u32 width = metadata.width;
			u32 height = metadata.height;
			u32 arraySize = metadata.arraySize;
			bool isCube = metadata.dimension == MTL::TextureTypeCube;
			
			// 计算实际的mip级别数
			u32 maxPossibleLevels = get_max_mip_count(width, height, 1);
			u32 mipLevels = std::min(levels, maxPossibleLevels);
			if (mipLevels <= 1) {
				// 不需要生成mipmap
				dst.m_nimages = src.m_nimages;
				dst.m_images = std::move(src.m_images);
				dst.metadata = src.metadata;
				dst.pixels = std::move(src.pixels);
				return true;
			}
			
			// 计算所有mip级别的总图像数
			u32 totalImages = arraySize * mipLevels;
			if (isCube) {
				totalImages *= 6; // 立方体贴图有6个面
			}
			
			// 初始化目标ScratchImage
			dst.metadata = metadata;
			dst.metadata.mipLevels = mipLevels;
			dst.m_nimages = totalImages;
			dst.m_images.resize(totalImages);
			
			// 计算所有mip级别的总大小
			u64 totalSize = 0;
			u32 w = width;
			u32 h = height;
			for (u32 level = 0; level < mipLevels; ++level) {
				u32 rowPitch = w * 4; // 假设RGBA8格式，每像素4字节
				u32 slicePitch = rowPitch * h;
				totalSize += slicePitch * arraySize * (isCube ? 6 : 1);
				w = std::max(1u, w / 2);
				h = std::max(1u, h / 2);
			}
			
			// 分配像素数据
			dst.pixels.resize(totalSize);
			
			// 复制第一级mip（原始图像）
			u64 offset = 0;
			u32 imageIndex = 0;
			for (u32 item = 0; item < arraySize * (isCube ? 6 : 1); ++item) {
				const tools::Image& srcImage = src.m_images[item];
				tools::Image& dstImage = dst.m_images[imageIndex++];
				
				dstImage = srcImage;
				dstImage.pixels = dst.pixels.data() + offset;
				
				// 复制像素数据
				std::memcpy(dstImage.pixels, srcImage.pixels, srcImage.slicePitch);
				offset += srcImage.slicePitch;
			}
			
			// 生成剩余的mip级别
			w = width;
			h = height;
			for (u32 level = 1; level < mipLevels; ++level) {
				u32 mipWidth = std::max(1u, w / 2);
				u32 mipHeight = std::max(1u, h / 2);
				u32 mipRowPitch = mipWidth * 4; // 假设RGBA8格式
				u32 mipSlicePitch = mipRowPitch * mipHeight;
				
				for (u32 item = 0; item < arraySize * (isCube ? 6 : 1); ++item) {
					// 获取上一级mip
					const tools::Image& prevMip = dst.m_images[item + (level-1) * arraySize * (isCube ? 6 : 1)];
					
					// 设置当前mip级别的图像
					tools::Image& currMip = dst.m_images[imageIndex++];
					currMip.width = mipWidth;
					currMip.height = mipHeight;
					currMip.format = prevMip.format;
					currMip.rowPitch = mipRowPitch;
					currMip.slicePitch = mipSlicePitch;
					currMip.pixels = dst.pixels.data() + offset;
					
					// 使用双线性过滤生成mip级别
					GenerateMipLevel(prevMip, currMip);
					
					offset += mipSlicePitch;
				}
				
				w = mipWidth;
				h = mipHeight;
			}
			
			return true;
		}

		/**
		 * @brief 使用3D过滤生成单个mip级别
		 * @param srcSlices 源图像切片数组
		 * @param srcDepth 源深度
		 * @param srcSliceStart 源切片起始索引
		 * @param dst 目标图像（输出）
		 */
		void GenerateMipLevel3D(const tools::Image* srcSlices, u32 srcDepth, u32 srcSliceStart, tools::Image& dst)
		{
			// 获取源切片
			const tools::Image& slice1 = srcSlices[srcSliceStart];
			const tools::Image& slice2 = (srcSliceStart + 1 < srcDepth) ? srcSlices[srcSliceStart + 1] : slice1;
			
			// 临时图像用于存储每个切片的降采样结果
			tools::Image tempSlice1;
			tempSlice1.width = dst.width;
			tempSlice1.height = dst.height;
			tempSlice1.format = dst.format;
			tempSlice1.rowPitch = dst.rowPitch;
			tempSlice1.slicePitch = dst.slicePitch;
			
			std::vector<u8> tempPixels(dst.slicePitch);
			tempSlice1.pixels = tempPixels.data();
			
			// 对第一个切片进行降采样
			GenerateMipLevel(slice1, tempSlice1);
			
			if (srcSliceStart + 1 < srcDepth) {
				// 如果有第二个切片，对其进行降采样并与第一个结果混合
				tools::Image tempSlice2 = tempSlice1;
				std::vector<u8> tempPixels2(dst.slicePitch);
				tempSlice2.pixels = tempPixels2.data();
				
				GenerateMipLevel(slice2, tempSlice2);
				
				// 混合两个切片的结果
				for (u32 i = 0; i < dst.slicePitch; ++i) {
					dst.pixels[i] = (tempSlice1.pixels[i] + tempSlice2.pixels[i]) / 2;
				}
			} else {
				// 只有一个切片，直接复制结果
				std::memcpy(dst.pixels, tempSlice1.pixels, dst.slicePitch);
			}
		}

		void texture_info_from_metadata(const TexMetadata& metadata, texture_info& info)
		{
			using namespace primal::content;
			const tools::MTLPixelFormat format{ metadata.format };
			info.format = (u32)format;
			info.width = (u32)metadata.width;
			info.height = (u32)metadata.height;
			info.array_size = (u32)metadata.arraySize;
			info.mip_levels = (u32)metadata.mipLevels;
			set_or_clear_flag(info.flags, texture_flags::has_alpha, metadata.hasAlpha);
			set_or_clear_flag(info.flags, texture_flags::is_hdr, format == tools::MTLPixelFormatRGBA32Float);
			set_or_clear_flag(info.flags, texture_flags::is_premultiplied_alpha, metadata.isPMAlpha);
			set_or_clear_flag(info.flags, texture_flags::is_volume_map, metadata.dimension == MTL::TextureType3D);
			set_or_clear_flag(info.flags, texture_flags::is_cube_map, metadata.dimension == MTL::TextureTypeCube);
			set_or_clear_flag(info.flags, texture_flags::is_srgb, metadata.isSRGB);
		}

		void copy_subresources(const ScratchImage& scratch, texture_data *const data)
		{
			const TexMetadata& metadata{ scratch.metadata };
			const tools::Image *const images{ scratch.m_images.data() };
			const u32 image_count{ (u32)scratch.m_nimages };

			u64 subresource_size = 0;
			for(u32 i{ 0 }; i < image_count; ++i)
			{
				const tools::Image& img{ images[i] };
				subresource_size += sizeof(u32) * 4 + img.slicePitch;
			}

			if (subresource_size > 0xFFFFFFFF)
			{
				data->info.import_error = import_error::max_size_exceeded;
				return;
			}

			data->subresource_size = (u32)subresource_size;
			data->subresource_data = (u8*)malloc(subresource_size);
			assert(data->subresource_data);
			utl::blob_stream_writer blob{ data->subresource_data, subresource_size };

			for (u32 i{ 0 }; i < image_count; ++i)
			{
				const tools::Image& img{ images[i] };
				blob.write((u32)img.width);
				blob.write((u32)img.height);
				blob.write((u32)img.rowPitch);
				blob.write((u32)img.slicePitch);
				blob.write(img.pixels, img.slicePitch);
			}
		}

		[[nodiscard]] utl::vector<tools::Image> subsource_data_to_images(texture_data *const data)
		{
			assert(data && data->subresource_data && data->subresource_size);
			assert(data->info.mip_levels && data->info.mip_levels <= texture_data::max_mip);
			assert(data->info.array_size);

			const texture_info& info{ data->info };
			u32 image_count{ info.array_size };

			if(info.flags & content::texture_flags::is_volume_map)
			{
				u32 depth_per_mip_level{ info.array_size };
				for(u32 i{ 0 }; i < info.mip_levels; ++i)
				{
					depth_per_mip_level = std::max(depth_per_mip_level >> 1, (u32)1);
					image_count += depth_per_mip_level;
				}
			}
			else
			{
				image_count *= info.mip_levels;
			}

			utl::blob_stream_reader blob{ data->subresource_data };
			utl::vector<tools::Image> images(image_count);

			for(u32 i{ 0 }; i < image_count; ++i)
			{
				tools::Image image{};
				image.width = blob.read<u32>();
				image.height = blob.read<u32>();
				image.format = (tools::MTLPixelFormat)info.format;
				image.rowPitch = blob.read<u32>();
				image.slicePitch = blob.read<u32>();
				image.pixels = (u8*)blob.position();

				blob.skip(image.slicePitch);
				images[i] = image;
			}

			return images;
		}

		bool decompress_metal(const tools::Image& image, tools::ScratchImage& outScratch);
		void copy_icon(const tools::Image& bc_image, texture_data *const data)
		{
			ScratchImage scratch;
			if(!decompress_metal(bc_image, scratch))
			{
				return;
			}
			assert(scratch.m_nimages);
			const tools::Image& image{ scratch.m_images[0] };

			// 4 x u32 for width, height, row pitch, slice pitch
			data->icon_size = (u32)(sizeof(u32) * 4 + image.slicePitch);
			data->icon = (u8*)malloc(data->icon_size);
			assert(data->icon);

			utl::blob_stream_writer blob{ data->icon, data->icon_size };
			blob.write((u32)image.width);
			blob.write((u32)image.height);
			blob.write((u32)image.rowPitch);
			blob.write((u32)image.slicePitch);
			blob.write(image.pixels, image.slicePitch);
		}

		[[nodiscard]] ScratchImage load_from_file(texture_data *const data, const char *const file_name)
		{
			using namespace primal::content;
			assert(file_exists(file_name));
			if(!file_exists(file_name))
			{
				data->info.import_error = import_error::file_not_found;
				return {};
			}

			data->info.import_error = import_error::load;

			// 创建一个ScratchImage对象来存储加载的图像数据
			ScratchImage scratch;

			// 创建文件URL
			CFStringRef cfPath = CFStringCreateWithCString(kCFAllocatorDefault, file_name, kCFStringEncodingUTF8);
			CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault, cfPath, kCFURLPOSIXPathStyle, false);
			CFRelease(cfPath);

			if (!url) {
				data->info.import_error = import_error::file_not_found;
				return {};
			}

			// 创建CGImageSource来加载图像
			CGImageSourceRef imageSource = CGImageSourceCreateWithURL(url, NULL);
			CFRelease(url);
			
			if (!imageSource) {
				data->info.import_error = import_error::load;
				return {};
			}

			// 获取图像格式信息
			CFStringRef imageType = CGImageSourceGetType(imageSource);
			bool isHDR = false;

			// 检查是否为HDR格式
			if (imageType && CFStringCompare(imageType, CFSTR("public.hdr"), 0) == kCFCompareEqualTo) {
				isHDR = true;
				data->info.flags |= texture_flags::is_hdr;
			}

			// 获取图像
			CGImageRef image = CGImageSourceCreateImageAtIndex(imageSource, 0, NULL);
			CFRelease(imageSource);

			u64 bpp = CGImageGetBitsPerPixel(image);
			if (bpp != 32) {
				data->info.import_error = import_error::size_mismatch;
				return {};
			}
			
			if (!image) {
				data->info.import_error = import_error::load;
				return {};
			}

			// 获取图像尺寸和属性
			u64 width = CGImageGetWidth(image);
			u64 height = CGImageGetHeight(image);
			u64 bitsPerComponent = CGImageGetBitsPerComponent(image);
			u64 bitsPerPixel = CGImageGetBitsPerPixel(image);
			u64 bytesPerComponent = CGImageGetBitsPerComponent(image);
			bool hasAlpha = CGImageGetAlphaInfo(image) != kCGImageAlphaNone;
			
			// 创建位图上下文以获取像素数据
			CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

			// 确定像素格式
			tools::MTLPixelFormat format;
			if (isHDR) {
				format = tools::MTLPixelFormatRGBA32Float;
			} else if (hasAlpha) {
				format = tools::MTLPixelFormatRGBA8Unorm;
			} else {
				format = tools::MTLPixelFormatRGBA8Unorm; // 即使没有Alpha通道，我们也使用RGBA格式以保持一致性
			}

			// 创建位图上下文
			u64 bytesPerRow = (width * 4 + 15) & ~15;
    		utl::vector<u8> pixelData{ height * bytesPerRow };
			CGBitmapInfo bitmapInfo = kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big;
			CGContextRef context = CGBitmapContextCreate(pixelData.data(), width, height, 8, bytesPerRow, colorSpace, bitmapInfo);
			CGColorSpaceRelease(colorSpace);

			if (!context) {
				CGImageRelease(image);
				data->info.import_error = import_error::load;
				return {};
			}

			// 绘制图像到上下文
			CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);

			// 获取像素数据
			if (pixelData.empty()) {
				CGContextRelease(context);
				CGImageRelease(image);
				data->info.import_error = import_error::load;
				return {};
			}

			// 计算图像大小
			u64 imageSize = height * bytesPerRow;

			// 初始化ScratchImage
			TexMetadata metadata = {};
			metadata.width = width;
			metadata.height = height;
			metadata.depth = 1;
			metadata.arraySize = 1;
			metadata.mipLevels = 1;
			metadata.bitsPerPixel = bitsPerPixel;
			metadata.bitsPerComponent = bitsPerComponent;
			metadata.format = format;
			metadata.dimension = MTL::TextureType2D;
			metadata.hasAlpha = HasAlpha(image);
			metadata.isPMAlpha = IsPMAlpha(image);
			metadata.isSRGB = IsRGB(image);

			scratch.metadata = metadata;
			scratch.pixels = std::move(pixelData);

			scratch.m_nimages = 1; // 单图，没有 mip 没有 array
			scratch.m_images.resize(1);
			// scratch.pixels.resize(imageSize); // 分配扁平像素 buffer
			// std::memcpy(scratch.pixels.data(), pixelData.data(), imageSize);

			// 填充 m_images[0]
			scratch.m_images[0].width = width;
			scratch.m_images[0].height = height;
			scratch.m_images[0].rowPitch = bytesPerRow;
			scratch.m_images[0].slicePitch = imageSize;
			scratch.m_images[0].pixels = scratch.pixels.data();

			// 释放资源
			CGContextRelease(context);
			CGImageRelease(image);
			data->info.import_error = import_error::succeeded;
			return scratch;
		}

		[[nodiscard]] ScratchImage initialize_from_images(texture_data *const data, const utl::vector<tools::Image>& images)
		{
			assert(data);
			const texture_import_settings& settings{ data->import_settings };

			ScratchImage scratch;
			const u32 array_size{ (u32)images.size() };

			// Scope for working scratch
			{
				ScratchImage working_scratch{};

				if(settings.dimension == texture_dimension::texture_1d 
					|| settings.dimension == texture_dimension::texture_2d)
				{
					const bool allow_1d{ settings.dimension == texture_dimension::texture_1d };
					assert(array_size >= 1 && images.size() >= 1);
					
					// 初始化元数据
					TexMetadata metadata = {};
					metadata.width = images[0].width;
					metadata.height = images[0].height;
					metadata.depth = 1;
					metadata.arraySize = array_size;
					metadata.mipLevels = 1;
					metadata.format = images[0].format;
					metadata.dimension = allow_1d && images[0].height == 1 ? 
						MTL::TextureType1D : MTL::TextureType2D;
					metadata.hasAlpha = false; // 将在后续处理中更新
					metadata.isPMAlpha = false; // 将在后续处理中更新
					metadata.isSRGB = false; // 将在后续处理中更新

					// 计算总大小并分配内存
					u64 totalSize = 0;
					for (const auto& img : images) 
					{
						totalSize += img.slicePitch;
					}

					working_scratch.metadata = metadata;
					working_scratch.m_nimages = array_size;
					working_scratch.m_images.resize(array_size);
					working_scratch.pixels.resize(totalSize);

					// 复制图像数据
					u8* destPtr = working_scratch.pixels.data();
					for (u32 i = 0; i < array_size; ++i) {
						const tools::Image& srcImg = images[i];
						tools::Image& destImg = working_scratch.m_images[i];
						
						destImg.width = srcImg.width;
						destImg.height = srcImg.height;
						destImg.format = srcImg.format;
						destImg.rowPitch = srcImg.rowPitch;
						destImg.slicePitch = srcImg.slicePitch;
						destImg.pixels = destPtr;
						
						// 复制像素数据
						std::memcpy(destPtr, srcImg.pixels, srcImg.slicePitch);
						destPtr += srcImg.slicePitch;
						
						// 更新元数据中的Alpha信息
						if (HasAlphaChannel(srcImg.format)) {
							metadata.hasAlpha = true;
						}
					}
				}
				else if (settings.dimension == texture_dimension::texture_cube)
				{
					if (array_size % 6)
					{
						data->info.import_error = import_error::need_six_images;
						return {};
					}
					
					// 初始化元数据
					TexMetadata metadata = {};
					metadata.width = images[0].width;
					metadata.height = images[0].height;
					metadata.depth = 1;
					metadata.arraySize = array_size / 6;
					metadata.mipLevels = 1;
					metadata.format = images[0].format;
					metadata.dimension = MTL::TextureTypeCube;
					metadata.hasAlpha = false; // 将在后续处理中更新
					metadata.isPMAlpha = false; // 将在后续处理中更新
					metadata.isSRGB = false; // 将在后续处理中更新
					
					// 计算总大小并分配内存
					u64 totalSize = 0;
					for (const auto& img : images) {
						totalSize += img.slicePitch;
					}
					
					working_scratch.metadata = metadata;
					working_scratch.m_nimages = array_size;
					working_scratch.m_images.resize(array_size);
					working_scratch.pixels.resize(totalSize);
					
					// 复制图像数据
					u8* destPtr = working_scratch.pixels.data();
					for (u32 i = 0; i < array_size; ++i) {
						const tools::Image& srcImg = images[i];
						tools::Image& destImg = working_scratch.m_images[i];
						
						destImg.width = srcImg.width;
						destImg.height = srcImg.height;
						destImg.format = srcImg.format;
						destImg.rowPitch = srcImg.rowPitch;
						destImg.slicePitch = srcImg.slicePitch;
						destImg.pixels = destPtr;
						
						// 复制像素数据
						std::memcpy(destPtr, srcImg.pixels, srcImg.slicePitch);
						destPtr += srcImg.slicePitch;
						
						// 更新元数据中的Alpha信息
						if (HasAlphaChannel(srcImg.format)) {
							metadata.hasAlpha = true;
						}
					}
				}
				else
				{
					assert(settings.dimension == texture_dimension::texture_3d);
					
					// 初始化元数据
					TexMetadata metadata = {};
					metadata.width = images[0].width;
					metadata.height = images[0].height;
					metadata.depth = array_size;
					metadata.arraySize = 1;
					metadata.mipLevels = 1;
					metadata.format = images[0].format;
					metadata.dimension = MTL::TextureType3D;
					metadata.hasAlpha = false; // 将在后续处理中更新
					metadata.isPMAlpha = false; // 将在后续处理中更新
					metadata.isSRGB = false; // 将在后续处理中更新
					
					// 计算总大小并分配内存
					u64 totalSize = 0;
					for (const auto& img : images) {
						totalSize += img.slicePitch;
					}
					
					working_scratch.metadata = metadata;
					working_scratch.m_nimages = array_size;
					working_scratch.m_images.resize(array_size);
					working_scratch.pixels.resize(totalSize);
					
					// 复制图像数据
					u8* destPtr = working_scratch.pixels.data();
					for (u32 i = 0; i < array_size; ++i) {
						const tools::Image& srcImg = images[i];
						tools::Image& destImg = working_scratch.m_images[i];
						
						destImg.width = srcImg.width;
						destImg.height = srcImg.height;
						destImg.format = srcImg.format;
						destImg.rowPitch = srcImg.rowPitch;
						destImg.slicePitch = srcImg.slicePitch;
						destImg.pixels = destPtr;
						
						// 复制像素数据
						std::memcpy(destPtr, srcImg.pixels, srcImg.slicePitch);
						destPtr += srcImg.slicePitch;
						
						// 更新元数据中的Alpha信息
						if (HasAlphaChannel(srcImg.format)) {
							metadata.hasAlpha = true;
						}
					}
				}
				scratch = std::move(working_scratch);
			}

			if (settings.mip_levels != 1)
			{
				ScratchImage mip_scratch;
				const TexMetadata& metadata{ scratch.metadata };
				u32 mip_levels{ math::clamp(settings.mip_levels, (u32)0, get_max_mip_count((u32)metadata.width, (u32)metadata.height, (u32)metadata.depth)) };

				// 生成mipmap
				if (settings.dimension != texture_dimension::texture_3d)
				{
					// 为2D纹理或立方体贴图生成mipmap
					GenerateMipMaps(scratch, mip_scratch, mip_levels);
				}
				else
				{
					// 为3D纹理生成mipmap
					GenerateMipMaps3D(scratch, mip_scratch, mip_levels);
				}

				if (mip_scratch.m_nimages == 0)
				{
					data->info.import_error = import_error::mipmap_generation;
					return {};
				}

				scratch = std::move(mip_scratch);
			}

			data->info.import_error = import_error::succeeded;

			return scratch;
		}

		/**
		 * @brief 确定Metal纹理的输出格式（对应DirectXTex的determine_output_format）
		 * @param data 纹理数据指针
		 * @param scratch 临时图像数据
		 * @param image 源图像指针
		 * @return 确定的Metal像素格式
		 */
		tools::MTLPixelFormat determine_output_format_metal(texture_data *const data, ScratchImage& scratch, const tools::Image *const image)
		{
			assert(data && data->import_settings.compress);
			using namespace primal::content;
			const tools::MTLPixelFormat image_format{ image->format };
			tools::MTLPixelFormat output_format{ (tools::MTLPixelFormat)data->import_settings.output_format };

			// 如果导入设置明确指定了格式，则使用该格式
			if (output_format != tools::MTLPixelFormatInvalid)
			{
				goto _done;
			}

			// 如果是HDR纹理，使用BC6H等效的Metal格式
			if ((data->info.flags & texture_flags::is_hdr) ||
				image_format == tools::MTLPixelFormatRGBA32Float)
			{
				// Metal没有直接的BC6H等效格式，使用RGBA32Float
				output_format = tools::MTLPixelFormatRGBA32Float;
			}
			// 如果源图像是灰度或单通道格式，使用R8格式
			else if (image_format == tools::MTLPixelFormatR8Unorm)
			{
				output_format = tools::MTLPixelFormatR8Unorm;
			}
			// 测试是否为法线贴图，如果是则使用RG格式
			else if (is_normal_map(image))
			{
				data->info.flags |= texture_flags::is_imported_as_normal_map;
				output_format = tools::MTLPixelFormatBC5_RGUnorm;
			}
			// 默认使用RGBA格式
			else
			{
				// Metal的压缩格式选择
				if (data->import_settings.prefer_bc7)
				{
					output_format = tools::MTLPixelFormatBC7_RGBAUnorm_sRGB;
				}
				else
				{
					// 检查是否有Alpha通道
					bool hasAlpha = HasAlphaChannel(image_format);
					if (hasAlpha)
					{
						output_format = tools::MTLPixelFormatBC3_RGBA_sRGB;
					}
					else
					{
						// 无Alpha通道，可以使用压缩格式
						output_format = tools::MTLPixelFormatASTC_4x4_LDR;
					}
				}
			}

		_done:
			if (HasAlphaChannel(output_format)) data->info.flags |= texture_flags::has_alpha;

			// 处理sRGB格式
			if (scratch.metadata.isSRGB)
			{
				switch (output_format)
				{
				case tools::MTLPixelFormatRGBA8Unorm:
					return tools::MTLPixelFormatRGBA8Unorm_sRGB;
				case tools::MTLPixelFormatASTC_4x4_LDR:
					return tools::MTLPixelFormatASTC_4x4_sRGB;
				default:
					return output_format;
				}
			}

			return output_format;
		}

		/**
		 * @brief 使用astcenc库压缩纹理
		 * @param src 源图像
		 * @param dst 目标压缩图像
		 * @param format 目标格式
		 * @return 成功返回true
		 */
		bool compress_with_astcenc(const tools::Image& src, tools::Image& dst, tools::MTLPixelFormat format)
		{
			// 创建临时文件用于存储源数据和压缩后的数据
			std::string tempInputPath = "/tmp/temp_source_texture.png";
			std::string tempOutputPath = "/tmp/temp_compressed_texture.astc";
			
			// 将源数据写入临时文件
			CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
			CGContextRef context = CGBitmapContextCreate(
				(void*)src.pixels, src.width, src.height, 8, src.rowPitch,
				colorSpace, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
			
			if (!context)
			{
				CGColorSpaceRelease(colorSpace);
				return false;
			}
			
			CGImageRef image = CGBitmapContextCreateImage(context);
			CGContextRelease(context);
			CGColorSpaceRelease(colorSpace);
			
			if (!image)
			{
				return false;
			}
			
			// 保存为PNG文件
			CFURLRef url = CFURLCreateWithFileSystemPath(kCFAllocatorDefault,
				CFStringCreateWithCString(kCFAllocatorDefault, tempInputPath.c_str(), kCFStringEncodingUTF8),
				kCFURLPOSIXPathStyle, false);
			
			if (!url)
			{
				CGImageRelease(image);
				return false;
			}
			
			CGImageDestinationRef destination = CGImageDestinationCreateWithURL(url, kUTTypePNG, 1, NULL);
			CFRelease(url);
			
			if (!destination)
			{
				CGImageRelease(image);
				return false;
			}
			
			CGImageDestinationAddImage(destination, image, NULL);
			CGImageDestinationFinalize(destination);
			CFRelease(destination);
			CGImageRelease(image);
			
			// 构建astcenc命令
			std::string blockSize;
			std::string quality = "-medium"; // 默认使用medium质量
			
			switch (format)
			{
				case tools::MTLPixelFormatASTC_4x4_LDR:
				case tools::MTLPixelFormatASTC_4x4_sRGB:
					blockSize = "4x4";
					break;
				case tools::MTLPixelFormatASTC_6x6_LDR:
				case tools::MTLPixelFormatASTC_6x6_sRGB:
					blockSize = "6x6";
					break;
				case tools::MTLPixelFormatASTC_8x8_LDR:
				case tools::MTLPixelFormatASTC_8x8_sRGB:
					blockSize = "8x8";
					break;
				case tools::MTLPixelFormatBC1_RGBA:
				case tools::MTLPixelFormatBC1_RGBA_sRGB:
				case tools::MTLPixelFormatBC3_RGBA:
				case tools::MTLPixelFormatBC3_RGBA_sRGB:
					// astcenc不支持BC格式，这里需要使用其他工具或库
					return false;
				default:
					return false; // 不支持的格式
			}
			
			// 构建命令行，添加sRGB标志如果需要
			std::string colorProfile = "";
			if (format == tools::MTLPixelFormatASTC_4x4_sRGB ||
				format == tools::MTLPixelFormatASTC_6x6_sRGB ||
				format == tools::MTLPixelFormatASTC_8x8_sRGB ||
				format == tools::MTLPixelFormatBC1_RGBA_sRGB ||
				format == tools::MTLPixelFormatBC3_RGBA_sRGB)
			{
				colorProfile = " -cs";
			}
			else
			{
				colorProfile = " -cl";
			}
			
			std::string cmd = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCP/third_party/astc-encoder/build/Source/astcenc-neon "  + colorProfile + " " 
				+ tempInputPath + " " + tempOutputPath + " " + 
				blockSize + " " + quality;
			
			// 执行命令
			int result = system(cmd.c_str());
			if (result != 0)
			{
				// 命令执行失败
				remove(tempInputPath.c_str());
				return false;
			}
			
			// 读取压缩后的数据
			FILE* file = fopen(tempOutputPath.c_str(), "rb");
			if (!file)
			{
				remove(tempInputPath.c_str());
				return false;
			}
			
			// 获取文件大小
			fseek(file, 0, SEEK_END);
			long fileSize = ftell(file);
			fseek(file, 0, SEEK_SET);
			
			// ASTC文件格式有16字节的头部
			const size_t astcHeaderSize = 16;
			
			// 跳过ASTC头部
			fseek(file, astcHeaderSize, SEEK_SET);
			
			// 计算压缩数据大小
			long compressedSize = fileSize - astcHeaderSize;
			
			// 分配内存并读取压缩数据
			std::vector<u8> compressedData(compressedSize);
			fread(compressedData.data(), 1, compressedSize, file);
			fclose(file);
			
			// 设置目标图像属性
			dst.width = src.width;
			dst.height = src.height;
			dst.format = format;
			
			// 计算压缩后的行间距和切片间距
			u32 blockWidth = 4, blockHeight = 4; // 默认为4x4块
			switch (format)
			{
				case tools::MTLPixelFormatASTC_6x6_LDR:
				case tools::MTLPixelFormatASTC_6x6_sRGB:
					blockWidth = blockHeight = 6;
					break;
				case tools::MTLPixelFormatASTC_8x8_LDR:
				case tools::MTLPixelFormatASTC_8x8_sRGB:
					blockWidth = blockHeight = 8;
					break;
			}
			
			u32 blocksX = (src.width + blockWidth - 1) / blockWidth;
			u32 blocksY = (src.height + blockHeight - 1) / blockHeight;
			
			// ASTC格式每块16字节
			dst.rowPitch = blocksX * 16;
			dst.slicePitch = compressedSize;
			
			// 分配内存并复制压缩数据
			dst.pixels = new u8[compressedSize];
			memcpy(dst.pixels, compressedData.data(), compressedSize);
			
			// 删除临时文件
			remove(tempInputPath.c_str());
			remove(tempOutputPath.c_str());
			
			return true;
		}

		/**
		 * @brief Metal版本的纹理压缩（对应DirectXTex的compress_image）
		 * @param data 纹理数据指针
		 * @param scratch 临时图像数据
		 * @return 压缩后的图像数据
		 */
		[[nodiscard]] ScratchImage compress_image_metal(texture_data *const data, ScratchImage& scratch)
		{
			assert(data && data->import_settings.compress && !scratch.m_images.empty());

			const tools::Image *const image{ &scratch.m_images[0] };
			if (!image)
			{
				data->info.import_error = import_error::unknown;
				return {};
			}

			const tools::MTLPixelFormat output_format{ determine_output_format_metal(data, scratch, image) };
			bool success = false;
			ScratchImage compressed_scratch;
			
			// 初始化压缩后的ScratchImage
			compressed_scratch.metadata = scratch.metadata;
			compressed_scratch.metadata.format = output_format;
			compressed_scratch.m_nimages = scratch.m_nimages;
			compressed_scratch.m_images.resize(scratch.m_nimages);
			
			// 直接使用texturetool进行压缩，不再尝试GPU或CPU压缩
			for (u64 img = 0; img < scratch.m_nimages; ++img)
			{
				success = compress_with_astcenc(scratch.m_images[img], 
					compressed_scratch.m_images[img], output_format);
				if (!success) break;
			}

			if (!success)
			{
				data->info.import_error = import_error::compress;
				return {};
			}

			data->info.import_error = import_error::succeeded;
			return compressed_scratch;
		}

		/**
		 * @brief 使用astcenc解压缩纹理
		 * @param src 源压缩图像
		 * @param dst 目标解压缩图像
		 * @return 成功返回true
		 */
		bool decompress_with_astcenc(const tools::Image& src, tools::Image& dst)
		{
			// 创建临时文件用于存储压缩数据
			std::string tempInputPath = "/tmp/temp_compressed_texture.astc";
			std::string tempOutputPath = "/tmp/temp_decompressed_texture.png";
			
			// 将压缩数据写入临时文件，需要添加ASTC头部
			FILE* file = fopen(tempInputPath.c_str(), "wb");
			if (!file)
			{
				return false;
			}
			
			// 创建ASTC头部
			u8 astcHeader[16] = {0};
			
			// 设置ASTC魔数
			astcHeader[0] = 0x13;
			astcHeader[1] = 0xAB;
			astcHeader[2] = 0xA1;
			astcHeader[3] = 0x5C;
			
			// 设置块大小
			u8 blockX = 4, blockY = 4;
			switch (src.format)
			{
				case tools::MTLPixelFormatASTC_6x6_LDR:
				case tools::MTLPixelFormatASTC_6x6_sRGB:
					blockX = blockY = 6;
					break;
				case tools::MTLPixelFormatASTC_8x8_LDR:
				case tools::MTLPixelFormatASTC_8x8_sRGB:
					blockX = blockY = 8;
					break;
			}
			astcHeader[4] = blockX;
			astcHeader[5] = blockY;
			astcHeader[6] = 1; // blockZ = 1 for 2D textures
			
			// 设置尺寸 (3字节 x, 3字节 y, 3字节 z)
			astcHeader[7] = src.width & 0xFF;
			astcHeader[8] = (src.width >> 8) & 0xFF;
			astcHeader[9] = (src.width >> 16) & 0xFF;
			astcHeader[10] = src.height & 0xFF;
			astcHeader[11] = (src.height >> 8) & 0xFF;
			astcHeader[12] = (src.height >> 16) & 0xFF;
			astcHeader[13] = 1; // z = 1 for 2D textures
			astcHeader[14] = 0;
			astcHeader[15] = 0;
			
			// 写入ASTC头部
			fwrite(astcHeader, 1, sizeof(astcHeader), file);
			
			// 写入压缩数据
			fwrite(src.pixels, 1, src.slicePitch, file);
			fclose(file);
			
			// 构建astcenc命令进行解压缩
			std::string blockSize;
			switch (src.format)
			{
				case tools::MTLPixelFormatASTC_4x4_LDR:
				case tools::MTLPixelFormatASTC_4x4_sRGB:
					blockSize = "4x4";
					break;
				case tools::MTLPixelFormatASTC_6x6_LDR:
				case tools::MTLPixelFormatASTC_6x6_sRGB:
					blockSize = "6x6";
					break;
				case tools::MTLPixelFormatASTC_8x8_LDR:
				case tools::MTLPixelFormatASTC_8x8_sRGB:
					blockSize = "8x8";
					break;
				default:
					return false; // 不支持的格式
			}
			
			// 构建命令行，添加sRGB标志如果需要
			std::string colorProfile = "";
			if (src.format == tools::MTLPixelFormatASTC_4x4_sRGB ||
				src.format == tools::MTLPixelFormatASTC_6x6_sRGB ||
				src.format == tools::MTLPixelFormatASTC_8x8_sRGB)
			{
				colorProfile = " -ds";
			}
			else
			{
				colorProfile = " -dl";
			}
			
			// astcenc解压缩命令
			std::string cmd = "/Users/zhanyuanwei/Desktop/GameEngine_VulkanCPP/astc-encoder/build/Source/astcenc-neon "  + colorProfile + " " + tempInputPath + " " + tempOutputPath;
			
			// 执行命令
			int result = system(cmd.c_str());
			if (result != 0)
			{
				// 命令执行失败
				remove(tempInputPath.c_str());
				return false;
			}
			
			// 读取解压缩后的图像
			CGImageSourceRef imageSource = CGImageSourceCreateWithURL(
				CFURLCreateWithFileSystemPath(kCFAllocatorDefault, 
					CFStringCreateWithCString(kCFAllocatorDefault, tempOutputPath.c_str(), kCFStringEncodingUTF8),
					kCFURLPOSIXPathStyle, false), NULL);
			
			if (!imageSource)
			{
				return false;
			}
			
			CGImageRef image = CGImageSourceCreateImageAtIndex(imageSource, 0, NULL);
			CFRelease(imageSource);
			
			if (!image)
			{
				return false;
			}
			
			// 获取图像属性
			size_t width = CGImageGetWidth(image);
			size_t height = CGImageGetHeight(image);
			
			// 设置目标图像属性
			dst.width = width;
			dst.height = height;
			dst.format = tools::MTLPixelFormatRGBA8Unorm; // 解压缩后为RGBA8格式
			dst.rowPitch = width * 4; // 4字节/像素
			dst.slicePitch = dst.rowPitch * height;
			
			// 创建位图上下文
			CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
			CGContextRef context = CGBitmapContextCreate(dst.pixels, width, height, 8, dst.rowPitch,
				colorSpace, kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
			
			CGColorSpaceRelease(colorSpace);
			
			if (!context)
			{
				CGImageRelease(image);
				return false;
			}
			
			// 绘制图像到位图上下文
			CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
			
			// 清理资源
			CGContextRelease(context);
			CGImageRelease(image);
			
			// 删除临时文件
			remove(tempInputPath.c_str());
			remove(tempOutputPath.c_str());
			
			return true;
		}

		/**
		 * @brief 解压缩单个Metal纹理
		 * @param image 源图像
		 * @param outScratch 输出的解压缩图像
		 * @return 是否解压缩成功
		 */
		bool decompress_metal(const tools::Image& image, tools::ScratchImage& outScratch) 
		{
			// 确定解压缩后的格式
			tools::MTLPixelFormat decompressedFormat = tools::MTLPixelFormatRGBA8Unorm;
			
			// 创建目标图像
			tools::Image destImg;
			destImg.width = image.width;
			destImg.height = image.height;
			destImg.format = decompressedFormat;
			destImg.rowPitch = image.width * GetBytesPerPixel(decompressedFormat);
			destImg.slicePitch = destImg.rowPitch * destImg.height;
			
			// 分配像素数据内存并初始化ScratchImage
			outScratch.pixels.resize(destImg.slicePitch);
			destImg.pixels = outScratch.pixels.data();
			outScratch.m_images.push_back(destImg);
			outScratch.m_nimages = 1;
			
			// 使用texturetool解压缩
			if (!decompress_with_astcenc(image, destImg))
			{
				// 如果texturetool失败，填充粉色作为错误指示
				return false;
			}
			
			return true;
		}
	} // anonymous namespace

	void ShutDownTextureTools()
	{
		metal_devices.clear();
	}

	EDITOR_INTERFACE void Decompress(texture_data *const data)
	{
		using namespace primal::content;
		assert(data->import_settings.compress);
		texture_info& info{ data->info };
		const tools::MTLPixelFormat format{ (tools::MTLPixelFormat)info.format };
		utl::vector<tools::Image> images{ subsource_data_to_images(data) };
		const bool is_3d{ (info.flags & texture_flags::is_volume_map) != 0 };

		TexMetadata metadata{};
		metadata.width = info.width;
		metadata.height = info.height;
		metadata.depth = is_3d ? info.array_size : 1;
		metadata.arraySize = is_3d ? 1: info.array_size;
		metadata.mipLevels = info.mip_levels;
		metadata.format = format;
		metadata.isSRGB = (info.flags & texture_flags::is_srgb)!= 0;
		metadata.dimension = is_3d ? MTL::TextureType3D : MTL::TextureType2D;
		metadata.hasAlpha = (info.flags & texture_flags::has_alpha) != 0;
		metadata.isPMAlpha = (info.flags & texture_flags::is_premultiplied_alpha) != 0;
		metadata.isSRGB = (info.flags & texture_flags::is_srgb) != 0;

		ScratchImage scratch;
		if (IsCompressed(format))
		{
			for(u32 i{ 0 }; i < images.size(); ++i)
			{
				// 解压缩单个图像
				if (decompress_metal(images[i], scratch))
				{
					copy_subresources(scratch, data);
					texture_info_from_metadata(scratch.metadata, info);
				}
				else
				{
					// 解压缩失败，填充粉色作为错误指示
					info.import_error = import_error::decompress;
				}
			}
		}
		else
		{
			// 如果不是压缩格式，直接复制原始数据
			scratch.metadata = metadata;
			scratch.m_nimages = images.size();
			scratch.m_images.resize(images.size());
			
			// 计算总大小
			size_t totalSize = 0;
			for (const auto& img : images)
			{
				totalSize += img.slicePitch;
			}
			
			// 分配内存并复制数据
			scratch.pixels.resize(totalSize);
			u8* destPtr = scratch.pixels.data();
			
			for (size_t i = 0; i < images.size(); ++i)
			{
				const tools::Image& srcImg = images[i];
				tools::Image& destImg = scratch.m_images[i];
				
				destImg.width = srcImg.width;
				destImg.height = srcImg.height;
				destImg.format = srcImg.format;
				destImg.rowPitch = srcImg.rowPitch;
				destImg.slicePitch = srcImg.slicePitch;
				destImg.pixels = destPtr;
				
				std::memcpy(destPtr, srcImg.pixels, srcImg.slicePitch);
				destPtr += srcImg.slicePitch;
			}
		}
	}

	EDITOR_INTERFACE void Import(texture_data *const data)
	{
		// return imported texture data that might be compressed using block compression
		if(1)
		{
			const texture_import_settings& settings{ data->import_settings };
			assert(settings.sources && settings.source_count);

			utl::vector<ScratchImage> scratch_images;
			utl::vector<tools::Image> images;

			u32 width{ 0 };
			u32 height{ 0 };
			tools::MTLPixelFormat format{ tools::MTLPixelFormatInvalid };
			utl::vector<std::string> files = split(settings.sources, ';');
			assert(files.size() == settings.source_count);

			for(u32 i{ 0 }; i < settings.source_count; ++i)
			{
				scratch_images.emplace_back(load_from_file(data, files[i].c_str()));
				if(data->info.import_error) return;

				const ScratchImage& scratch{ scratch_images.back() };
				const TexMetadata& metadata{ scratch.metadata };

				if(i == 0)
				{
					width = (u32)metadata.width;
					height = (u32)metadata.height;
					format = metadata.format;
				}

				// All image sources should have the same size
				if(width != metadata.width || height != metadata.height)
				{
					data->info.import_error = import_error::size_mismatch;
					return;
				}

				// All image sources should have the same format
				if(format != metadata.format)
				{
					data->info.import_error = import_error::format_mismatch;
					return;
				}
				
				const u32 array_size{ (u32)metadata.arraySize };
				const u32 depth{ (u32)metadata.depth };

				for(u32 array_index{ 0 }; array_index < array_size; ++array_index)
					for(u32 depth_index{ 0 }; depth_index < depth; ++depth_index)
					{
						const tools::Image* image{ &scratch.m_images[array_index * depth_index] };
						assert(image);

						if(!image)
						{
							data->info.import_error = import_error::unknown;
							return;
						}
						if(width != image->width || height != image->height)
						{
							data->info.import_error = import_error::size_mismatch;
							return;
						}
						images.emplace_back(*image);
					}
			}

			ScratchImage scratch{ initialize_from_images(data, images) };
			if(data->info.import_error) return;

			if(settings.compress)
			{
				ScratchImage bc_scratch{ compress_image_metal(data, scratch) };
				
				if (data->info.import_error) return;
			
				// 解压缩第一个图像用作图标
				copy_icon(bc_scratch.m_images[0], data);
				scratch = std::move(bc_scratch);
			}

			copy_subresources(scratch, data);
			texture_info_from_metadata(scratch.metadata, data->info);
		}
	}
}
#endif