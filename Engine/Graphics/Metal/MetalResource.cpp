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
        _mip_count = texture()->mipmapLevelCount();
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