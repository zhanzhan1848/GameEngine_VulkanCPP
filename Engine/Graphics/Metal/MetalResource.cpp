#include "MetalResource.h"

#include "MetalCore.h"
#include "MetalHelper.h"

namespace primal::graphics::metal
{
    namespace 
    {

    } // anonymous namespace
    
    ////	METAL BUFFER		///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	metal_buffer::metal_buffer(metal_buffer_init_info info, bool is_cpu_accessible)
	{
        MTL::Device* device = core::get_device();
		assert(!_buffer && info.size && info.alignment);
		_size = (u32)math::align_size_up(info.size, info.alignment);
		_buffer = device->newBuffer(_size, is_cpu_accessible ? MTL::ResourceStorageModeShared : MTL::ResourceStorageModePrivate);
		NAME_METAL_OBJECT_INDEXED(_buffer, _size, "METAL Buffer - size");
	}

	void metal_buffer::release()
	{
        core::deferred_release(_buffer);
		_size = 0;
	}

    ////	CONSTANT BUFFER		///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

	constant_buffer::constant_buffer(metal_buffer_init_info info)
        : _buffer{ info, true }
    {
        NAME_METAL_OBJECT_INDEXED(buffer(), size(), "Constant Buffer - size");

        _cpu_address = (u8*)_buffer.buffer()->contents();
        assert(_cpu_address);
    }

    u8 *const constant_buffer::allocate(u32 size)
    {
        std::lock_guard lock{ _mutex };
        const u32 aligned_size{ (u32)align_size_for_constant_buffer(size) };
        assert(_cpu_offset + aligned_size <= _buffer.size());
        if (_cpu_offset + aligned_size <= _buffer.size())
        {
            u8 *const address{ _cpu_address + _cpu_offset };
            _cpu_offset += aligned_size;
            return address;
        }

        return nullptr;
    }

    ////	METAL TEXTURE		///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    metal_texture::metal_texture(metal_texture_init_info info)
    {
        auto *const device{ core::get_device() };
        assert(device);

        MTL::ClearColor clear_color{ info.clear_value };

        if(info.resource)
        {
            assert(!info.heap);
            _texture = info.resource;
        }
        else if(info.heap)
        {
            // TODO: Allocate texture from heap
            assert(!info.resource);
        }
        else
        {
            assert(info.texture_desc);
            _texture = device->newTexture(info.texture_desc);
        }

        if(_texture)
        {
            const NS::UInteger width{ info.texture_desc->width() };
            const NS::UInteger height{ info.texture_desc->height() };
            const NS::UInteger array_size{ info.texture_desc->arrayLength() };
            
            // 根据像素格式计算每个像素的字节数
            u32 bytes_per_pixel{ 4 }; // 默认RGBA8格式
            MTL::PixelFormat pixel_format{ info.texture_desc->pixelFormat() };
            
            switch(pixel_format)
            {
                case MTL::PixelFormatRGBA16Float:
                    bytes_per_pixel = 8; // 4个通道 × 2字节
                    break;
                case MTL::PixelFormatDepth32Float:
                    bytes_per_pixel = 4; // 1个通道 × 4字节
                    break;
                case MTL::PixelFormatRGBA8Unorm:
                default:
                    bytes_per_pixel = 4; // 4个通道 × 1字节
                    break;
            }
            
            // 计算单个纹理层所需的内存大小
            const u64 bytes_per_row{ static_cast<u64>(width * bytes_per_pixel) };
            const u64 bytes_per_image{ static_cast<u64>(bytes_per_row * height) };
            
            // 使用动态内存分配而不是alloca，避免大纹理时的栈溢出
            std::unique_ptr<u8[]> texture_data{ std::make_unique<u8[]>(bytes_per_image) };
            u8* pTextureData = texture_data.get();
            
            // 根据像素格式填充纹理数据
            if(pixel_format == MTL::PixelFormatRGBA16Float)
            {
                // RGBA16Float格式：每个通道2字节的半精度浮点数
                u16* pData16 = reinterpret_cast<u16*>(pTextureData);
                for(u32 h{ 0 }; h < static_cast<u32>(height); ++h)
                {
                    for(u32 w{ 0 }; w < static_cast<u32>(width); ++w)
                    {
                        u64 i = h * width + w;
                        // 将浮点值转换为半精度浮点数（简化处理，直接使用浮点值的位表示）
                        pData16[i * 4 + 0] = static_cast<u16>(clear_color.red * 65535);
                        pData16[i * 4 + 1] = static_cast<u16>(clear_color.green * 65535);
                        pData16[i * 4 + 2] = static_cast<u16>(clear_color.blue * 65535);
                        pData16[i * 4 + 3] = static_cast<u16>(clear_color.alpha * 65535);
                    }
                }
            }
            else if(pixel_format == MTL::PixelFormatDepth32Float)
            {
                // Depth32Float格式：每个像素4字节的浮点深度值
                f32* pDataFloat = reinterpret_cast<f32*>(pTextureData);
                for(u32 h{ 0 }; h < static_cast<u32>(height); ++h)
                {
                    for(u32 w{ 0 }; w < static_cast<u32>(width); ++w)
                    {
                        u64 i = h * width + w;
                        pDataFloat[i] = static_cast<f32>(clear_color.red); // 深度值使用red通道
                    }
                }
            }
            else
            {
                // RGBA8Unorm格式：每个通道1字节的无符号整数
                for(u32 h{ 0 }; h < static_cast<u32>(height); ++h)
                {
                    for(u32 w{ 0 }; w < static_cast<u32>(width); ++w)
                    {
                        u64 i = h * width + w;
                        pTextureData[i * 4 + 0] = static_cast<u8>(clear_color.red * 255);
                        pTextureData[i * 4 + 1] = static_cast<u8>(clear_color.green * 255);
                        pTextureData[i * 4 + 2] = static_cast<u8>(clear_color.blue * 255);
                        pTextureData[i * 4 + 3] = static_cast<u8>(clear_color.alpha * 255);
                    }
                }
            }

            // 为每个数组层上传相同的纹理数据
            for (NS::UInteger z{ 0 }; z < array_size; ++z)
            {
                _texture->replaceRegion( 
                    MTL::Region( 0, 0, 0, width, height, 1 ), 
                    0, z, 
                    pTextureData, 
                    bytes_per_row, bytes_per_image 
                );
            }
        }

        // assert(_texture);
    }

    void metal_texture::release()
    {
        core::deferred_release(_texture);
    }

    ////	METAL RENDER TEXTURE		///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

    metal_render_texture::metal_render_texture(metal_texture_init_info info)
        : _texture{ info }
    {
    	assert(info.texture_desc);
        _mip_count = static_cast<u32>(texture()->mipmapLevelCount());
        assert(_mip_count && _mip_count <= metal_texture::max_mips);

        // auto *const device{ core::get_device() };
		// assert(device);

        // info.texture_desc->setUsage(MTL::TextureUsageRenderTarget | info.texture_desc->usage());
        // _texture = metal_texture{ info };
        // assert(_texture);
    }

    void metal_render_texture::release()
    {
        _texture.release();
        _mip_count = 0;
    }
}